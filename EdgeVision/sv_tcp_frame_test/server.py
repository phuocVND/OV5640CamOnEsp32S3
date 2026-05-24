"""
EdgeVision — TCP Frame Test Server
===================================
Port 9000: nhận JPEG frames từ ESP32, hiển thị bằng OpenCV
Port 9001: chấp nhận kết nối, gửi dummy angles (90°) định kỳ

Protocol (khớp với tcp_client.c trên ESP32):
  ESP → Server (port 9000):  [4 bytes length big-endian] [JPEG bytes]
  Server → ESP (port 9001):  [6 × float32 little-endian] = 24 bytes
"""

import socket
import struct
import threading
import time

import cv2
import numpy as np

# ─── Config ───────────────────────────────────────────────────────────────────
HOST        = "0.0.0.0"   # Lắng nghe tất cả interface
PORT_IMAGE  = 9000        # ESP gửi JPEG vào đây
PORT_CMD    = 9001        # ESP nhận servo angles từ đây

# Angles gửi về ESP (đơn vị độ, 0–180)
# Thay đổi để test servo
DUMMY_ANGLES = [90.0, 90.0, 90.0, 90.0, 90.0, 90.0]

# ─── Shared state ─────────────────────────────────────────────────────────────
latest_frame = None          # numpy array (BGR) của frame mới nhất
frame_lock   = threading.Lock()
frame_count  = 0
last_frame_time = None       # timestamp của frame cuối cùng
fps = 0.0                    # FPS hiện tại


# ══════════════════════════════════════════════════════════════════════════════
# Helpers
# ══════════════════════════════════════════════════════════════════════════════

def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Đọc đúng n bytes từ socket, xử lý partial reads."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed by peer")
        buf += chunk
    return buf


# ══════════════════════════════════════════════════════════════════════════════
# Thread: nhận JPEG frames (port 9000)
# ══════════════════════════════════════════════════════════════════════════════

def image_server():
    global latest_frame, frame_count, last_frame_time, fps

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_IMAGE))
    srv.listen(1)
    print(f"[image] Listening on {HOST}:{PORT_IMAGE} ...")

    while True:
        conn, addr = srv.accept()
        print(f"[image] ESP connected from {addr}")
        conn.settimeout(5.0)

        try:
            while True:
                # 1. Đọc 4 bytes length (big-endian uint32)
                raw_len = recv_exact(conn, 4)
                (jpg_len,) = struct.unpack(">I", raw_len)

                if jpg_len == 0 or jpg_len > 500_000:
                    print(f"[image] Bad frame length: {jpg_len}, skipping")
                    continue

                # 2. Đọc JPEG bytes
                jpg_data = recv_exact(conn, jpg_len)

                # 3. Decode JPEG → numpy array
                arr = np.frombuffer(jpg_data, dtype=np.uint8)
                img = cv2.imdecode(arr, cv2.IMREAD_COLOR)

                if img is None:
                    print("[image] cv2.imdecode failed")
                    continue

                # 4. Tính FPS
                now = time.time()
                if last_frame_time is not None:
                    delta = now - last_frame_time
                    if delta > 0:
                        fps = 0.9 * fps + 0.1 * (1.0 / delta)  # exponential moving average

                with frame_lock:
                    latest_frame = img
                    frame_count += 1
                    last_frame_time = now

                print(f"[image] Frame #{frame_count:05d}  size={jpg_len:6d} bytes  "
                      f"res={img.shape[1]}×{img.shape[0]}  "
                      f"ratio={100.0*jpg_len/(img.shape[0]*img.shape[1]*3):.1f}%  FPS={fps:.1f}")

        except (ConnectionError, socket.timeout, OSError) as e:
            print(f"[image] Disconnected: {e}")
        finally:
            conn.close()


# ══════════════════════════════════════════════════════════════════════════════
# Thread: gửi dummy servo angles (port 9001)
# ══════════════════════════════════════════════════════════════════════════════

def cmd_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT_CMD))
    srv.listen(1)
    print(f"[cmd]   Listening on {HOST}:{PORT_CMD} ...")

    while True:
        conn, addr = srv.accept()
        print(f"[cmd]   ESP connected from {addr}")

        try:
            while True:
                # Gửi 6 floats (little-endian) mỗi 500ms
                payload = struct.pack("<6f", *DUMMY_ANGLES)
                conn.sendall(payload)
                time.sleep(0.5)

        except (BrokenPipeError, OSError) as e:
            print(f"[cmd]   Disconnected: {e}")
        finally:
            conn.close()


# ══════════════════════════════════════════════════════════════════════════════
# Main: OpenCV display loop (phải chạy trên main thread)
# ══════════════════════════════════════════════════════════════════════════════

def main():
    # Spawn 2 server threads
    t_img = threading.Thread(target=image_server, daemon=True)
    t_cmd = threading.Thread(target=cmd_server,   daemon=True)
    t_img.start()
    t_cmd.start()

    print("\nServer started. Waiting for ESP32...")
    print("Press Q (in image window) or Ctrl+C to quit.\n")

    cv2.namedWindow("EdgeVision — Live", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("EdgeVision — Live", 640, 480)

    while True:
        with frame_lock:
            frame = latest_frame.copy() if latest_frame is not None else None
            current_fps = fps

        if frame is not None:
            # Overlay frame count và FPS
            cv2.putText(frame, f"Frame #{frame_count}", (10, 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
            cv2.putText(frame, f"FPS: {current_fps:.1f}", (10, 55),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 165, 255), 2)
            cv2.imshow("EdgeVision — Live", frame)
        else:
            # Màn hình chờ
            blank = np.zeros((240, 320, 3), dtype=np.uint8)
            cv2.putText(blank, "Waiting for ESP32...", (20, 120),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (100, 100, 255), 1)
            cv2.imshow("EdgeVision — Live", blank)

        key = cv2.waitKey(30) & 0xFF
        if key == ord('q') or key == 27:   # Q hoặc ESC
            break

    cv2.destroyAllWindows()
    print("Bye.")


if __name__ == "__main__":
    main()

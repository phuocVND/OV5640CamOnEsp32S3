# Giải Thích Chi Tiết app_config.h — EdgeVision

Tài liệu này giải thích **từng dòng config** trong `main/app_config.h` bằng **Tiếng Việt**.

---

## 📡 Phần 1: WiFi Configuration

### `#define WIFI_SSID "Ge cafe"`
- **Tên WiFi** mà ESP32 sẽ kết nối tới
- **"Ge cafe"** = tên router/hotspot của bạn
- Thay đổi thành tên WiFi thực tế của bạn trước khi flash

### `#define WIFI_PASSWORD "gexincamon"`
- **Mật khẩu WiFi** để xác thực
- Hỗ trợ WPA2-PSK (bảo mật chuẩn)
- Thay bằng mật khẩu WiFi của bạn

### `#define WIFI_MAX_RETRY 5`
- **Số lần reconnect tối đa** nếu WiFi bị mất
- Sau 5 lần thất bại → dừng cố gắng, chờ reset thủ công
- Nếu WiFi mất liên tục → kiểm tra router hoặc tín hiệu

---

## 🌐 Phần 2: TCP Server Configuration

### `#define TCP_SERVER_IP "192.168.1.77"`
- **IP của server chạy detection** (Python script server.py)
- ESP32 kết nối đến IP này để gửi JPEG + nhận lệnh servo
- **⚠️ QUAN TRỌNG:** Dùng **Static IP** (không DHCP)
  - Nếu server chuyển sang IP khác → phải update lại config này
- Cách tìm IP server: `ifconfig` hoặc `ipconfig getifaddr en0`

### `#define TCP_PORT_IMAGE 9000`
- **Port để ESP gửi JPEG frames** (ESP → Server)
- **Port này dành riêng cho stream hình**
- Flow: Camera capture → TCP send task → Server port 9000
- Server.py mở listener trên port 9000 để nhận ảnh

### `#define TCP_PORT_CMD 9001`
- **Port để ESP nhận lệnh servo** (Server → ESP)
- **Port này dành riêng cho servo commands**
- Server.py gửi 6 góc servo (24 bytes) qua port 9001
- Tách riêng 2 port → tránh xung đột, gửi + nhận đồng thời không block

### `#define TCP_CONNECT_TIMEOUT_MS 5000`
- **Timeout khi kết nối TCP** = 5 giây
- Nếu server không respond trong 5s → reconnect
- **Cân bằng:**
  - Quá ngắn (1s): fail trên mạng chậm
  - Quá dài (10s): WiFi hiện tượng hang lâu
  - 5s = tối ưu cho WiFi ở nhà/phòng

### `#define TCP_RECV_TIMEOUT_MS 3000`
- **Timeout khi chờ servo command** = 3 giây
- Được set vào socket option `SO_RCVTIMEO`
- Nếu server không gửi lệnh trong 3s → recv() trả error, task reconnect
- Servo vẫn giữ position cũ khi chờ command

---

## 📷 Phần 3: Camera Pins (OV5640 DVP)

### Tổng Quan DVP
- **DVP = Digital Video Port** — giao tiếp camera song song 8-bit
- 8 đường data (D0-D7) truyền 1 byte/pixel cùng lúc
- VSYNC, HREF, PCLK = tín hiệu điều khiển thời gian

### `#define CAM_PIN_PWDN 38`
- **Power Down pin** (GPIO 38)
- Active LOW: Khi PWDN=LOW → camera tắt (tiết kiệm power)
- Khi PWDN=HIGH → camera bật & hoạt động
- `camera_drv_init()` deassert PWDN (set HIGH) để enable camera

### `#define CAM_PIN_RESET 47`
- **Reset pin** (GPIO 47)
- Active LOW: Tạo pulse LOW → HIGH để reset sensor registers
- esp_camera_init() tự động handle reset, không cần code thêm
- Dùng để clear lỗi hoặc reset trạng thái camera

### `#define CAM_PIN_XCLK 15`
- **Master Clock output** (GPIO 15)
- ESP32 **phát ra 20MHz** clock cho OV5640
- OV5640 dùng clock này để **synchronize pixel generation**
- Sinh từ LEDC_TIMER_0 (LED PWM module được tái sử dụng)

### `#define CAM_PIN_SIOD 4` & `CAM_PIN_SIOC 5`
- **SCCB Bus** = I2C-compatible serial control bus
- **SIOD = SDA (data line), SIOC = SCL (clock line)**
- Dùng để **config OV5640 registers** (resolution, quality, etc)
- Tất cả setting OV5640 → gửi qua I2C bus này

### `#define CAM_PIN_D0 11` đến `CAM_PIN_D7 16`
- **8 đường data parallel (D0-D7)**
- D0 = LSB (Least Significant Bit), D7 = MSB (Most Significant Bit)
- Mỗi PCLK pulse → 1 byte pixel từ D0-D7 vào ESP32 DMA
- **⚠️ Thứ tự pin PHẢI ĐÚNG** — nếu lộn → ảnh méo

### `#define CAM_PIN_VSYNC 6`
- **Vertical Sync** — tín hiệu bắt đầu frame mới
- Camera kéo LOW → HIGH 1 lần/frame để mark boundary
- DMA reset khi detect VSYNC rising edge

### `#define CAM_PIN_HREF 7`
- **Horizontal Reference** — tín hiệu data valid/invalid
- HREF=LOW → data valid, có thể lấy D0-D7
- HREF=HIGH → blanking period (no data)
- Mỗi dòng scan có 1 HREF pulse

### `#define CAM_PIN_PCLK 13`
- **Pixel Clock** (GPIO 13)
- Mỗi rising edge → lấy 1 byte từ D0-D7
- DMA hardware trigger trên PCLK edge → capture frame tự động

---

## 🎥 Phần 4: Camera Capture Settings

### `#define CAM_XCLK_FREQ_HZ 20000000`
- **Tần số clock = 20 MHz** (20 triệu Hz)
- OV5640 spec: 6-24 MHz OK, nhưng 20MHz = chuẩn
- **20MHz = balance tốc độ + ổn định**
  - Thấp hơn 15MHz = chậm, FPS giảm
  - Cao hơn 24MHz = có thể unstable
- **Phát từ LEDC_TIMER_0** (được cấu hình ở servo_ctrl.c)

### `#define CAM_FRAME_SIZE FRAMESIZE_SVGA`
- **Độ phân giải output = SVGA (800×600)**
- **Vì sao SVGA?** Thay đổi từ UXGA (1600×1200) để tối ưu FPS
  - UXGA = 375KB/frame nén → 2-3 FPS over WiFi
  - SVGA = 50KB/frame nén → 8-10 FPS over WiFi
- **Các option khác:**
  - QVGA: 320×240 (rất nhỏ)
  - VGA: 640×480 (medium)
  - SXGA: 1280×1024 (lớn)
  - UXGA: 1600×1200 (quá lớn cho WiFi)

### `#define CAM_JPEG_QUALITY 10`
- **Chất lượng nén JPEG = 10** (scale 0-63)
- **Càng thấp = nén mạnh = file nhỏ + mất chất lượng**
  - 0-10: Rất nén mạnh (~25KB), ảnh hơi sáp
  - 20-30: Cân bằng (~60KB), chất lượng tốt
  - 50-63: Ít nén (~150KB), rất sắc nét
- **Hiện tại = 10** để gửi qua WiFi nhanh
- **Muốn ảnh sắc nét?** Tăng lên 20-25, chấp nhận FPS giảm

### `#define CAM_FB_COUNT 2`
- **Số frame buffer trong PSRAM = 2 cái**
- **2 × ~50KB (SVGA) = 100KB PSRAM dùng cho buffer**
- **Tại sao 2?**
  - Buffer 1: Camera đang capture frame mới
  - Buffer 2: TCP gửi frame cũ → **gửi + capture không block nhau**
- **Nếu tăng lên 3 hoặc 4:**
  - Pro: Mượt hơn nếu WiFi jitter
  - Con: Dùng nhiều PSRAM (4×50KB = 200KB)
  - Nhưng PSRAM chỉ 8MB → không sao, có chỗ
- **2 = minimum, optimal cho SVGA**

---

## 🦾 Phần 5: Servo Configuration

### Tổng Quan Servo
- **6 servo điều khiển 6-DOF robot arm**
- Mỗi servo = 1 PWM signal từ LEDC (50Hz, pulse 500-2500µs)
- Pulse width map: 500µs→0°, 1500µs→90°, 2500µs→180°

### `#define SERVO_PIN_0 1` đến `SERVO_PIN_5 39`
- **6 GPIO pins cho 6 servo (GPIO1, 2, 42, 41, 40, 39)**
- Mỗi pin → 1 servo:
  - GPIO1 = Base (xoay cơ sở)
  - GPIO2 = Shoulder (nâng cánh tay)
  - GPIO42 = Elbow (uốn cánh tay)
  - GPIO41 = Wrist Pitch (nghiêng cổ tay)
  - GPIO40 = Wrist Roll (xoay cổ tay)
  - GPIO39 = Gripper (mở/đóng claw)
- **⚠️ Các GPIO này PHẢI khác các pin camera D0-D7, VSYNC, HREF, PCLK**

### `#define SERVO_COUNT 6`
- **Tổng số servo = 6**
- `servo_ctrl_init()` loop khởi tạo 6 servo
- `servo_ctrl_set_all(angles[6])` cập nhật hết 6

---

## ⚡ Phần 6: Servo PWM (LEDC)

### `#define SERVO_LEDC_FREQ_HZ 50`
- **Tần số PWM = 50 Hz** (1 cycle = 20ms)
- **Standard servo = 50Hz**
- Deviation: 48-52Hz OK, nhưng <40Hz hoặc >60Hz → servo rung lắc
- Period = 1/50 = 20ms = 20000µs

### `#define SERVO_LEDC_RESOLUTION LEDC_TIMER_14_BIT`
- **14-bit resolution = 16384 steps** per 20ms period
- Step size = 20000µs / 16384 = **1.22µs precision**
- **Đủ precision để:**
  - 500µs = ~410 steps
  - 2500µs = ~2048 steps
  - Difference = 1638 steps (từ 0° đến 180°)

### `#define SERVO_PULSE_MIN_US 500` & `SERVO_PULSE_MAX_US 2500`
- **Pulse width range = 500-2500 µs (milliseconds)**
- 500µs = 0° (servo full left/down)
- 2500µs = 180° (servo full right/up)
- 1500µs = 90° (neutral/center)
- **Outside range:**
  - <500µs: servo stall, motor quay liên tục
  - >2500µs: servo stall, motor quay liên tục
  - Safe range protect servo từ overextend

### `#define SERVO_ANGLE_MIN 0.0f` & `SERVO_ANGLE_MAX 180.0f`
- **Góc command range = 0° đến 180°**
- `servo_ctrl_set_angle()` clamp ngoài range này
- Nếu server gửi -10° → clamp thành 0°
- Nếu server gửi 200° → clamp thành 180°

---

## 🧵 Phần 7: Task Configuration (FreeRTOS)

### Tổng Quan Task
- **4 tasks chạy song song:**
  1. `camera_task` — Capture frame
  2. `tcp_send_task` — Gửi JPEG
  3. `tcp_recv_task` — Nhận servo command
  4. `servo_task` — Control servo
- **2 cores:**
  - Core 0 (PRO_CPU) = WiFi + TCP stack
  - Core 1 (APP_CPU) = Camera + Servo (isolated)

### `#define TASK_CORE_CAMERA 1`
- **camera_task pinned to Core 1 (APP_CPU)**
- **Tại sao Core 1?**
  - DVP DMA chạy tốt nhất trên Core 1
  - Core 0 bận WiFi interrupt → Core 1 capture smooth
  - Result: 8fps ổn định, không bị WiFi hiện tượng

### `#define TASK_CORE_TCP_SEND 0` & `TASK_CORE_TCP_RECV 0`
- **TCP tasks pinned to Core 0 (PRO_CPU)**
- **Tại sao Core 0?**
  - lwIP WiFi stack chạy trên Core 0
  - Socket operations có affinity với WiFi core
  - TCP gửi/nhận nhanh hơn nếu cùng core với WiFi

### `#define TASK_CORE_SERVO 1`
- **servo_task pinned to Core 1 (APP_CPU)**
- **Tại sao Core 1?**
  - PWM timing critical → tránh interrupt từ WiFi
  - LEDC PWM precision cao hơn nếu tránh context switch
  - Servo movement smooth, không bị jitter từ WiFi

---

## 🎯 Priority (Ưu Tiên Task)

### `#define TASK_PRIO_CAMERA 5`
- **Priority = 5** (cao hơn TCP send)
- **Ý nghĩa:** Nếu camera_task và tcp_send_task cùng ready → camera chạy trước
- **Lý do:** Capture frame fresh → đợi chút để TCP gửi cái frame cũ
- Priority cao = capture không bị delay

### `#define TASK_PRIO_TCP_SEND 4` & `TASK_PRIO_TCP_RECV 4`
- **Priority = 4** (thấp hơn camera)
- **Cùng priority = race condition OK** (không có priority giữa send vs recv)
- **Lý do:** Stream secondary, camera primary

### `#define TASK_PRIO_SERVO 6`
- **Priority = 6 (HIGHEST!)**
- **Ý nghĩa:** servo_task preempt tất cả task khác nếu có servo command
- **Lý do:** PWM timing critical + robot arm phải respond ngay
- Nếu servo priority thấp → delay → arm lag response

### Priority Order:
```
6: servo_task (urgent)
5: camera_task (capture drive rate)
4: tcp_send_task (stream secondary)
4: tcp_recv_task (listen network)
```

---

## 💾 Stack Size (Bộ Nhớ Stack)

### `#define TASK_STACK_CAMERA 4096`
- **4 KB stack** cho camera_task
- **Đủ vì:** Chỉ gọi `camera_drv_capture()`, không lưu large buffers locally
- Frame pointer = 8 byte, không lớn

### `#define TASK_STACK_TCP_SEND 8192`
- **8 KB stack** (LỚN NHẤT!)
- **Tại sao?**
  - Socket operations (send_all loop) dùng bộ nhớ stack
  - TCP library functions sâu → nhiều stack frames
  - Frame pointer + temp variables → cần margin lớn
- Extra buffer an toàn cho TCP overhead

### `#define TASK_STACK_TCP_RECV 4096`
- **4 KB stack** cho tcp_recv_task
- **Đủ vì:** Nhận 24 byte servo command, xử lý đơn giản

### `#define TASK_STACK_SERVO 2048`
- **2 KB stack** (NHỎ NHẤT!)
- **Đủ vì:** Chỉ cập nhật servo angle, hàm servo_ctrl_set_all() gọi đơn giản

---

## 📦 Queue Depths (Độ Sâu Queue)

### `#define FRAME_QUEUE_DEPTH 2`
- **Số item queue có thể chứa = 2**
- **Mỗi item = frame pointer (8 bytes), KHÔNG copy JPEG (~50KB)**
- **Ring Buffer Logic:**
  - Camera push frame vào queue
  - Nếu queue full (2 item) → evict oldest frame → push newest
  - TCP pop frame từ queue, gửi, return buffer
  - Result: **Luôn gửi 2 frame MỚI NHẤT** → video mượt nếu WiFi jitter
- **PSRAM dùng:** 2 × ~50KB JPEG buffer (đã tính vào CAM_FB_COUNT)

### `#define CMD_QUEUE_DEPTH 1`
- **Servo command queue = depth 1 (PHẢI LÀ 1!)**
- **Tại sao 1?**
  - FreeRTOS `xQueueOverwrite()` **chỉ hoạt động với depth=1**
  - xQueueOverwrite: luôn **ghi đè** slot duy nhất với servo command mới
  - Nếu depth > 1 → assert fail
- **Behavior:**
  - Server gửi servo angles → overwrite cũ (always latest)
  - servo_task chỉ nhận latest command, ignore stale
  - Không bao giờ có queue "backed up" servo commands

---

## 🔧 Tóm Tắt Quick Reference

| Config | Value | Giải Thích |
|--------|-------|-----------|
| WIFI_SSID | "Ge cafe" | Tên WiFi cần kết nối |
| TCP_SERVER_IP | 192.168.1.77 | IP chạy Python server |
| TCP_PORT_IMAGE | 9000 | Port gửi JPEG |
| TCP_PORT_CMD | 9001 | Port nhận servo |
| CAM_FRAME_SIZE | FRAMESIZE_SVGA | 800×600 resolution |
| CAM_JPEG_QUALITY | 10 | Nén mạnh (~25KB) |
| CAM_FB_COUNT | 2 | 2 frame buffer = 100KB |
| SERVO pins | 1,2,42,41,40,39 | 6 GPIO cho 6 servo |
| SERVO_LEDC_FREQ_HZ | 50 | 50Hz PWM standard |
| SERVO_PULSE_MIN/MAX | 500-2500µs | 0-180° range |
| TASK_CORE_CAMERA | 1 | Core 1 (isolated) |
| TASK_CORE_TCP | 0 | Core 0 (WiFi) |
| TASK_PRIO_SERVO | 6 | Highest priority |
| FRAME_QUEUE_DEPTH | 2 | Ring buffer |
| CMD_QUEUE_DEPTH | 1 | xQueueOverwrite requirement |

---

## 🚀 Khi Nào Thay Đổi Config?

### WiFi / TCP
- **WiFi mất kết nối?** → Kiểm tra SSID/PASSWORD
- **Server IP thay đổi?** → Update TCP_SERVER_IP
- **TCP timeout thường xuyên?** → Tăng TCP_CONNECT_TIMEOUT_MS

### Camera
- **FPS quá chậm?** → Resolution đã SVGA rồi, quality có thể giảm xuống 5
- **Ảnh quá sáp/mờ?** → Tăng CAM_JPEG_QUALITY (20-30)
- **"Out of memory"?** → Giảm CAM_FB_COUNT (nhưng >2 không khuyến khích)

### Servo
- **Servo không vừa full range 0-180°?** → Adjust SERVO_PULSE_MIN/MAX
- **Servo rung lắc?** → Kiểm tra SERVO_LEDC_FREQ_HZ = 50Hz
- **Servo lag response?** → Đã priority 6 (max) rồi, check TCP delay

### Task/Queue
- **Task stack overflow?** → Tăng TASK_STACK_*
- **Frame queue loss?** → Đã depth 2 (ring buffer) rồi
- **Servo command lag?** → Đã CMD_QUEUE_DEPTH = 1 (xQueueOverwrite)

---

## 📚 Tài Liệu Liên Quan
- [ARCHITECTURE_GUIDE.md](ARCHITECTURE_GUIDE.md) — Giải thích kiến trúc toàn hệ thống
- [PSRAM_CAMERA_CONFIG_GUIDE.md](PSRAM_CAMERA_CONFIG_GUIDE.md) — Chi tiết PSRAM + camera
- [FIXBUG_PSRAM_ESP32S3.md](FIXBUG_PSRAM_ESP32S3.md) — Cách fix PSRAM trên ESP32-S3

---

**Tạo ngày:** May 24, 2026  
**Project:** EdgeVision — ESP32-S3 + OV5640 + 6-DOF Robot Arm

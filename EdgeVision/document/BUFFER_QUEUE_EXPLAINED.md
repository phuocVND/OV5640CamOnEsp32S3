# Giải Thích Chi Tiết: CAM_FB_COUNT vs FRAME_QUEUE_DEPTH vs CMD_QUEUE_DEPTH

---

## 🎯 Overview 3 Config

```
CAM_FB_COUNT = 2          ← Số frame buffer trong PSRAM (nơi lưu JPEG)
FRAME_QUEUE_DEPTH = 2     ← Số pointer có thể chứa trong frame queue
CMD_QUEUE_DEPTH = 1       ← Số servo command có thể chứa trong cmd queue
```

**Chúng là 3 thứ hoàn toàn khác nhau:**
- CAM_FB_COUNT = PSRAM memory allocation
- FRAME_QUEUE_DEPTH = FreeRTOS queue capacity (frame pointer)
- CMD_QUEUE_DEPTH = FreeRTOS queue capacity (servo command)

---

## 💾 Part 1: CAM_FB_COUNT = 2 (Frame Buffers in PSRAM)

### Ý Nghĩa
- **2 frame buffers** lưu trữ trong PSRAM
- Mỗi buffer = **~50KB** (SVGA JPEG compressed)
- **Tổng PSRAM = 2 × 50KB = 100KB**

### Memory Layout

```
PSRAM (8MB)
├─ 0x00000 - 0xC7FF   Buffer 0 (~50KB) ← Camera lưu frame 0 ở đây
├─ 0xC800 - 0x18FFF   Buffer 1 (~50KB) ← Camera lưu frame 1 ở đây
├─ 0x19000 - ...      [Còn lại của PSRAM]
└─ ...
```

### Cách Hoạt Động

```
Thời điểm T0: Camera capture frame mới
  ├─ Frame 0 (old) → Buffer 0
  └─ Frame 1 (new) → Buffer 1
                     ↓
Thời điểm T1: Camera capture tiếp
  ├─ Frame 0 (new) → Buffer 0  ← Ghi đè frame cũ
  └─ Frame 1 (old) → Buffer 1
```

### Ví Dụ Flow

```
T=0ms: Camera captures frame#1 → stores in Buffer 0
       Buffer 0: [Frame#1 JPEG data] (50KB)
       Buffer 1: [empty]

T=33ms: Camera captures frame#2 → stores in Buffer 1
       Buffer 0: [Frame#1 JPEG data]
       Buffer 1: [Frame#2 JPEG data]

T=66ms: Camera captures frame#3 → stores in Buffer 0 (overwrites Frame#1)
       Buffer 0: [Frame#3 JPEG data]  ← Frame#1 gone
       Buffer 1: [Frame#2 JPEG data]

T=99ms: Camera captures frame#4 → stores in Buffer 1 (overwrites Frame#2)
       Buffer 0: [Frame#3 JPEG data]
       Buffer 1: [Frame#4 JPEG data]  ← Frame#2 gone
```

### ⚠️ Quan Trọng: Buffers Không Phải Để Lưu Lâu

- **Buffers này được camera driver quản lý**
- **Chỉ lưu tạm thời** khi camera capture xong
- **TCP task phải return buffer** qua `camera_drv_fb_return(fb)`
  ```c
  camera_fb_t *fb = camera_drv_capture();  // Lấy buffer
  tcp_send_frame(fb);                      // Gửi JPEG
  camera_drv_fb_return(fb);                // Trả buffer cho camera
  ```
- Sau khi return → **buffer rảnh, camera có thể ghi đè frame mới**

---

## 📦 Part 2: FRAME_QUEUE_DEPTH = 2 (Frame Pointer Queue)

### Ý Nghĩa
- **FreeRTOS Queue** chứa **frame pointers** (không phải JPEG data!)
- Mỗi item = **8 bytes pointer** (địa chỉ PSRAM của frame)
- **Không copy JPEG** → chỉ pass pointer

### Queue Layout

```
Frame Queue (depth=2)
┌──────────────────────────────────────┐
│ [Slot 0] [Slot 1]                    │
│ pointer1  pointer2                    │
│ (8 bytes) (8 bytes)                  │
└──────────────────────────────────────┘
```

### Task Roles

```
camera_task:              tcp_send_task:
┌──────────────────┐      ┌──────────────────┐
│ Capture frame    │      │ Receive pointer  │
│ from Buffer      │      │ from Queue       │
│ Push pointer     │ ---> │ Get JPEG data    │
│ into Queue       │      │ Send to TCP      │
└──────────────────┘      │ Return buffer    │
                          │ to camera_drv    │
                          └──────────────────┘
```

### Ví Dụ Flow: Normal (WiFi OK)

```
T=0ms:
  camera_task: Capture frame#1 → Buffer 0
               xQueueSend(frame_queue, &Buffer0_ptr)
               Queue: [Buffer0_ptr] [empty]

T=10ms:
  tcp_send_task: Receive from queue
                 ptr = Queue[0] = Buffer0_ptr
                 Get JPEG from Buffer 0
                 Start TCP send (takes 50ms)
                 Queue: [empty] [empty]

T=33ms:
  camera_task: Capture frame#2 → Buffer 1
               xQueueSend(frame_queue, &Buffer1_ptr)
               Queue: [empty] [Buffer1_ptr]
               (TCP still sending frame#1)

T=60ms:
  tcp_send_task: Finish sending frame#1
                 camera_drv_fb_return(Buffer0_ptr)  ← Return buffer!
                 Receive from queue
                 ptr = Queue[0] = Buffer1_ptr
                 Get JPEG from Buffer 1
                 Start TCP send

T=66ms:
  camera_task: Capture frame#3 → Buffer 0  ← Can reuse Buffer0 now
               xQueueSend(frame_queue, &Buffer0_ptr)
               Queue: [Buffer0_ptr] [Buffer1_ptr]  ← FULL!
```

### Ví Dụ Flow: WiFi Slow (Buffer Full)

```
T=0ms:
  camera_task: Capture frame#1 → Buffer 0
               xQueueSend(frame_queue, &Buffer0_ptr)
               Queue: [Buffer0_ptr] [empty]

T=33ms:
  camera_task: Capture frame#2 → Buffer 1
               xQueueSend(frame_queue, &Buffer1_ptr)
               Queue: [Buffer0_ptr] [Buffer1_ptr]  ← NOW FULL

T=66ms:
  camera_task: Capture frame#3 → ???
               Queue is FULL! Can't push.
               
               RING BUFFER LOGIC:
               - xQueueReceive(frame_queue, &old_ptr)  ← Pop oldest
               - old_ptr = Buffer0_ptr (frame#1)
               - (Don't return it, just discard pointer)
               - xQueueSend(frame_queue, &Buffer0_ptr)  ← Push newest
               
               Queue: [Buffer0_ptr] [Buffer1_ptr]
               But Buffer0 now has frame#3 (overwritten frame#1)

T=100ms:
  tcp_send_task: Finally finish sending frame#1
                 camera_drv_fb_return(Buffer0_ptr)
                 BUT frame#3 is now in Buffer0!
                 Problem: We lost frame#2, kept frame#3
                 
                 Result: NEWEST frames (3,4,5...) always sent
                         OLDEST frames (1,2) discarded if WiFi slow
```

### Ring Buffer Strategy

```
Queue: [oldest] [newest]

If queue full when camera tries to push:
  1. Pop oldest → discard it (we don't return it)
  2. But the PSRAM buffer gets reused
  3. Push newest
  4. Result: Always keeps NEWEST 2 frames

Benefit: Video streaming stays FRESH (newest content)
         Even if WiFi is slow, viewer sees latest camera view
         Not frozen on old frame
```

---

## 🎮 Part 3: CMD_QUEUE_DEPTH = 1 (Servo Command Queue)

### Ý Nghĩa
- **Servo command queue** chứa **servo angles** (6 floats = 24 bytes)
- **Depth MUST = 1** (không thể thay đổi!)
- Dùng `xQueueOverwrite()` function (FreeRTOS requirement)

### Queue Layout

```
Cmd Queue (depth=1)
┌──────────────────────────────┐
│ [Slot 0]                     │
│ [6 servo angles]             │
│ [24 bytes]                   │
└──────────────────────────────┘
```

### Tại Sao Phải Depth=1?

**`xQueueOverwrite()` chỉ hoạt động nếu depth=1**

```c
// In tcp_recv_task (Server→ESP port 9001)
servo_cmd_t cmd = { ... };
xQueueOverwrite(cmd_queue, &cmd);  ← KHÔNG wait
```

**xQueueOverwrite behavior:**
- Nếu queue empty → put item
- Nếu queue full → GWRITE ĐÈ item cũ
- **LUÔN thành công**, không bao giờ block

```c
// Contrast: Normal xQueueSend
xQueueSend(cmd_queue, &cmd, portMAX_DELAY);  ← BLOCK nếu full
```

### Task Roles

```
tcp_recv_task:            servo_task:
┌──────────────────────┐  ┌──────────────────┐
│ Receive 24 bytes     │  │ Wait for command │
│ from port 9001       │  │ xQueueReceive()  │
│ xQueueOverwrite()    │  │ Get servo angles │
│ Always succeed       │  │ Set PWM          │
└──────────────────────┘  └──────────────────┘
```

### Ví Dụ Flow: Always Latest Command

```
T=0ms:
  Server sends: angles[6] = {45, 90, 135, 0, 90, 45}
  tcp_recv_task: xQueueOverwrite(cmd_queue, &angles)
                 Queue: [angles#1]

  servo_task: xQueueReceive() ← Get angles#1
              Set all 6 servos

T=50ms:
  Server sends: angles[6] = {90, 90, 90, 90, 90, 90}
  tcp_recv_task: xQueueOverwrite(cmd_queue, &angles)
                 Queue: [angles#2]  ← Overwrite angles#1

  servo_task: Still setting angles#1
              (servo_task slower than command arrival)

T=100ms:
  Server sends: angles[6] = {180, 0, 45, 135, 90, 50}
  tcp_recv_task: xQueueOverwrite(cmd_queue, &angles)
                 Queue: [angles#3]  ← Overwrite angles#2

T=150ms:
  servo_task: Finally done with angles#1
              xQueueReceive() ← Get angles#3 (latest!)
              Set all 6 servos
              
              angles#2 was NEVER executed!
              But that's OK — angles#3 is newer anyway
```

### Why Not Use Depth > 1?

```
❌ If CMD_QUEUE_DEPTH = 4:
   - Server sends quickly
   - tcp_recv_task queues angles#1, #2, #3, #4
   - servo_task takes time → keeps executing old commands
   - Arm moves in DELAYED stale positions
   - 200ms lag = arm far behind robot intention

✅ If CMD_QUEUE_DEPTH = 1:
   - Server sends angles#1
   - tcp_recv_task overwrites with angles#2
   - Overwrite with angles#3
   - servo_task only sees LATEST
   - ARM always responds to newest command
   - Real-time control!
```

---

## 🔄 All 3 Together: Complete Flow

### Scenario: Camera + Stream + Servo Control

```
PSRAM (8MB)
├─ Buffer 0 (50KB)  ← Camera alternates writing here
├─ Buffer 1 (50KB)  ← and here
└─ [Remaining PSRAM for other stuff]

FreeRTOS Queues:
├─ frame_queue: [pointer1] [pointer2]  ← depth=2, frame pointers
└─ cmd_queue:   [angles]               ← depth=1, servo command

Tasks:
├─ camera_task (Core 1, prio 5)
│  ├─ Capture JPEG → Buffer 0 or 1
│  ├─ Push pointer to frame_queue
│  └─ If full: evict oldest, push newest
│
├─ tcp_send_task (Core 0, prio 4)
│  ├─ Receive pointer from frame_queue
│  ├─ Get JPEG from PSRAM buffer
│  ├─ Send via TCP port 9000
│  └─ Return buffer to camera
│
├─ tcp_recv_task (Core 0, prio 4)
│  ├─ Listen port 9001
│  ├─ Receive 24-byte servo angles
│  └─ Overwrite cmd_queue (always latest)
│
└─ servo_task (Core 1, prio 6)
   ├─ Wait for cmd_queue
   ├─ Get latest angles
   └─ Set all 6 servo PWM
```

### Timeline Example

```
T=0ms:   cam: capture→Buffer0 → push ptr
T=33ms:  cam: capture→Buffer1 → push ptr
T=50ms:  send: recv ptr0 → get Buffer0 → send (takes 50ms)
T=66ms:  cam: capture→Buffer0 (overwrite!) → push ptr
T=70ms:  recv: get angles#1 → overwrite queue
T=100ms: send: done sending → return Buffer0 → recv ptr1 → send
T=120ms: recv: get angles#2 → overwrite queue
T=150ms: servo: recv angles#2 → set servos
T=180ms: recv: get angles#3 → overwrite queue
T=190ms: recv: send done → recv ptr2 → send
T=200ms: servo: recv angles#3 → set servos
```

---

## 📊 Memory & Timing Summary

| Item | Type | Count | Size | Purpose |
|------|------|-------|------|---------|
| **CAM_FB_COUNT** | PSRAM buffers | 2 | 50KB each = 100KB total | Lưu JPEG frames |
| **FRAME_QUEUE_DEPTH** | Queue slots | 2 | 8 bytes each (pointers) | Ring buffer pointers |
| **CMD_QUEUE_DEPTH** | Queue slots | 1 | 24 bytes (angles) | Latest servo command |

| Timing (SVGA @50KB JPEG over WiFi 54Mbps) |  |
|---|---|
| TCP send time | ~50ms per frame |
| Camera capture | ~33ms per frame (8fps = 30fps hardware) |
| Queue full scenario | Happens every 2 captures if TCP slow |
| Ring buffer eviction | Oldest frame discarded, newest kept |
| Servo command lag | <10ms (depth=1 = always latest) |

---

## 🎓 Key Insights

### 1. CAM_FB_COUNT ≠ Frame Queue Depth
```
CAM_FB_COUNT=2 → 2 PSRAM buffers (memory)
FRAME_QUEUE_DEPTH=2 → 2 queue slots (pointers to buffers)

They're independent!
- More buffers = more PSRAM (CAM_FB_COUNT=4 → 200KB)
- More queue = more pending frames (but we chose 2 for ring buffer)
```

### 2. Ring Buffer (FRAME_QUEUE_DEPTH=2) Benefits
```
✅ Keeps 2 NEWEST frames if WiFi jitter
✅ Discards old frames (not frozen on outdated content)
✅ Low PSRAM (only 100KB for 2 buffers)
✅ Suitable for streaming (user wants fresh view)
```

### 3. CMD_QUEUE_DEPTH=1 Requirement
```
❌ Cannot change to 2, 3, 4 — MUST be 1
❌ xQueueOverwrite() ONLY works with depth=1
✅ This is FreeRTOS API constraint
✅ Benefit: Always latest servo command, no stale queue
```

### 4. Flow Control: Who Waits?
```
camera_task:     If queue full → evicts oldest, pushes newest (NEVER blocks)
tcp_send_task:   If queue empty → blocks (waits for frame)
servo_task:      If queue empty → blocks (waits for command)
tcp_recv_task:   Overwrites (NEVER blocks)
```

---

## 🔧 Troubleshooting

### "Out of Memory" / PSRAM Error
```
Likely cause: CAM_FB_COUNT too high
Fix: Reduce CAM_FB_COUNT to 2
     Or reduce resolution (SVGA already optimal)
```

### Frame Drops / Frame Loss Visible in Video
```
Likely cause: Queue too small for WiFi jitter
Current setup (FRAME_QUEUE_DEPTH=2): 
     - Buffers 2 newest frames
     - Discards old ones = expected behavior
     
If you see stuttering: 
     - It's ring buffer eviction (expected with slow TCP)
     - Not a bug, strategy is "newest > oldest"
```

### Servo Lag / Delayed Arm Response
```
Unlikely because: CMD_QUEUE_DEPTH=1 (always latest)
Check: servo_task priority = 6 (should be highest)
       If other tasks blocking = servo response delayed
```

### Queue Assertion Failures
```
"assert failed: xQueueGenericSend queue.c:938"
     → Someone tried to use xQueueOverwrite on depth>1 queue
Fix: Ensure CMD_QUEUE_DEPTH = 1 (cannot change!)
```

---

## 📝 Key Takeaway

```
CAM_FB_COUNT   = Allocation (how many buffers in PSRAM)
FRAME_QUEUE_DEPTH = Queueing (how many pointers to buffer)
CMD_QUEUE_DEPTH   = Command (latest servo angles ONLY)

All 3 work TOGETHER:
1. Camera fills 2 buffers (CAM_FB_COUNT)
2. Frame pointers queued (FRAME_QUEUE_DEPTH) 
3. TCP sends, returns buffer
4. Camera reuses freed buffer
5. Ring buffer keeps newest 2 frames
6. Servo gets latest command (CMD_QUEUE_DEPTH=1)
```

---

**Created:** May 24, 2026  
**Project:** EdgeVision

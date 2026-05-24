# EdgeVision — Hướng dẫn kiến trúc hệ thống

> Tài liệu này giải thích **tại sao** code được tổ chức theo cách hiện tại,
> không chỉ là **cái gì**. Đọc từ trên xuống theo thứ tự để hiểu toàn bộ.

---

## Mục lục

1. [Bức tranh tổng thể](#1-bức-tranh-tổng-thể)
2. [Tại sao tách thành nhiều module?](#2-tại-sao-tách-thành-nhiều-module)
3. [ESP32-S3 có 2 core — dùng như thế nào?](#3-esp32-s3-có-2-core--dùng-như-thế-nào)
4. [FreeRTOS Tasks — thiết kế chi tiết](#4-freertos-tasks--thiết-kế-chi-tiết)
5. [Queue — cầu nối giữa các task](#5-queue--cầu-nối-giữa-các-task)
6. [TCP — tại sao dùng 2 port?](#6-tcp--tại-sao-dùng-2-port)
7. [Camera — PSRAM và frame buffer](#7-camera--psram-và-frame-buffer)
8. [Servo — LEDC PWM](#8-servo--ledc-pwm)
9. [WiFi — event-driven design](#9-wifi--event-driven-design)
10. [app_config.h — "single source of truth"](#10-app_configh--single-source-of-truth)
11. [Sơ đồ luồng dữ liệu đầy đủ](#11-sơ-đồ-luồng-dữ-liệu-đầy-đủ)

---

## 1. Bức tranh tổng thể

Hệ thống làm 4 việc cùng lúc liên tục:

```
[Camera OV5640]
      │  chụp JPEG 320×240
      ▼
[PSRAM frame buffer]
      │  g_frame_queue (pointer)
      ▼
[TCP port 9000] ──── gửi ảnh JPEG lên server
                           │
                           ▼
                    [Server Python]
                    phát hiện vật thể
                    tính góc servo
                           │
                    [TCP port 9001] ──── trả về 6 góc float
                           │
                      g_cmd_queue
                           │
                           ▼
                   [6 Servo LEDC PWM]
                   điều khiển cánh tay
```

Tất cả 4 việc này chạy song song, không chờ nhau — đó là lý do phải dùng
**FreeRTOS tasks** thay vì vòng lặp `while(1)` đơn giản trong `app_main`.

---

## 2. Tại sao tách thành nhiều module?

### Vấn đề nếu để hết trong `main.c`

Tưởng tượng file main.c có 1000 dòng chứa:
- Config pin camera + init camera + capture
- Init WiFi + event handler + connect
- TCP socket + send + recv
- LEDC timer + channel + servo control
- FreeRTOS task logic

Kết quả: **không thể đọc, không thể sửa, không thể tái sử dụng**.

### Nguyên tắc "Single Responsibility"

Mỗi file chỉ làm **một việc duy nhất**:

| File | Trách nhiệm duy nhất |
|------|---------------------|
| `camera/camera_drv.c` | Khởi tạo và capture frame từ OV5640 |
| `network/wifi_sta.c` | Kết nối WiFi STA + quản lý sự kiện |
| `network/tcp_client.c` | Gửi frame và nhận lệnh qua TCP |
| `servo/servo_ctrl.c` | Điều khiển 6 servo bằng LEDC PWM |
| `main.c` | Tạo queues + khởi tạo modules + spawn tasks |
| `app_config.h` | **Toàn bộ** config — chỉ 1 nơi duy nhất |

### Lợi ích thực tế

```
Muốn đổi camera từ OV5640 sang OV2640?
→ Chỉ sửa camera_drv.c, không động vào file nào khác.

Muốn đổi WiFi sang Ethernet?
→ Chỉ sửa wifi_sta.c (hoặc thêm eth_sta.c).

Muốn tích hợp Bluetooth thêm?
→ Thêm network/ble.c, không phá vỡ code cũ.

Muốn đổi pin servo?
→ Chỉ sửa SERVO_PIN_0..5 trong app_config.h.
```

---

## 3. ESP32-S3 có 2 core — dùng như thế nào?

### ESP32-S3 hardware

ESP32-S3 có 2 Xtensa LX7 cores:

```
Core 0 (PRO_CPU)                Core 1 (APP_CPU)
─────────────────                ────────────────
• WiFi stack (lwip)              • Người dùng tùy chọn
• WiFi interrupt handler         • Không bị ảnh hưởng WiFi
• esp_event_loop_default         • Cache miss từ camera DMA
• Mặc định của app_main          • Phù hợp real-time tasks
```

> **Quan trọng:** WiFi stack của ESP-IDF được hard-code để chạy trên Core 0.
> Nếu bạn gọi `send()` hay `recv()` từ Core 1, nó vẫn hoạt động, nhưng
> phải **cross-core interrupt** để vào lwip → thêm latency.

### Phân công trong EdgeVision

```
Core 0 (PRO_CPU)                Core 1 (APP_CPU)
─────────────────                ────────────────
tcp_send_task (prio 4)          camera_task  (prio 5)
tcp_recv_task (prio 4)          servo_task   (prio 6)
WiFi stack    (hệ thống)        DMA camera   (hardware)
```

**Lý do cụ thể:**

- `tcp_send_task` và `tcp_recv_task` → Core 0 vì `send()`/`recv()` gọi trực tiếp
  vào lwip đang chạy trên Core 0. Không phải cross-core → nhanh hơn.

- `camera_task` → Core 1 vì camera DMA tạo ra rất nhiều cache miss
  (dữ liệu từ PSRAM vào cache). Nếu để Core 0, cache miss này sẽ làm
  chậm WiFi interrupt handler → mất gói tin.

- `servo_task` → Core 1, priority **cao nhất** (6) trong app vì PWM timing
  quan trọng. Nếu servo_task bị delay 1-2ms, servo sẽ giật.

### Priority có ý nghĩa gì?

FreeRTOS scheduler: **task có priority cao hơn chạy trước**.

```
Priority 6: servo_task       ← ưu tiên cao nhất, ít bị block nhất
Priority 5: camera_task      ← cần chạy nhanh để không drop frame
Priority 4: tcp_send_task    ← có thể chờ 1-2ms, network có buffer
Priority 4: tcp_recv_task    ← tương tự
Priority 1: idle task        ← chạy khi không ai khác cần CPU
```

> Trên **cùng một core**, task cao priority sẽ preempt (chen ngang) task
> thấp priority. Trên **2 core khác nhau**, chúng chạy thật sự song song.

---

## 4. FreeRTOS Tasks — thiết kế chi tiết

### `camera_task` (Core 1, prio 5)

```c
while (1) {
    camera_fb_t *fb = camera_drv_capture();   // block cho đến khi có frame
    if (xQueueSend(g_frame_queue, &fb, 0) != pdTRUE) {
        camera_drv_fb_return(fb);             // queue đầy → DROP frame
    }
}
```

**Tại sao timeout = 0 (non-blocking)?**

Nếu dùng `xQueueSend(..., portMAX_DELAY)` (chờ mãi):
- TCP đang bận gửi frame cũ → camera_task bị block
- Camera DMA tiếp tục chạy, không có buffer → **overflow → crash**

Với timeout = 0:
- Nếu queue đầy → trả buffer ngay → camera_task tiếp tục vòng kế
- **Không bao giờ stall DMA** → hệ thống ổn định
- Trade-off: đôi khi drop frame — chấp nhận được vì đây là video stream, không phải transaction quan trọng

### `tcp_send_task` (Core 0, prio 4)

```c
while (1) {
    camera_fb_t *fb = NULL;
    xQueueReceive(g_frame_queue, &fb, portMAX_DELAY);  // chờ frame
    tcp_send_frame(fb);
    camera_drv_fb_return(fb);  // LUÔN LUÔN trả buffer dù send lỗi
}
```

**Tại sao `camera_drv_fb_return` luôn được gọi?**

Frame buffer được cấp phát trong PSRAM. ESP32-S3 chỉ có `CAM_FB_COUNT=2`
buffers. Nếu không trả về sau khi dùng → sau 2 frame → `esp_camera_fb_get()`
trả NULL → camera_task fail liên tục.

### `tcp_recv_task` (Core 0, prio 4)

```c
while (1) {
    servo_cmd_t cmd;
    if (tcp_recv_cmd(&cmd) == ESP_OK) {
        xQueueOverwrite(g_cmd_queue, &cmd);  // ghi đè lệnh cũ
    }
}
```

**`xQueueOverwrite` thay vì `xQueueSend`?**

Nếu server gửi lệnh nhanh hơn servo_task xử lý → queue đầy →
`xQueueSend` sẽ fail → **bỏ mất lệnh mới nhất**.

Với `xQueueOverwrite`: lệnh cũ trong queue bị ghi đè bởi lệnh mới nhất.
Servo luôn thực thi **lệnh hiện tại**, không thực thi lệnh đã lỗi thời.

> **cmd_queue depth = 4** (không phải 1) vì: đôi khi server gửi burst commands.
> Buffer 4 giúp servo không bỏ qua lệnh ngay cả khi có jitter network nhỏ.

### `servo_task` (Core 1, prio 6)

```c
while (1) {
    servo_cmd_t cmd;
    xQueueReceive(g_cmd_queue, &cmd, portMAX_DELAY);  // chờ lệnh
    servo_ctrl_set_all(cmd.angle);
}
```

Đơn giản nhất trong 4 tasks. Ưu tiên cao nhất vì sau khi nhận lệnh,
cần update PWM duty ngay, không để trễ.

---

## 5. Queue — cầu nối giữa các task

### Tại sao dùng Queue thay vì biến global?

```c
// CÁCH SAI: biến global
camera_fb_t *g_latest_frame;  // Race condition!

// camera_task:
g_latest_frame = fb;          // Core 1 ghi

// tcp_send_task:
send(g_latest_frame);         // Core 0 đọc -- có thể đọc giữa chừng khi Camera đang ghi!
```

Queue của FreeRTOS là **thread-safe**: có mutex bên trong, dùng được từ
nhiều task/core cùng lúc mà không cần lock thủ công.

### Truyền pointer vs truyền data

```
g_frame_queue: truyền camera_fb_t* (8 bytes)
g_cmd_queue:   truyền servo_cmd_t  (24 bytes = 6 × float)
```

`g_frame_queue` truyền **pointer** (địa chỉ) chứ không phải copy toàn bộ data.
Frame JPEG có thể 20-50KB — copy vào queue sẽ cực chậm. Thay vào đó:
- Camera viết JPEG vào PSRAM → trả về pointer
- Queue chỉ chuyển giao pointer (8 bytes)
- tcp_send_task đọc PSRAM qua pointer → gửi TCP
- Trả pointer về driver để giải phóng buffer

### Sơ đồ ownership của frame buffer

```
esp_camera_fb_get()
      │
      │  trả camera_fb_t* (trỏ vào PSRAM buffer)
      ▼
camera_task  ──── g_frame_queue ────► tcp_send_task
                 [truyền ownership]         │
                                            │ dùng xong
                                            ▼
                                   esp_camera_fb_return()
                                   [buffer trở về pool]
```

---

## 6. TCP — tại sao dùng 2 port?

### Phương án A: 1 TCP connection (BAD)

```
ESP  ──────────────────►  Server
     [JPEG frame]
     [chờ reply...]
                ◄──────── [servo angles]
     [JPEG frame]
     [chờ reply...]
```

Vấn đề: `send()` và `recv()` dùng chung socket.
- Phải `recv()` xong mới `send()` tiếp — **gọi là "stop-and-wait"**
- Nếu server xử lý chậm (inference model lâu), camera bị stall
- Không thể pipeline (gửi frame tiếp trong lúc server đang process frame trước)

### Phương án B: 2 TCP connections (GOOD) ← đang dùng

```
ESP  ──── TCP:9000 ────────────────────────────────►  Server
          [JPEG] [JPEG] [JPEG] [JPEG] ...               │
                                               detect + calculate
                                                         │
ESP  ◄──── TCP:9001 ──────────────────────────────────  Server
          ... [angles] [angles] [angles] ...
```

Lợi ích:
- `tcp_send_task` không bao giờ block chờ reply
- `tcp_recv_task` không ảnh hưởng tới việc gửi
- Server có thể trả lời **không đồng bộ** — không cần 1-1 với mỗi frame
- Nếu inference server chậm hơn camera capture → chỉ lệnh servo bị trễ,
  không làm chậm camera hoặc TCP send

### Giao thức truyền frame (TCP:9000)

```
┌─────────────────┬──────────────────────────────────┐
│ uint32_t len BE │        JPEG bytes (len bytes)     │
│   4 bytes       │                                   │
└─────────────────┴──────────────────────────────────┘
```

- `BE` = Big-Endian (network byte order chuẩn)
- Server đọc 4 bytes trước để biết cần đọc bao nhiêu bytes tiếp theo
- Tại sao cần length prefix? TCP là stream — không có "ranh giới" giữa các gói.
  Nếu không có length, server không biết 1 frame kết thúc ở đâu.

### Giao thức nhận lệnh (TCP:9001)

```
┌──────────────────────────────────────────────────────┐
│  float32[6] angles (24 bytes, little-endian)         │
│  [base, shoulder, elbow, wrist_pitch, wrist_roll, grip] │
└──────────────────────────────────────────────────────┘
```

- Fixed 24 bytes → không cần length prefix
- Server biết ESP cần đúng 6 góc → đơn giản hóa giao thức
- ESP clamp góc về [0.0, 180.0] để tránh servo quay quá giới hạn

### Auto-reconnect

```c
// Trong tcp_send_frame():
if (send_all(s_send_sock, ...) != ESP_OK) {
    close(s_send_sock);
    s_send_sock = -1;
    tcp_send_connect();  // tự kết nối lại
}
```

Nếu server restart, ESP tự kết nối lại — không cần reset ESP.

---

## 7. Camera — PSRAM và frame buffer

### Tại sao frame buffer phải ở PSRAM?

ESP32-S3 chỉ có ~512KB SRAM nội bộ. Frame JPEG 320×240 ≈ 10-30KB.
Với 2 buffers (`CAM_FB_COUNT=2`) → cần 20-60KB.

**Có thể dùng SRAM?** Về lý thuyết có, nhưng:
- 60KB / 512KB = 12% SRAM chỉ cho camera
- Không còn chỗ cho stack của 4 tasks + WiFi + lwip buffer
- → **Crash vì out of memory**

PSRAM (8MB external) → dư chỗ thoải mái cho frame buffers.

### `CAM_FB_COUNT=2` — tại sao 2 buffer?

```
Buffer A: Camera DMA đang ghi vào
Buffer B: tcp_send_task đang đọc để gửi

Khi A đầy: swap → A trở thành "ready to send", B trở thành "DMA đang ghi"
```

Với 1 buffer: camera phải dừng khi tcp_send đang đọc → frame rate giảm.
Với 2 buffers: camera và TCP hoạt động **hoàn toàn song song**.

### `CAMERA_GRAB_WHEN_EMPTY` — grab mode

```c
.grab_mode = CAMERA_GRAB_WHEN_EMPTY,
```

Hai chế độ:
- `GRAB_WHEN_EMPTY`: DMA capture frame mới **chỉ khi** buffer rảnh
- `GRAB_LATEST`: DMA **liên tục** capture, overwrite buffer cũ

Dùng `GRAB_WHEN_EMPTY` vì:
- Camera_task kiểm soát được khi nào lấy frame
- Không tốn bandwidth PSRAM khi tcp_send đang bận (không có ai đọc)
- Tiết kiệm điện hơn

### LEDC_TIMER_0 / LEDC_CHANNEL_0 cho XCLK

Camera OV5640 cần clock XCLK 20MHz từ ESP32-S3. ESP-IDF dùng LEDC
(LED Control) peripheral để tạo clock này:

```c
.ledc_timer   = LEDC_TIMER_0,
.ledc_channel = LEDC_CHANNEL_0,
```

→ Đây là lý do servo **không được dùng** TIMER_0 hoặc CHANNEL_0/1.

---

## 8. Servo — LEDC PWM

### Tại sao servo dùng LEDC thay vì RMT?

ESP32-S3 có 2 peripheral để tạo PWM:
- **LEDC**: đơn giản, 8 channels, ideal cho servo (50Hz cố định)
- **RMT**: phức tạp hơn, dùng cho protocols như WS2812 LED, IR, etc.

Cho servo MG996R/SG90 chuẩn, LEDC là lựa chọn tốt nhất.

### LEDC_TIMER_1 và LEDC_CHANNEL_2..7

```c
.timer_num = LEDC_TIMER_1,          // TIMER_0 dùng bởi camera XCLK
.channel   = LEDC_CHANNEL_2 + i,    // CH_0, CH_1 dùng bởi camera
```

LEDC có 4 timer (0-3) và 8 channels (0-7):
- Timer 0, Channel 0: Camera XCLK (đã chiếm)
- Timer 0, Channel 1: (dự phòng cho camera)
- **Timer 1, Channel 2-7: 6 servos** ← dùng ở đây

### Tính duty từ góc

```
Servo chuẩn: pulse 500µs = 0°, 2500µs = 180°
LEDC 14-bit: period = 20,000µs, steps = 16,384

                angle
pulse_µs = 500 + ───── × (2500 - 500)
                 180

         pulse_µs
duty = ────────── × 16,384
        20,000
```

Ví dụ tính với 90°:

```
pulse_µs = 500 + (90/180) × 2000 = 500 + 1000 = 1500µs
duty     = (1500 / 20000) × 16384 = 1228
```

Hàm `angle_to_duty()` trong `servo_ctrl.c` làm chính xác điều này.

### Khởi tạo 90° (neutral)

```c
.duty = angle_to_duty(90.0f),   // start at 90° neutral
```

Khi power on, tất cả servo về giữa (90°) trước khi nhận lệnh đầu tiên.
Tránh trường hợp servo quay đột ngột về 0° hoặc 180° gây hư gear.

---

## 9. WiFi — event-driven design

### Tại sao không dùng vòng lặp polling?

```c
// CÁCH SAI (polling):
while (!wifi_is_connected()) {
    vTaskDelay(100);  // waste CPU, block task
}
```

```c
// CÁCH ĐÚNG (event-driven):
xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, ...);
// Task này sleep hoàn toàn cho đến khi WiFi kết nối xong
// FreeRTOS scheduler dùng CPU cho tasks khác trong lúc này
```

### EventGroup

```c
EventGroupHandle_t g_wifi_event_group;

#define WIFI_CONNECTED_BIT  BIT0   // bit 0 = connected
#define WIFI_FAIL_BIT       BIT1   // bit 1 = failed permanently
```

`xEventGroupSetBits()` gọi từ wifi_event_handler (interrupt context) →
`xEventGroupWaitBits()` unblock trong `wifi_sta_wait_connected()` →
`app_main` tiếp tục spawn tasks.

Nếu WiFi fail sau `WIFI_MAX_RETRY=5` lần → `WIFI_FAIL_BIT` được set →
`wifi_sta_wait_connected()` trả `false` → `app_main` halt với log lỗi.

### NVS (Non-Volatile Storage)

```c
esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
}
```

WiFi stack của ESP-IDF lưu calibration data và WiFi credentials vào NVS
(flash partition). Phải init NVS trước khi init WiFi — đây là yêu cầu bắt buộc.

Xử lý 2 error codes:
- `NO_FREE_PAGES`: NVS partition đầy → erase và init lại
- `NEW_VERSION_FOUND`: firmware mới có NVS format khác → erase và init lại

---

## 10. `app_config.h` — "single source of truth"

### Vấn đề khi config nằm rải rác

```c
// camera_drv.c:
.xclk_freq_hz = 20000000,   // ← magic number

// servo_ctrl.c:
.freq_hz = 50,              // ← magic number
.duty_resolution = LEDC_TIMER_14_BIT,  // ← hard-coded

// wifi_sta.c:
.ssid = "my_wifi",          // ← hard-coded
```

Muốn đổi SSID → phải mở wifi_sta.c, tìm trong đống code.
Muốn đổi tần số XCLK → phải nhớ nó ở file nào.

### Giải pháp: `app_config.h`

```c
// Tất cả config trong 1 file:
#define WIFI_SSID         "your_ssid"
#define CAM_XCLK_FREQ_HZ  20000000
#define SERVO_LEDC_FREQ_HZ 50
```

Toàn bộ file `.c` chỉ `#include "app_config.h"` và dùng tên define.
Muốn thay đổi bất kỳ thứ gì → **chỉ sửa 1 file duy nhất**.

### Checklist khi triển khai mới

Mở `app_config.h` và điền các giá trị thực:

```c
#define WIFI_SSID           "TÊN_WIFI_CỦA_BẠN"
#define WIFI_PASSWORD       "MẬT_KHẨU_WIFI"
#define TCP_SERVER_IP       "IP_CỦA_SERVER_DETECTION"

// Servo pins theo sơ đồ board của bạn:
#define SERVO_PIN_0         1    // Base
#define SERVO_PIN_1         2    // Shoulder
// ...
```

---

## 11. Sơ đồ luồng dữ liệu đầy đủ

```
┌─────────────────────────────────────────────────────────────────────┐
│                         ESP32-S3 N16R8                              │
│                                                                     │
│  CORE 1 (APP_CPU)              │  CORE 0 (PRO_CPU)                  │
│  ─────────────────────         │  ────────────────────────          │
│                                │                                    │
│  ┌──────────────────┐          │  ┌──────────────────────┐          │
│  │   camera_task    │          │  │   tcp_send_task      │          │
│  │   prio = 5       │          │  │   prio = 4           │          │
│  │                  │          │  │                      │          │
│  │ esp_camera_fb_get│          │  │ xQueueReceive(       │          │
│  │   (blocks on DMA)│          │  │   g_frame_queue)     │          │
│  │        │         │          │  │        │             │          │
│  │        ▼         │          │  │        ▼             │          │
│  │  xQueueSend(     │          │  │  tcp_send_frame()    │          │
│  │   g_frame_queue, │──ptr──►  │  │  [4B len][JPEG...]   │──TCP:9000►│
│  │   timeout=0)     │          │  │        │             │          │
│  │  (non-blocking)  │          │  │        ▼             │          │
│  └──────────────────┘          │  │  camera_drv_fb_return│          │
│                                │  └──────────────────────┘          │
│  ┌──────────────────┐          │                                    │
│  │   servo_task     │          │  ┌──────────────────────┐          │
│  │   prio = 6       │          │  │   tcp_recv_task      │          │
│  │                  │          │  │   prio = 4           │          │
│  │ xQueueReceive(   │          │  │                      │          │
│  │   g_cmd_queue)   │◄─cmd──   │  │  tcp_recv_cmd()      │◄TCP:9001─│
│  │        │         │          │  │  [6× float32]        │          │
│  │        ▼         │          │  │        │             │          │
│  │ servo_ctrl_set_  │          │  │  xQueueOverwrite(    │          │
│  │   all(angles)    │          │  │   g_cmd_queue)       │          │
│  │        │         │          │  └──────────────────────┘          │
│  │        ▼         │          │                                    │
│  │ LEDC duty update │          │  [WiFi stack — lwip]               │
│  │ 6× GPIO PWM      │          │  [esp_event_loop]                  │
│  └──────────────────┘          │                                    │
│                                │                                    │
└────────────────────────────────┴────────────────────────────────────┘
          │                                    ▲
          │ PWM 50Hz                           │ TCP/IP
          ▼                                    │
  [6 Servo SG90/MG996R]              [Detection Server Python]
  [Robot arm 6-DOF]                  port 9000: recv JPEG
                                     port 9001: send angles
                                     (YOLO / OpenCV / etc.)
```

### Memory layout

```
SRAM (512KB nội bộ)              PSRAM (8MB external OPI)
───────────────────              ──────────────────────────
• Task stacks (4 tasks)          • Camera frame buffer × 2
• FreeRTOS kernel                • lwip TCP/IP buffers
• WiFi stack buffers             • Heap cho malloc lớn
• Queue data structures          • .text code (SPIRAM_FETCH)
• Global variables               • .rodata (SPIRAM_RODATA)
```

---

## Tóm tắt "why" cho từng quyết định

| Quyết định | Tại sao |
|---|---|
| Tách thành 4 module | Mỗi module độc lập, dễ thay thế, dễ test |
| `app_config.h` central | 1 nơi thay đổi config, không tìm trong nhiều file |
| camera_task → Core 1 | Tránh DMA cache miss làm chậm WiFi interrupt |
| tcp tasks → Core 0 | lwip chạy Core 0, tránh cross-core overhead |
| servo prio = 6 (cao nhất) | PWM timing nhạy cảm với latency |
| camera prio = 5 | Cần chạy đều để không drop frame |
| `g_frame_queue` depth = 1 | Luôn gửi frame mới nhất, không buffer stale |
| xQueueSend timeout = 0 | Camera không bao giờ stall, tránh DMA overflow |
| `g_cmd_queue` xQueueOverwrite | Servo luôn thực thi lệnh hiện tại, không lệnh cũ |
| 2 TCP connections | Send và recv độc lập, pipeline, không stop-and-wait |
| Length prefix trong frame | TCP là stream, cần biết ranh giới frame |
| 2 frame buffers (PSRAM) | Camera và TCP đọc song song không tranh nhau |
| LEDC_TIMER_1 cho servo | TIMER_0 đã dùng cho camera XCLK |
| LEDC_CHANNEL_2..7 cho servo | CH_0/1 đã dùng cho camera |
| Auto-reconnect TCP | Không cần reset ESP khi server restart |
| Event-driven WiFi | Không waste CPU polling, FreeRTOS sleep thật sự |

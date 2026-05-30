# Bug Report — EdgeVision Session 2026-05-24

Tài liệu này ghi lại tất cả các bug được phát hiện và sửa trong phiên làm việc ngày 24/05/2026.

---

## Bug #1 — FRAME_QUEUE_DEPTH vi phạm công thức buffer

### Triệu chứng
```
[CAM-STATS] captured=3960 null=0 evicted=882 queue=4/4
```
- Queue thường xuyên ở mức `4/4` (đầy hoàn toàn)  
- Stream bị treo ngắn 1–2 giây định kỳ  
- Eviction rate ~28% (882/3960 frames bị loại)

### Nguyên nhân
Người dùng đã đổi `FRAME_QUEUE_DEPTH` từ 2 lên 4, nhưng không tăng `CAM_FB_COUNT` tương ứng:

```
Công thức an toàn: CAM_FB_COUNT ≥ FRAME_QUEUE_DEPTH + 2

Trạng thái lỗi:
  CAM_FB_COUNT     = 4
  FRAME_QUEUE_DEPTH = 4  ← vi phạm!
  4 ≥ 4 + 2 = 6    → FALSE ❌
```

**Giải thích cơ chế:**  
Khi `queue = 4/4`:
- 4 buffers bị giữ trong `g_frame_queue`
- 1 buffer đang được `tcp_send_task` gửi đi
- Camera DMA muốn capture vào buffer mới → **không còn buffer trống**
- DMA stall → FPS giảm từ ~22fps xuống ~13fps → stream treo hình

### Fix
**File:** `main/app_config.h`
```c
// TRƯỚC (lỗi):
#define FRAME_QUEUE_DEPTH       4   // vi phạm công thức

// SAU (fixed):
#define FRAME_QUEUE_DEPTH       2   // thỏa: CAM_FB_COUNT(4) ≥ 2+2 ✅
```

---

## Bug #2 — CAM_JPEG_QUALITY quá cao gây drop rate 28%

### Triệu chứng
```
[SEND-STATS] sent=598 frames trong 40s = 14.8fps
[CAM-STATS]  captured=840 frames trong 40s = 20.7fps
Drop rate = (840-598)/840 = 28.8%
```

### Nguyên nhân
- `CAM_JPEG_QUALITY = 20` → mỗi frame ~90KB
- Bandwidth cần: 20fps × 90KB = **1.8MB/s**
- WiFi 2.4GHz overhead thực tế chỉ đạt ~1.2MB/s ổn định
- TCP queue backup → camera evict frames liên tục

### Fix
**File:** `main/app_config.h`
```c
// TRƯỚC:
#define CAM_JPEG_QUALITY    20   // ~90KB/frame, 1.8MB/s → WiFi bottleneck

// SAU:
#define CAM_JPEG_QUALITY    15   // ~60KB/frame, 1.2MB/s → WiFi handles ổn
```

**Kết quả kỳ vọng:** Drop rate giảm từ 28% xuống ~5–10%.

---

## Bug #3 — LCD GPIO conflict với Camera pins

### Triệu chứng
Recommendation ban đầu dùng:
- RES → **GPIO47** ❌
- DC  → **GPIO38** ❌

Đây là 2 pin đã bị chiếm bởi camera:
```c
#define CAM_PIN_RESET   47   // GPIO47 = Camera RESET
#define CAM_PIN_PWDN    38   // GPIO38 = Camera Power-Down
```

### Nguyên nhân
Khi tra cứu pinout board hình ảnh, không chú ý đến `app_config.h` đã map sẵn camera vào GPIO38 và GPIO47. Nếu cắm LCD vào các pin này sẽ gây:
- Camera bị reset liên tục (GPIO47 pulled LOW)
- Camera bị power-down (GPIO38 pulled LOW)
- Stream đứng, device treo

### Fix
**Wiring đúng** (dùng các GPIO thực sự rảnh):

| LCD Pin | GPIO Sai | GPIO Đúng | Ghi chú |
|---------|----------|-----------|---------|
| RES | ~~GPIO47~~ | **GPIO3** | JTAG_EN, OK sau boot |
| DC  | ~~GPIO38~~ | **GPIO19** | USB OTG D+, không dùng |
| CS  | GPIO3 | **GPIO20** | USB OTG D-, không dùng |
| SCL | GPIO14 ✅ | **GPIO14** | Không đổi |
| SDA | GPIO21 ✅ | **GPIO21** | Không đổi |
| BLK | 3.3V ✅ | **3.3V** | Không đổi |

**Danh sách tất cả GPIO đã bị chiếm:**
```
Camera DVP: GPIO 4,5,6,7,8,9,10,11,12,13,15,16,17,18,38,47
Servo LEDC: GPIO 1,2,39,40,41,42
PSRAM:      GPIO 35,36,37 (internal)
Serial:     GPIO 43 (TX), 44 (RX)
→ GPIO rảnh an toàn: 3, 14, 19, 20, 21, 45
```

---

## Bug #4 — CMakeLists.txt explicit REQUIRES phá vỡ auto-resolve

### Triệu chứng
```
fatal error: esp_wifi.h: No such file or directory
compilation terminated.
```

### Nguyên nhân
Thêm `REQUIRES driver esp_lcd` vào `idf_component_register` của component `main`:

```cmake
# TRƯỚC (lỗi):
idf_component_register(
    ...
    REQUIRES driver esp_lcd   ← khi explicit REQUIRES được khai báo,
)                               chỉ các component này và dependencies của chúng
                                mới được resolve. esp_wifi KHÔNG phải dependency
                                của driver hoặc esp_lcd → header không tìm thấy.
```

Trong ESP-IDF, component `main` có đặc quyền đặc biệt: nếu KHÔNG có `REQUIRES`, nó tự động nhìn thấy **tất cả** component. Nhưng khi khai báo `REQUIRES` explicit, quy tắc đó bị override.

### Fix
**File:** `main/CMakeLists.txt`
```cmake
# SAU (fixed): xóa REQUIRES hoàn toàn
idf_component_register(
    SRCS
        "main.c"
        "lcd/lcd_drv.c"
        ...
    INCLUDE_DIRS
        "."
        "lcd"
        ...
    LDFRAGMENTS
        "psram_iram.lf")
    # Không có REQUIRES → main tự resolve tất cả components ✅
```

---

## Bug #5 — Race condition trong LCD JPEG buffer → WDT crash

### Triệu chứng
```
E (14446) JPEG: esp_jpeg_decode(114): Error in decoding JPEG image! 2
E (14446) JPEG: esp_jpeg_decode(114): Error in decoding JPEG image! 6
E (16946) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (16946) task_wdt:  - IDLE1 (CPU 1)
E (16946) task_wdt: Tasks currently running:
E (16946) task_wdt: CPU 1: lcd
Backtrace: ... jd_decomp in ROM ... esp_jpeg_decode ... lcd_task
```

Lỗi JPEG:
- Error code **2** = `JDR_INP` (Input stream error — dữ liệu bị cắt đứt giữa chừng)
- Error code **6** = `JDR_FMT1` (Data format error — dữ liệu bị hỏng format)

### Nguyên nhân — Race Condition Chi Tiết

Code ban đầu trong `lcd_task` giải phóng mutex **trước khi decode**:

```c
// CODE LỖI:
xSemaphoreTake(g_lcd_mutex, portMAX_DELAY);
size_t len = g_lcd_jpeg_len;
xSemaphoreGive(g_lcd_mutex);       // ← GIẢI PHÓNG MUTEX SỚM QUÁ!

// ↓ Camera_task có thể overwrite g_lcd_jpeg_buf ngay đây!

esp_jpeg_decode(g_lcd_jpeg_buf);   // ← Đọc buffer đang bị ghi đè!
```

**Timeline race condition:**

```
camera_task (Core 1, prio 5)         lcd_task (Core 1, prio 3)
──────────────────────────────────   ──────────────────────────
Take(mutex, 0) → SUCCESS
memcpy(JPEG → g_lcd_jpeg_buf)
g_lcd_jpeg_len = len
Give(mutex)
Give(sem) ────────────────────────→ Take(sem) → wakes up
                                      Take(mutex) → SUCCESS
                                      len = g_lcd_jpeg_len
                                      Give(mutex)  ← MUTEX FREE!
                                           ↓
Take(mutex, 0) → SUCCESS  ←── mutex free ─╯
memcpy(JPEG_mới → g_lcd_jpeg_buf)  ↓  esp_jpeg_decode(g_lcd_jpeg_buf)
    ↑ ĐANG GHI ĐÈ!                 ↑  ĐANG ĐỌC!
    ↑──────── DATA RACE ────────────↑
```

**Hậu quả:**
1. JPEG bị corrupt giữa chừng
2. `jd_decomp` trong ROM nhận corrupt data
3. ROM decoder không thể yield (không có FreeRTOS hook bên trong ROM)
4. ROM function loop rất lâu (>5 giây) parsing corrupt data
5. IDLE1 không nhận được CPU time → WDT trigger

### Fix
**File:** `main/main.c` — Giữ mutex trong suốt quá trình decode:

```c
// CODE ĐÃ FIX:
xSemaphoreTake(g_lcd_mutex, portMAX_DELAY);
size_t len = g_lcd_jpeg_len;

if (len == 0 || len > LCD_JPEG_BUF_SIZE) {
    xSemaphoreGive(g_lcd_mutex);
    continue;
}

esp_jpeg_decode(g_lcd_jpeg_buf);  // decode TRONG KHI giữ mutex

xSemaphoreGive(g_lcd_mutex);      // ← Chỉ giải phóng SAU khi decode xong
```

**Tại sao không bị deadlock:**  
`camera_task` dùng `xSemaphoreTake(g_lcd_mutex, 0)` — **non-blocking**. Nếu `lcd_task` đang giữ mutex, `camera_task` tức thời `fail` và **bỏ qua** LCD copy lần đó (không block). Decode chỉ mất ~15ms nên camera bỏ qua LCD copy khoảng 10% số lần → hoàn toàn chấp nhận được cho mục đích display.

**Timeline sau fix:**
```
camera_task                          lcd_task
──────────────────────────────────   ──────────────────────────
                                      Take(mutex) → GIỮA DECODE
Take(mutex, 0) → FAIL ✅             (decode đang chạy)
bỏ qua copy lần này — OK             ...
                                      Give(mutex) → XONG decode
Take(mutex, 0) → SUCCESS (lần sau)   Take(sem) → chờ frame tiếp
copy JPEG mới
Give(mutex)
```

---

## Tổng kết

| # | Bug | Loại | Ảnh hưởng | File |
|---|-----|------|-----------|------|
| 1 | FRAME_QUEUE_DEPTH=4 vi phạm CAM_FB_COUNT≥DEPTH+2 | Config error | Stream treo 1-2s định kỳ | `app_config.h` |
| 2 | CAM_JPEG_QUALITY=20 gây drop 28% | Config tuning | Nhiều frame bị drop | `app_config.h` |
| 3 | LCD GPIO38/47 conflict với camera | Wiring error | Camera crash nếu cắm sai | Wiring hardware |
| 4 | explicit REQUIRES phá auto-resolve | Build system | Build fail: esp_wifi.h not found | `CMakeLists.txt` |
| 5 | Race condition JPEG buffer → WDT | Concurrency bug | Device crash mỗi 5-10s | `main.c` |

### Lesson Learned

**Về buffer management:**
> Công thức bắt buộc: `CAM_FB_COUNT ≥ FRAME_QUEUE_DEPTH + 2`  
> (+1 cho camera DMA đang capture, +1 cho tcp_send_task đang giữ khi gửi)

**Về ESP-IDF CMake:**
> Component `main` **không nên** khai báo `REQUIRES` explicit trừ khi thực sự cần override. Explicit REQUIRES thay thế (không cộng thêm) auto-resolve.

**Về mutex và long-running operations:**
> Mutex phải bảo vệ **toàn bộ thời gian** tài nguyên được sử dụng, không chỉ thời gian đọc metadata.  
> Khi consumer giữ mutex lâu (decode ~15ms), producer dùng `Take(mutex, 0)` non-blocking để skip — không bao giờ dùng blocking wait từ producer.

**Về ROM functions và WDT:**
> Các function trong ROM (như `jd_decomp`) không có FreeRTOS yield point. Nếu gọi với corrupt data, chúng có thể loop vô hạn mà không nhường CPU → WDT crash. Phải đảm bảo data hợp lệ **trước khi gọi** ROM function.

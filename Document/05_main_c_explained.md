# main.c — Giải thích từng dòng code

> **Đọc trước**: Các file 00-04 để hiểu nền tảng

---

## Cấu trúc tổng thể của main.c

```
#include ...              ← Khai báo header cần dùng
#define CAM_PIN_xxx ...   ← Định nghĩa chân GPIO của camera
static void capture_frame_task() ← Task FreeRTOS chụp ảnh liên tục
void app_main()           ← Entry point — chạy đầu tiên khi boot
```

---

## Phần 1: Headers (#include)

```c
#include <stdio.h>              // printf(), fflush()
#include "freertos/FreeRTOS.h"  // FreeRTOS kernel
#include "freertos/task.h"      // xTaskCreate(), vTaskDelay()
#include "driver/gpio.h"        // gpio_config(), gpio_set_level()
#include "esp_log.h"            // ESP_LOGI(), ESP_LOGE(), ESP_LOGW()
#include "esp_camera.h"         // esp_camera_init(), esp_camera_fb_get()
#include "esp_heap_caps.h"      // heap_caps_malloc(), heap_caps_get_free_size()
#include "esp_psram.h"          // esp_psram_is_initialized()
#include "sdkconfig.h"          // CONFIG_SPIRAM, CONFIG_CAMERA_PSRAM_DMA ...
```

**Tại sao có 2 loại include?**
- `<stdio.h>`: Dùng dấu `<>` → tìm trong system include path (C standard library)
- `"freertos/FreeRTOS.h"`: Dùng `""` → tìm trong project include paths (ESP-IDF components)

**`sdkconfig.h`** là file **tự sinh** từ sdkconfig, chứa tất cả `#define CONFIG_xxx`.
Khi bạn viết `#ifdef CONFIG_SPIRAM`, preprocessor đọc từ file này.
File này ở `build/config/sdkconfig.h` — đừng sửa thủ công.

---

## Phần 2: Camera Pin Definitions

```c
#define CAM_PIN_PWDN  38  // Power Down
#define CAM_PIN_RESET 47  // Hardware Reset
#define CAM_PIN_XCLK  15  // External Clock output → cấp clock cho sensor
#define CAM_PIN_SIOD   4  // SCCB Data (giống I2C SDA)
#define CAM_PIN_SIOC   5  // SCCB Clock (giống I2C SCL)

// 8-bit parallel data bus (DVP interface)
#define CAM_PIN_D7 16   // MSB
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2  8
#define CAM_PIN_D1  9
#define CAM_PIN_D0 11   // LSB

#define CAM_PIN_VSYNC  6  // Vertical Sync — bắt đầu/kết thúc frame
#define CAM_PIN_HREF   7  // Horizontal Reference — bắt đầu/kết thúc dòng
#define CAM_PIN_PCLK  13  // Pixel Clock — mỗi xung = 1 pixel data
```

### Giao thức DVP (Digital Video Port) là gì?

Camera OV5640 dùng giao thức DVP — giao thức truyền ảnh song song 8-bit:

```
                OV5640 Sensor
XCLK  ──────→  [Clock Input]    ESP cấp clock cho sensor
SIOC  ←──────  [I2C/SCCB]       ESP gửi lệnh config tới sensor
SIOD  ←──────  [I2C/SCCB]
VSYNC ←──────  [Frame sync]     Sensor báo ESP: "frame mới bắt đầu"
HREF  ←──────  [Line sync]      Sensor báo ESP: "dòng mới bắt đầu"
PCLK  ←──────  [Pixel clock]    Mỗi xung clock = 1 byte pixel data
D0-D7 ←──────  [8-bit data]     8 bits pixel data song song
PWDN  ──────→  [Power Down]     HIGH = tắt sensor, LOW = bật sensor
RESET ──────→  [Reset]          LOW pulse = reset sensor
```

**Tại sao PWDN=38 và đây là GPIO LED?**
Trên ESP32-S3-DevKitC v1.1, GPIO38 kết nối với LED. Nhưng trong project này GPIO38
được dùng cho camera PWDN. Nếu bạn dùng DevKit thật, phải đổi sang GPIO khác hoặc
tháo LED.

---

## Phần 3: capture_frame_task()

```c
static void capture_frame_task(void *arg)
{
    ESP_LOGI(TAG, "Starting frame capture task");
    
    while (1) {
        // 1. Lấy frame từ camera driver
        camera_fb_t *fb = esp_camera_fb_get();
        
        if (!fb) {
            ESP_LOGE(TAG, "Frame buffer could not be acquired");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        
        // 2. Log thông tin frame
        ESP_LOGI(TAG, "Captured frame: %d bytes, size: %dx%d", 
                 fb->len, fb->width, fb->height);
        
        // 3. Kiểm tra frame buffer có ở PSRAM không
        if (heap_caps_check_integrity_addr((intptr_t)fb->buf, false)) {
            uint32_t caps = heap_caps_get_allocated_size((void *)fb->buf);
            if (heap_caps_check_integrity_addr((intptr_t)fb->buf, true)) {
                ESP_LOGI(TAG, "Frame buffer in PSRAM (size: %lu bytes)", caps);
            }
        }
        
        // 4. Trả frame buffer về để dùng lại
        esp_camera_fb_return(fb);
        
        // 5. Chờ 2 giây rồi chụp tiếp
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
```

### `esp_camera_fb_get()` — Cơ chế frame buffer

Camera driver quản lý một **pool** (bể) các frame buffer:

```
Camera Hardware → DMA → Frame Buffer Pool (trong PSRAM)
                                │
                    esp_camera_fb_get() → Trả về pointer đến frame ready
                                │
                    [Code bạn xử lý frame]
                                │
                    esp_camera_fb_return() → Trả buffer về pool để reuse
```

**`grab_mode = CAMERA_GRAB_WHEN_EMPTY`** trong config:
- Chỉ chụp frame mới khi pool đang trống (buffer đã được return)
- Tránh tràn bộ nhớ
- Ngược lại: `CAMERA_GRAB_LATEST` = luôn chụp frame mới nhất, bỏ qua frame cũ

**Lỗi khi quên `esp_camera_fb_return()`**:
- Frame buffer không được trả về pool
- Sau vài lần gọi `fb_get()` → pool hết buffer → `fb_get()` trả về NULL
- Camera dừng chụp ảnh

### `heap_caps_check_integrity_addr()` — Verify frame trong PSRAM

```c
// Kiểm tra địa chỉ fb->buf có thuộc về heap hợp lệ không
if (heap_caps_check_integrity_addr((intptr_t)fb->buf, false))
```

`(intptr_t)fb->buf` = ép kiểu pointer → integer để truyền vào hàm.

Địa chỉ PSRAM trên ESP32-S3 thường bắt đầu từ `0x3C000000`.
Địa chỉ SRAM nội thường bắt đầu từ `0x3FC80000`.
Bạn có thể kiểm tra thủ công: `if ((uint32_t)fb->buf >= 0x3C000000) { /* PSRAM */ }`

### `vTaskDelay(2000 / portTICK_PERIOD_MS)`

```c
// portTICK_PERIOD_MS = 10 (vì CONFIG_FREERTOS_HZ=100, 1 tick = 10ms)
// 2000 / 10 = 200 ticks = 2 giây

// Cách viết tốt hơn (rõ ràng hơn):
vTaskDelay(pdMS_TO_TICKS(2000));  // pdMS_TO_TICKS = macro chuyển ms → ticks
```

**Tại sao phải delay?** Nếu không delay, task chạy liên tục 100% CPU → task khác
không có cơ hội chạy → hệ thống bị treo (FreeRTOS cooperative scheduling).

---

## Phần 4: app_main()

### 4a. Khởi động và PSRAM check

```c
void app_main(void)
{
    printf("\n\n========== APP MAIN STARTED ==========\n\n");
    fflush(stdout);                        // Force flush output buffer
    vTaskDelay(500 / portTICK_PERIOD_MS);  // Chờ 500ms để UART ổn định
```

`fflush(stdout)` quan trọng vì UART output có buffer. Nếu không flush, printf có
thể không hiện ra ngay. Đặc biệt quan trọng trước các bước có thể crash.

```c
    #ifdef CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "PSRAM OK - Total: %lu bytes, Free: %lu bytes",
                 (unsigned long)psram_total, (unsigned long)psram_free);
    } else {
        ESP_LOGE(TAG, "PSRAM NOT initialized!");
    }
    #else
    ESP_LOGW(TAG, "PSRAM is DISABLED in config");
    #endif
```

**`#ifdef CONFIG_SPIRAM`**: Code trong block này chỉ compile khi `CONFIG_SPIRAM=y`
được set trong sdkconfig. Nếu PSRAM tắt, đoạn code này bị compiler bỏ qua hoàn toàn.

**`(unsigned long)psram_total`**: Cast về `unsigned long` để tương thích với `%lu`
format. Trên các platform khác nhau, `size_t` có thể là 32 hoặc 64 bit.

### 4b. Khởi tạo GPIO cho Camera PWDN

```c
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CAM_PIN_PWDN),  // GPIO 38
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(CAM_PIN_PWDN, 1);  // HIGH = tắt sensor (active low PWDN)
```

`(1ULL << 38)` = bit mask cho GPIO 38.
`ULL` = unsigned long long (64-bit) — cần thiết vì GPIO 38 > 31, nếu dùng `1 << 38`
trên 32-bit integer sẽ bị undefined behavior.

**Sequence đúng cho PWDN:**
1. PWDN HIGH → sensor tắt nguồn
2. Chờ một chút
3. Camera driver init → PWDN LOW → sensor bật nguồn

### 4c. Camera Configuration

```c
    camera_config_t config = {
        // --- Kết nối phần cứng ---
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk  = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,  ... .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,
        
        // --- Timing ---
        .xclk_freq_hz = 20000000,   // 20MHz clock cấp cho OV5640
        .ledc_timer   = LEDC_TIMER_0,    // Dùng LEDC timer 0 để tạo XCLK
        .ledc_channel = LEDC_CHANNEL_0,  // Dùng LEDC channel 0
        
        // --- Output format ---
        .pixel_format = PIXFORMAT_JPEG,   // Output JPEG (đã nén)
        .frame_size   = FRAMESIZE_QQVGA,  // 160x120 pixels
        .jpeg_quality = 25,               // 0=best quality, 63=worst
        .fb_count     = 1,                // 1 frame buffer
        .fb_location  = CAMERA_FB_IN_PSRAM,  // Frame buffer trong PSRAM
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };
```

**`xclk_freq_hz = 20000000`** — ESP cấp clock 20MHz cho sensor OV5640.
OV5640 cần ít nhất 6MHz để hoạt động, tối đa 24MHz. 20MHz là giá trị phổ biến nhất.

**`LEDC_TIMER_0` và `LEDC_CHANNEL_0`** — LEDC (LED Control) là peripheral tạo
PWM/clock signal. Camera driver dùng LEDC để tạo XCLK cho sensor. Nếu bạn đang
dùng LEDC cho mục đích khác (ví dụ LED PWM), chọn timer/channel khác (0-3).

**`PIXFORMAT_JPEG`** — Sensor OV5640 encode JPEG hardware. Output là JPEG đã nén,
nhỏ hơn nhiều so với RAW:
- JPEG QQVGA (160x120): ~5-20KB tùy jpeg_quality
- RAW RGB565 QQVGA (160x120): 160×120×2 = 38.4KB cố định

**`FRAMESIZE_QQVGA`** — Độ phân giải 160×120 = nhỏ nhất, fit vào DMA buffer.
Các size khác:
```
FRAMESIZE_96X96   = 96×96
FRAMESIZE_QQVGA   = 160×120
FRAMESIZE_QVGA    = 320×240
FRAMESIZE_VGA     = 640×480
FRAMESIZE_SVGA    = 800×600
FRAMESIZE_XGA     = 1024×768
FRAMESIZE_HD      = 1280×720
FRAMESIZE_UXGA    = 1600×1200  ← OV5640 max (2MP)
FRAMESIZE_QXGA    = 2048×1536
FRAMESIZE_5MP     = 2592×1944  ← Không hỗ trợ với JPEG output trên OV5640
```

**`jpeg_quality = 25`** — Giá trị 0-63:
- 0 = Chất lượng tốt nhất, file lớn nhất
- 63 = Chất lượng kém nhất, file nhỏ nhất
- 25 là cân bằng tốt cho truyền qua network hoặc lưu trữ

### 4d. Xử lý lỗi camera init

```c
    esp_err_t err = esp_camera_init(&config);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA INIT FAILED: 0x%x", err);
        ESP_LOGE(TAG, "Error reason: %s", esp_err_to_name(err));
        while(1) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
    }
```

**`esp_err_t`** — Kiểu trả về của hầu hết hàm ESP-IDF:
- `ESP_OK` (0) = thành công
- Giá trị khác = error code

**`esp_err_to_name(err)`** — Chuyển error code thành string mô tả:
- `0x105` → `"ESP_ERR_NOT_FOUND"`
- `0x101` → `"ESP_ERR_NO_MEM"`
- `-1` (0xFFFFFFFF) → `"ESP_FAIL"`

**Vòng lặp `while(1)` khi lỗi** — Không nên return từ `app_main()` vì FreeRTOS
sẽ delete main task và hành vi không xác định. Thay vào đó, loop mãi với delay
để watchdog timer không reset (hoặc cố ý không delay để WDT reset = restart chip).

### 4e. Tạo task và main loop

```c
    xTaskCreate(
        capture_frame_task,  // Function
        "camera_task",       // Task name (debug only)
        8192,                // Stack size: 8KB
        NULL,                // Argument (arg trong function)
        5,                   // Priority (1=thấp nhất, configMAX_PRIORITIES-1=cao nhất)
        NULL                 // Task handle (NULL = không cần control sau này)
    );
    
    while (1) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);  // Main task chờ, nhường CPU
    }
```

**Stack size 8192 (8KB)** — Phải đủ cho:
- Stack frame của function
- Local variables
- Nested function calls
- String buffers nếu có

Nếu stack overflow → panic "Stack smashing detected" → tăng stack size lên.

**Priority 5** — Thang từ 1 đến `configMAX_PRIORITIES-1` (mặc định = 24):
- Priority cao hơn → chạy trước khi có nhiều task cùng ready
- Camera task priority 5 là hợp lý (không quá cao, không quá thấp)
- Idle task = priority 0

---

## Tóm tắt luồng hoạt động

```
[BOOT]
  Bootloader → Init PSRAM (BOOT_INIT=y) → Load firmware

[app_main() - Core 0]
  Print banner
  Check PSRAM status
  Config GPIO38 (PWDN)
  Init camera (allocate frame buffer trong PSRAM)
  xTaskCreate(capture_frame_task) → tạo task mới
  while(1) delay   ← main task ngủ, chỉ giữ cho FreeRTOS không idle

[capture_frame_task - Core 0 hoặc 1]
  loop mãi:
    esp_camera_fb_get()  → Chờ frame từ camera DMA
    Log frame info
    Check frame ở PSRAM
    esp_camera_fb_return()  → Trả buffer về pool
    vTaskDelay(2 giây)  → Nhường CPU
```

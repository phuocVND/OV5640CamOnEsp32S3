# PSRAM Deep Dive — Từ A đến Z

> **Đọc trước**: `00_ESP32S3_SYSTEM_OVERVIEW.md` (phần 2, 3, 4)

---

## PSRAM là gì và tại sao cần?

**PSRAM** = Pseudo Static RAM = RAM ngoài được kết nối qua bus SPI.

ESP32-S3 chỉ có **~512KB Internal SRAM**. Một frame ảnh JPEG 320x240 đã là ~30KB,
frame RAW RGB565 320x240 = 150KB → internal SRAM không đủ cho camera.

**Giải pháp**: Chip N16**R8** có thêm **8MB PSRAM** bên ngoài, kết nối qua bus MSPI
theo chuẩn **Octal SPI (OPI)** — 8 data lines song song.

```
           ESP32-S3
         ┌──────────┐
         │          │   8 data lines (D0-D7)
         │   MSPI   │──────────────────────── PSRAM 8MB
         │          │   + CS, CLK
         │          │
         │   MSPI   │──────────────────────── Flash 16MB
         │          │   4 data lines (QIO)
         └──────────┘
```

---

## Octal SPI vs Quad SPI — Tại sao quan trọng?

| Chuẩn | Data lines | Tốc độ tương đối | Dùng cho |
|-------|-----------|-----------------|---------|
| SPI   | 1         | 1x              | Thiết bị cơ bản |
| DSPI  | 2         | 2x              | Flash cũ |
| QSPI (QIO) | 4   | 4x              | Flash hiện đại |
| OSPI (OPI/Octal) | 8 | 8x          | PSRAM của N16R8 |

Chip **R8** trong N16R8 dùng PSRAM loại OPI (Octal) → **bắt buộc** config:
```
CONFIG_SPIRAM_MODE_OCT=y
```

Nếu bạn để chế độ QPI (4 lines) trong khi chip hardware là OPI (8 lines) →
PSRAM sẽ không respond đúng → init fail hoặc data bị garbage.

---

## Quá trình PSRAM Init từng bước

```
[Boot] Bootloader chạy
    │
    ├─ (nếu CONFIG_SPIRAM_BOOT_INIT=y)
    │   │
    │   ├─ esp_psram_init()          ← Driver detect và init PSRAM hardware
    │   │   ├─ Gửi lệnh init qua MSPI
    │   │   ├─ Config timing, mode (Octal)
    │   │   ├─ Thay đổi MMU mapping  ← ICache bị flush ở bước này!
    │   │   └─ PSRAM sẵn sàng
    │   │
    │   └─ heap_caps_add_region()    ← Thêm PSRAM vào heap allocator
    │       └─ Giờ malloc() hoặc heap_caps_malloc() có thể dùng PSRAM
    │
    └─ Jump vào app_main()
           │
           └─ PSRAM đã sẵn sàng, không cần init lại
```

**Tại sao ICache bị flush?**

Khi `esp_psram_init()` thay đổi MMU mapping, các địa chỉ ảo (virtual address) mà
CPU đang dùng để truy cập Flash bị thay đổi. ICache đang cache data từ các địa chỉ
cũ → ICache không còn valid → ICache bị **flush (xóa)**. Trong thời gian ngắn này,
CPU không thể đọc code từ Flash → nếu có function nào cần chạy lúc này → CRASH.

---

## psram_iram.lf — Tại sao bắt buộc?

File `.lf` = **Linker Fragment** — nói linker đặt symbol (function/variable) vào
vùng nhớ nào trong firmware.

Khi không có file này:
```
esp_psram_init() thay đổi MMU
    → ICache bị flush
    → CPU cần gọi heap_caps_add_region()
    → heap_caps_add_region() ở trong Flash
    → ICache đang bị flush, không đọc được Flash
    → CRASH: "Cache disabled but cached memory region accessed"
```

File `psram_iram.lf` fix bằng cách đặt các hàm này vào IRAM:
```
[mapping:psram_iram]
archive: libheap.a
entries:
    tlsf (noflash)                                    ← Đặt vào IRAM, không phải Flash
    multi_heap:multi_heap_register_impl (noflash)      ← IRAM
    multi_heap:multi_heap_register (noflash)           ← IRAM
    heap_caps_init:heap_caps_add_region_with_caps (noflash)  ← IRAM
    heap_caps_init:heap_caps_add_region (noflash)      ← IRAM
```

**`(noflash)`** = đặt vào IRAM (Internal SRAM), không đặt vào Flash.
Khi CPU cần chạy hàm này lúc ICache bị tắt → nó đã ở IRAM → an toàn.

### Tại sao cần thêm I2C vào IRAM?

```
[mapping:i2c_iram]
archive: libesp_driver_i2c.a
entries:
    i2c_master (noflash)
    i2c_common (noflash)
```

Camera OV5640 dùng giao tiếp **SCCB** (giống I2C) để config sensor. Khi camera init:
1. esp_psram_init() → ICache flush
2. Camera driver cần gọi I2C để giao tiếp với sensor
3. I2C code ở trong Flash → ICache đang bị tắt → CRASH ở địa chỉ `0x4201eb27`

Fix: Đặt I2C master code vào IRAM → có thể chạy ngay cả khi ICache bị tắt.

---

## Khai báo psram_iram.lf trong CMakeLists.txt

```cmake
# main/CMakeLists.txt
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "psram_iram.lf"    ← Phải khai báo ở đây
)
```

**Lỗi nếu quên**: File `.lf` tồn tại nhưng không khai báo trong `LDFRAGMENTS` →
Linker không đọc file này → Hàm heap/I2C vẫn ở Flash → Crash khi init PSRAM.

---

## Cách cấp phát bộ nhớ từ PSRAM trong code

### 1. heap_caps_malloc — Cấp phát thủ công từ PSRAM

```c
#include "esp_heap_caps.h"

// Cấp phát 1MB từ PSRAM
uint8_t *big_buffer = heap_caps_malloc(1024 * 1024, MALLOC_CAP_SPIRAM);

if (big_buffer == NULL) {
    ESP_LOGE(TAG, "PSRAM allocation failed!");
    // Nguyên nhân: PSRAM chưa init, hoặc hết PSRAM, hoặc CAPS_ALLOC chưa bật
    return;
}

// Dùng buffer...
memset(big_buffer, 0, 1024 * 1024);

// Giải phóng khi xong
heap_caps_free(big_buffer);
```

### 2. MALLOC_CAP flags — Chỉ định loại bộ nhớ

```c
MALLOC_CAP_SPIRAM      // PSRAM (external RAM)
MALLOC_CAP_INTERNAL    // Internal SRAM
MALLOC_CAP_DMA         // DMA-capable memory (thường là internal)
MALLOC_CAP_8BIT        // Byte-accessible (default)
MALLOC_CAP_32BIT       // 32-bit word-accessible
```

Kết hợp flags:
```c
// Cần DMA-capable và internal (cho buffer SPI/I2S)
heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

// Cần PSRAM nhưng fallback về internal nếu PSRAM hết
heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
```

### 3. Camera frame buffer trong PSRAM

```c
camera_config_t config = {
    ...
    .fb_location = CAMERA_FB_IN_PSRAM,   // Frame buffer lưu trong PSRAM
    .fb_count = 2,                        // 2 frame buffers (double buffering)
};
```

Với `FRAMESIZE_VGA` (640x480 JPEG ~30KB/frame) × 2 buffers = ~60KB PSRAM
Với `FRAMESIZE_UXGA` (1600x1200 JPEG ~200KB/frame) × 2 = ~400KB PSRAM
Với `FRAMESIZE_QSXGA` (2592x1944 JPEG ~1MB/frame) × 2 = ~2MB PSRAM

---

## Kiểm tra PSRAM status trong code

```c
#include "esp_psram.h"
#include "esp_heap_caps.h"

void check_psram(void) {
    #ifdef CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t used  = total - free;
        
        ESP_LOGI(TAG, "PSRAM: Total=%uKB, Free=%uKB, Used=%uKB",
                 total/1024, free/1024, used/1024);
    } else {
        ESP_LOGE(TAG, "PSRAM not initialized!");
    }
    #else
    ESP_LOGW(TAG, "PSRAM disabled in config");
    #endif
}
```

---

## Giới hạn của PSRAM — Phải biết!

### PSRAM KHÔNG thể chạy code trực tiếp
Code phải ở IRAM hoặc Flash (đọc qua ICache). PSRAM chỉ chứa **data**.

```c
// ĐÚNG: Data trong PSRAM
uint8_t *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);

// SAI: Code/function không thể đặt vào PSRAM
// (linker sẽ báo lỗi nếu bạn cố làm điều này)
```

### PSRAM chậm hơn Internal SRAM
- Internal SRAM: ~1 cycle latency
- PSRAM qua MSPI: ~10-20 cycle latency + bus contention

→ Đặt code và biến thời gian thực (interrupt handlers, tight loops) vào IRAM/Internal SRAM.
→ Đặt buffer lớn, frame data vào PSRAM.

### DMA và PSRAM
Không phải tất cả DMA đều hỗ trợ PSRAM. Trên ESP32-S3:
```
CONFIG_SOC_PSRAM_DMA_CAPABLE=y  ← S3 hỗ trợ DMA với PSRAM
```

Camera driver của S3 có thể dùng DMA với PSRAM frame buffer → OK.
Một số peripheral khác (SPI DMA mode cũ) không hỗ trợ PSRAM → phải dùng internal SRAM.

---

## Debug PSRAM không hoạt động

### Symptom 1: `esp_psram_is_initialized()` trả về false
```
Nguyên nhân có thể:
1. CONFIG_SPIRAM=y chưa set
2. CONFIG_SPIRAM_BOOT_INIT=y chưa set
3. CONFIG_SPIRAM_MODE_OCT sai (chip R8 cần OCT)
4. Hardware lỗi (board không có PSRAM, solder kém)

Debug:
→ idf.py menuconfig → Component config → ESP PSRAM → xem config
→ idf.py monitor → tìm dòng "psram: Found ..." trong bootloader log
```

### Symptom 2: heap_caps_malloc(MALLOC_CAP_SPIRAM) trả về NULL
```
Nguyên nhân có thể:
1. CONFIG_SPIRAM_USE_CAPS_ALLOC=y chưa set
2. PSRAM đã đầy (hết free space)
3. Size quá lớn cho một chunk liên tục

Debug:
→ Kiểm tra heap_caps_get_free_size(MALLOC_CAP_SPIRAM) trước khi malloc
```

### Symptom 3: Crash với "Cache disabled but cached memory region accessed"
```
Nguyên nhân: Hàm trong Flash được gọi lúc ICache đang bị disable (khi PSRAM init)

Debug:
→ Xem backtrace trong panic dump
→ Hàm nào ở địa chỉ 0x420xxxxx (Flash region) đang được gọi?
→ Thêm hàm đó vào psram_iram.lf với (noflash)
```

### Symptom 4: PSRAM hoạt động nhưng data bị corrupt
```
Nguyên nhân có thể:
1. CONFIG_SPIRAM_MODE_OCT sai (ví dụ set QPI nhưng chip là OPI)
2. CONFIG_SPIRAM_SPEED quá cao cho board (thử giảm xuống 40MHz)
3. Power supply không ổn định khi đọc/ghi PSRAM nhanh

Debug:
→ Giảm tốc độ: CONFIG_SPIRAM_SPEED_40M=y
→ Bật memory test (chậm nhưng detect lỗi): CONFIG_SPIRAM_MEMTEST=y
```

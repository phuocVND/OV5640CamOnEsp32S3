# Hướng dẫn Config PSRAM & Camera trên ESP32-S3 (IDF v5.5+)

**Target:** ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM Octal)  
**IDF:** ESP-IDF v5.5  
**Camera:** OV5640 qua DVP interface  

---

## 1. Kiến trúc PSRAM trên ESP32-S3

### 1.1 Sơ đồ bus MSPI

```
ESP32-S3 SoC
│
├── MSPI (dùng chung 1 bus)
│   ├── Flash (QIO 80MHz) ─── GPIO pins (SPI Flash)
│   └── PSRAM (OCT 40MHz) ── GPIO30 (CLK), GPIO26 (CS)
│
├── CPU0 + CPU1
│   ├── ICache (instruction cache) ─ fetch từ Flash/PSRAM
│   └── DCache (data cache)        ─ access RAM/PSRAM
│
└── DMA (camera, UART, SPI...)     ─ direct access PSRAM
```

**Vấn đề cốt lõi:** Flash và PSRAM chia sẻ cùng bus MSPI. Khi camera DMA đang
đọc/ghi PSRAM, nếu CPU đồng thời fetch instruction từ Flash → **tranh chấp bus**
→ pipeline stall → giảm hiệu suất hoặc crash.

### 1.2 Giải pháp: SPIRAM_FETCH_INSTRUCTIONS + SPIRAM_RODATA

```
Lúc boot (trước app_main):
  IDF copy: Flash .text   → PSRAM  (CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y)
  IDF copy: Flash .rodata → PSRAM  (CONFIG_SPIRAM_RODATA=y)

Khi runtime:
  CPU fetch instruction → ICache → PSRAM  (không qua Flash bus)
  Camera DMA           → PSRAM            (không tranh chấp)
```

---

## 2. Luồng khởi động PSRAM (ESP-IDF v5.5)

```
cpu_start.c
│
├── Phase 1: CONFIG_SPIRAM_BOOT_HW_INIT=y
│   └── esp_psram_chip_init()
│       → Init phần cứng chip PSRAM (electrical, timing)
│       → PSRAM hoạt động nhưng CHƯA có trong heap
│
├── Phase 2: CONFIG_SPIRAM_BOOT_INIT=y  ← BẮT BUỘC
│   └── esp_psram_init()
│       → Map PSRAM vào MMU (virtual address space)
│       → esp_psram_extram_add_to_heap_allocator()
│       → PSRAM được add vào heap với MALLOC_CAP_SPIRAM
│
├── [Copy instructions/rodata nếu FETCH_INSTRUCTIONS/RODATA=y]
│
└── app_main() bắt đầu
    → heap_caps_malloc(size, MALLOC_CAP_SPIRAM) HOẠT ĐỘNG
    → esp_camera_init() với CAMERA_FB_IN_PSRAM HOẠT ĐỘNG
```

### ⚠️ Lỗi phổ biến nhất: Thiếu `SPIRAM_BOOT_INIT=y`

```
# sdkconfig SAI (chỉ có phase 1):
CONFIG_SPIRAM_BOOT_HW_INIT=y
# CONFIG_SPIRAM_BOOT_INIT is not set   ← THIẾU CÁI NÀY

# Hậu quả:
# - PSRAM hardware OK nhưng KHÔNG có trong heap
# - MALLOC_CAP_SPIRAM → 0 bytes available
# - esp_camera_init() → fail (không alloc được frame buffer)
# - Guru Meditation Error
```

---

## 3. Cấu hình sdkconfig đúng cho N16R8

### 3.1 PSRAM Settings

```ini
# === PSRAM Cơ bản ===
CONFIG_SPIRAM=y                     # Bật PSRAM
CONFIG_SPIRAM_MODE_OCT=y            # Octal mode (8 data lines)
CONFIG_SPIRAM_TYPE_AUTO=y           # Tự detect loại chip
CONFIG_SPIRAM_CLK_IO=30             # GPIO30 = PSRAM clock
CONFIG_SPIRAM_CS_IO=26              # GPIO26 = PSRAM chip select

# === PSRAM Speed ===
# CONFIG_SPIRAM_SPEED_80M is not set  # ← Không dùng 80M lúc đầu
CONFIG_SPIRAM_SPEED_40M=y           # 40MHz = an toàn, ổn định
                                    # 40MHz Octal = 320MB/s bandwidth (đủ cho camera)

# === PSRAM Init Flow (ESP-IDF v5.5) ===
CONFIG_SPIRAM_BOOT_HW_INIT=y        # Phase 1: init hardware (IDF tự set khi BOOT_INIT=y)
CONFIG_SPIRAM_BOOT_INIT=y           # Phase 2: full init + add to heap ← BẮT BUỘC

# === PSRAM Allocation ===
CONFIG_SPIRAM_USE_CAPS_ALLOC=y      # heap_caps_malloc(MALLOC_CAP_SPIRAM) hoạt động

# === Performance: Giảm bus contention ===
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y  # Copy .text section vào PSRAM lúc boot
CONFIG_SPIRAM_RODATA=y              # Copy .rodata section vào PSRAM lúc boot
```

### 3.2 Flash Settings (N16R8 = 16MB Flash)

```ini
# === Flash Mode ===
# CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT is not set  # Force QIO, không auto-detect
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y    # QIO mode: ít tranh chấp hơn DIO với OPI PSRAM
CONFIG_ESPTOOLPY_FLASHMODE="qio"    # String phải khớp với bool ở trên

# === Flash Frequency ===
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y    # 80MHz flash
CONFIG_ESPTOOLPY_FLASHFREQ="80m"

# === Flash Size (N16R8 có 16MB) ===
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y   # ← Quan trọng: phải đặt đúng chip
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
```

### 3.3 Partition Table (tận dụng 16MB)

File `partitions.csv`:
```csv
# Name,    Type, SubType, Offset,   Size,    Flags
nvs,       data, nvs,     0x9000,   0x6000,
phy_init,  data, phy,     0xF000,   0x1000,
factory,   app,  factory, 0x10000,  0x400000,
storage,   data, spiffs,  0x410000, 0xBF0000,
```

Bố cục bộ nhớ 16MB:
```
0x000000 ├─ Bootloader (32KB)
0x008000 ├─ Partition table (4KB)
0x009000 ├─ NVS (24KB)          ← lưu WiFi credentials, settings
0x00F000 ├─ PHY init (4KB)
0x010000 ├─ App factory (4MB)   ← firmware camera (~1.5-2MB thực tế)
0x410000 ├─ SPIFFS storage (12MB) ← ~1200 ảnh JPEG QVGA
0xFFFFFF └─ End
```

---

## 4. Danh sách lỗi cần tránh

### Lỗi 1: Không set `SPIRAM_BOOT_INIT=y`
```
Triệu chứng: PSRAM báo 0 bytes, camera init fail ngay lập tức
Fix: CONFIG_SPIRAM_BOOT_INIT=y trong sdkconfig
```

### Lỗi 2: Gọi `esp_psram_init()` thủ công trong app_main()
```c
// ❌ SAI - Khi SPIRAM_BOOT_HW_INIT=y, chip đã init rồi
// Gọi lại lần 2 → conflict internal state s_psram_ctx.is_chip_initialised
esp_err_t ret = esp_psram_init();

// ✅ ĐÚNG - IDF đã xử lý trước app_main(), chỉ cần verify:
if (esp_psram_is_initialized()) { ... }
```

### Lỗi 3: Dùng private API `esp_private/esp_psram_extram.h`
```c
// ❌ SAI - Internal API, không stable giữa các IDF version
#include "esp_private/esp_psram_extram.h"
esp_psram_extram_add_to_heap_allocator();   // double-add → heap corruption

// ✅ ĐÚNG - Khi SPIRAM_BOOT_INIT=y, IDF tự gọi qua ESP_SYSTEM_INIT_FN()
// Không cần gọi thủ công
```

### Lỗi 4: Gọi `Cache_Invalidate_ICache_All()` từ flash
```c
// ❌ SAI - Invalidate ICache trong khi đang fetch instruction từ flash
// = corrupt instruction stream → Guru Meditation Error
#include "esp32s3/rom/cache.h"
static void flush_icache_all(void) {
    Cache_Invalidate_ICache_All();   // NGUY HIỂM
}

// ✅ Không cần làm điều này. Xóa hoàn toàn.
```

### Lỗi 5: Flash mode DIO với OPI PSRAM
```ini
# ❌ SAI - DIO flash + OPI PSRAM gây tranh chấp MSPI bus
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y

# ✅ ĐÚNG - QIO flash giảm số clock cycles, ít xung đột hơn
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
```

### Lỗi 6: Flash size sai
```ini
# ❌ SAI (N16R8 có 16MB nhưng khai báo 2MB)
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
# Hậu quả: partition table sai, firmware bị giới hạn, OTA không dùng được

# ✅ ĐÚNG
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

---

## 5. Pattern code chuẩn để verify PSRAM

```c
#include "esp_psram.h"
#include "esp_heap_caps.h"

void check_psram_status(void)
{
#ifdef CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        size_t free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "PSRAM OK: total=%luKB, free=%luKB",
                 (unsigned long)total / 1024,
                 (unsigned long)free  / 1024);
    } else {
        ESP_LOGE(TAG, "PSRAM FAIL: check CONFIG_SPIRAM_BOOT_INIT=y");
        // Không nên tiếp tục init camera nếu PSRAM không hoạt động
    }
#else
    ESP_LOGW(TAG, "PSRAM disabled in config");
#endif
}
```

---

## 6. Camera config với PSRAM frame buffer

```c
camera_config_t config = {
    // ... pin definitions ...

    .xclk_freq_hz  = 20000000,           // 20MHz XCLK (ổn định cho OV5640)
    .pixel_format  = PIXFORMAT_JPEG,

    // Frame size gợi ý theo PSRAM availability:
    // QQVGA (160x120)  = ~10KB/frame   ← test ban đầu
    // QVGA  (320x240)  = ~15-30KB/frame ← production nhỏ
    // VGA   (640x480)  = ~30-60KB/frame ← production bình thường
    // SVGA  (800x600)  = ~50-100KB/frame
    // UXGA  (1600x1200)= ~100-200KB/frame ← cần fb_count=1
    .frame_size    = FRAMESIZE_QVGA,

    .jpeg_quality  = 12,                 // 0-63, nhỏ hơn = chất lượng cao hơn
    .fb_count      = 2,                  // 2 frame buffers trong PSRAM
    .fb_location   = CAMERA_FB_IN_PSRAM, // ← PHẢI chỉ định rõ
    .grab_mode     = CAMERA_GRAB_WHEN_EMPTY,
};
```

---

## 7. Checklist trước khi build

- [ ] `CONFIG_SPIRAM=y`
- [ ] `CONFIG_SPIRAM_BOOT_INIT=y` ← quan trọng nhất
- [ ] `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`
- [ ] `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y`
- [ ] `CONFIG_SPIRAM_RODATA=y`
- [ ] `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` và string `"qio"`
- [ ] `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` (cho N16R8)
- [ ] `CONFIG_PARTITION_TABLE_CUSTOM=y` với `partitions.csv`
- [ ] Không có `esp_psram_init()` thủ công trong code
- [ ] Không có `Cache_Invalidate_ICache_All()` trong code
- [ ] Không include `esp_private/esp_psram_extram.h`
- [ ] Camera config dùng `fb_location = CAMERA_FB_IN_PSRAM`

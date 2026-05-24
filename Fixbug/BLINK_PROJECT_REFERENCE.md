# Project Blink — Báo cáo tham chiếu cấu hình đã được fix

**Đường dẫn**: `/Users/phuocvnd/blink`
**Chip**: ESP32-S3-N8R8 (8MB Flash, 8MB PSRAM Octal)
**IDF Version**: ESP-IDF 5.5.4
**Trạng thái**: ✅ Đã fix xong, build thành công, PSRAM + Camera hoạt động

> Project này là **nguồn tham chiếu chính** khi fix EdgeVision (OV5640CamOnEsp32S3).
> Tất cả config trong blink đã được test thực tế và verify bằng `test_psram_config.py`.

---

## Cấu trúc thư mục của blink

```
blink/
├── CMakeLists.txt                  ← Root build file (có MINIMAL_BUILD)
├── sdkconfig                       ← Config tự sinh (đã có PSRAM đúng)
├── sdkconfig.defaults              ← Config chung cho mọi chip
├── sdkconfig.defaults.esp32s3      ← Config riêng cho S3: PSRAM + Flash QIO ← QUAN TRỌNG NHẤT
├── sdkconfig.defaults.esp32s3.bak  ← Backup trước khi fix (PSRAM speed 80MHz)
├── sdkconfig.defaults.esp32        ← Config cho ESP32 cũ
├── sdkconfig.defaults.esp32c3      ← Config cho C3
├── ... (các chip khác)
├── dependencies.lock               ← Lock file cho component versions
├── FIXBUG_PSRAM_ESP32S3.md         ← Ghi chép chi tiết quá trình fix PSRAM bug
├── PSRAM_CONFIGURATION.md          ← Tài liệu PSRAM config
├── test_psram_config.py            ← Script kiểm tra config sau build
├── pytest_blink.py                 ← Test tự động
└── main/
    ├── CMakeLists.txt              ← Khai báo src + 2 linker fragments
    ├── main.c                      ← Code camera + PSRAM check
    ├── idf_component.yml           ← Dependencies: led_strip + esp32-camera
    ├── Kconfig.projbuild           ← Custom menuconfig options
    ├── psram_iram.lf               ← Linker fragment: heap vào IRAM ← BẮT BUỘC
    └── freertos_iram.lf            ← Linker fragment: FreeRTOS caps vào IRAM ← BẮT BUỘC
```

---

## File 1: Root `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build — chỉ compile component thực sự dùng → binary nhỏ hơn, build nhanh hơn
idf_build_set_property(MINIMAL_BUILD ON)
project(blink)
```

**Điểm khác biệt so với EdgeVision**: Có dòng `MINIMAL_BUILD ON`
→ EdgeVision thiếu dòng này → build to hơn, chậm hơn.

---

## File 2: `main/CMakeLists.txt`

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "psram_iram.lf" "freertos_iram.lf")
```

**Điểm quan trọng**:
- Khai báo **2 linker fragment files** — cả 2 đều bắt buộc khi dùng PSRAM Octal
- Nếu thiếu `LDFRAGMENTS` → linker không đọc file .lf → crash khi boot

---

## File 3: `sdkconfig.defaults.esp32s3` ← QUAN TRỌNG NHẤT

```
# ==== LED Config ====
CONFIG_BLINK_LED_STRIP=y
CONFIG_BLINK_GPIO=38       ← GPIO38 = LED trên DevKitC v1.1

# ==== PSRAM — Octal 8MB (ESP32-S3 N8R8) ====
CONFIG_SPIRAM=y                            ← Bật PSRAM tổng thể
CONFIG_SPIRAM_MODE_OCT=y                   ← Octal mode (8 data lines) — đúng cho R8
CONFIG_SPIRAM_TYPE_AUTO=y                  ← Tự detect loại PSRAM
CONFIG_SPIRAM_SPEED_40M=y                  ← 40MHz — ổn định, đã test (trước là 80MHz bị lỗi)
# CONFIG_SPIRAM_BOOT_HW_INIT is not set    ← Tắt HW_INIT riêng lẻ
CONFIG_SPIRAM_BOOT_INIT=y                  ← Full init trước app_main() ← FIX CHÍNH
# CONFIG_SPIRAM_MEMTEST is not set         ← Tắt memory test khi boot
# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set  ← Không đặt code vào PSRAM
# CONFIG_SPIRAM_RODATA is not set          ← Không đặt read-only data vào PSRAM
CONFIG_SPIRAM_USE_CAPS_ALLOC=y             ← Cho phép heap_caps_malloc(MALLOC_CAP_SPIRAM)
# CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is not set  ← Không dùng PSRAM cho stack

# ==== Flash Mode — QIO để giảm xung đột MSPI với OPI PSRAM ====
# CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT is not set  ← Tắt auto detect
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y           ← QIO (4 lines) — ít chiếm bus hơn DIO
```

**So sánh trước/sau khi fix** (xem `sdkconfig.defaults.esp32s3.bak`):
```
TRƯỚC (bak):                    SAU (đã fix):
CONFIG_SPIRAM_SPEED_80M=y  →    CONFIG_SPIRAM_SPEED_40M=y
(CONFIG_SPIRAM_BOOT_INIT bị      CONFIG_SPIRAM_BOOT_INIT=y
thiếu trong sdkconfig)
```

---

## File 4: `sdkconfig.defaults` (config chung)

```
CONFIG_BLINK_LED_GPIO=y    ← Default: dùng LED GPIO thường (override bởi .esp32s3)
CONFIG_BLINK_GPIO=8        ← Default GPIO: 8
```

Đây là config fallback cho các chip không có file `.defaults.{chip}` riêng.
File `.esp32s3` sẽ **override** các giá trị này khi target là esp32s3.

---

## File 5: `main/idf_component.yml`

```yaml
dependencies:
  espressif/led_strip: "^3.0.0"    ← WS2812 LED strip driver
  espressif/esp32-camera: "*"       ← Camera driver (OV2640, OV5640, ...)
```

**Cách hoạt động**:
1. Khai báo dependency ở đây
2. Chạy `idf.py update-dependencies` → ESP-IDF tải về `managed_components/`
3. File `dependencies.lock` ghi lại version đã tải để reproducible build

---

## File 6: `main/psram_iram.lf` — Linker Fragment cho PSRAM

```
[mapping:psram_iram]
archive: libheap.a
entries:
    # Phải ở IRAM vì ICache không ổn định sau esp_psram_init() đổi MMU mapping
    tlsf (noflash)
    multi_heap:multi_heap_register_impl (noflash)
    multi_heap:multi_heap_register (noflash)
    heap_caps_init:heap_caps_add_region_with_caps (noflash)
    heap_caps_init:heap_caps_add_region (noflash)

[mapping:i2c_iram]
archive: libesp_driver_i2c.a
entries:
    # i2c_master_bus gọi trong camera init, sau khi PSRAM init làm corrupt ICache
    # Đặt vào IRAM tránh crash tại địa chỉ 0x4201eb27
    i2c_master (noflash)
    i2c_common (noflash)
```

**Tại sao có 2 section?**
- `psram_iram`: Fix crash khi heap init sau PSRAM MMU mapping
- `i2c_iram`: Fix crash đặc thù khi camera init gọi I2C sau PSRAM init

---

## File 7: `main/freertos_iram.lf` — Linker Fragment cho FreeRTOS

```
[mapping:freertos_idf_additions_iram]
archive: libfreertos.a
entries:
    idf_additions:xSemaphoreCreateGenericWithCaps (noflash)
    idf_additions:xQueueCreateWithCaps (noflash)
    idf_additions:vQueueDeleteWithCaps (noflash)
    idf_additions:vSemaphoreDeleteWithCaps (noflash)
    idf_additions:vStreamBufferGenericDeleteWithCaps (noflash)
    idf_additions:vTaskDeleteWithCaps (noflash)
    idf_additions:xStreamBufferGenericCreateWithCaps (noflash)
    idf_additions:xTaskCreatePinnedToCoreWithCaps (noflash)
```

**Lý do cần file này** (từ comment trong file):
Sau khi PSRAM init, MSPI có thể bị ICache fill failure cho "cold flash addresses"
(địa chỉ Flash chưa được cache từ trước). Các function FreeRTOS liên quan đến
`WithCaps` (tạo semaphore/queue/task trong PSRAM) nằm ở "cold flash page" → ICache
fill fail → crash ngẫu nhiên.

Đặt các function này vào IRAM → chạy an toàn dù ICache đang bất ổn.

---

## `sdkconfig` — Config tổng hợp sau fix (các section quan trọng)

### PSRAM (dòng 1027-1058)
```
CONFIG_SPIRAM=y                  ← Đã bật
CONFIG_SPIRAM_MODE_OCT=y         ← Octal mode
CONFIG_SPIRAM_TYPE_AUTO=y        ← Auto detect
CONFIG_SPIRAM_CLK_IO=30          ← GPIO30 (clock của PSRAM — hardware cố định)
CONFIG_SPIRAM_CS_IO=26           ← GPIO26 (chip select — hardware cố định)
CONFIG_SPIRAM_SPEED_40M=y        ← 40MHz (xuống từ 80MHz sau khi fix)
CONFIG_SPIRAM_BOOT_HW_INIT=y     ← Phase 1: init hardware
CONFIG_SPIRAM_BOOT_INIT=y        ← Phase 2: full init + add to heap ← FIX CHÍNH
CONFIG_SPIRAM_USE_CAPS_ALLOC=y   ← heap_caps_malloc(MALLOC_CAP_SPIRAM)
```

**Lưu ý quan trọng**: Trong `sdkconfig` có `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`
(dòng 1758) — điều này khác với `sdkconfig.defaults.esp32s3` đang set `is not set`.
Đây là điểm cần theo dõi: sdkconfig và defaults không đồng bộ ở config này.

### Flash (dòng 555-574)
```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y   ← Đúng (từ defaults.esp32s3)
CONFIG_ESPTOOLPY_FLASHMODE="dio"   ← ⚠️ String vẫn là "dio" — inconsistency nhỏ
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y   ← Flash frequency: 40MHz
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y   ← ⚠️ Vẫn là 2MB (chip thực là 8MB)
```

**Chú ý**: blink chip là N**8**R8 (8MB Flash) nhưng config đặt 2MB — đây là bug
tồn tại trong blink. Với EdgeVision N**16**R8 cần set 16MB.

### CPU Speed (dòng 1080)
```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160     ← 160MHz (mặc định)
```

### FreeRTOS (dòng 1209)
```
CONFIG_FREERTOS_HZ=100    ← 1 tick = 10ms
```

---

## Quá trình fix PSRAM trong blink (tóm tắt từ FIXBUG_PSRAM_ESP32S3.md)

```
TRẠNG THÁI BAN ĐẦU (bị lỗi):
  sdkconfig: CONFIG_SPIRAM_BOOT_HW_INIT=y
             # CONFIG_SPIRAM_BOOT_INIT is not set  ← THIẾU
  main.c:    Gọi esp_psram_init() thủ công trong app_main()  ← SAI
             Gọi flush_icache_all()  ← NGUY HIỂM
             Dùng esp_private/esp_psram_extram.h  ← PRIVATE API

TRIỆU CHỨNG:
  - PSRAM hardware init nhưng không add vào heap
  - heap_caps_malloc(MALLOC_CAP_SPIRAM) trả NULL
  - Guru Meditation Error khi flush ICache từ Flash

FIX:
  1. Bật CONFIG_SPIRAM_BOOT_INIT=y trong sdkconfig
  2. Xóa esp_psram_init() thủ công khỏi app_main()
  3. Xóa flush_icache_all() và private headers
  4. Giảm PSRAM speed từ 80MHz → 40MHz
  5. Thêm psram_iram.lf và freertos_iram.lf
```

---

## Mapping: blink → EdgeVision (những gì cần copy)

| File trong blink | Tương ứng trong EdgeVision | Hành động |
|---|---|---|
| `sdkconfig.defaults.esp32s3` | Chưa có | **Tạo mới**, copy PSRAM config |
| `main/CMakeLists.txt` (có LDFRAGMENTS) | Thiếu LDFRAGMENTS | **Sửa** thêm LDFRAGMENTS |
| `main/psram_iram.lf` | Chưa có | **Tạo mới**, copy nguyên |
| `main/freertos_iram.lf` | Chưa có | **Tạo mới**, copy nguyên |
| `main/idf_component.yml` | Chưa có | **Tạo mới**, bỏ led_strip |
| `CMakeLists.txt` (MINIMAL_BUILD) | Thiếu | **Sửa** thêm MINIMAL_BUILD |
| Flash size trong defaults | Chưa có | **Thêm** 16MB (khác blink là 8MB) |

---

## Kết quả build của blink sau khi fix

```
blink.bin binary size 0x44a60 bytes (build sạch)
Project build complete ✓

Test script output (test_psram_config.py):
✓ PASS: sdkconfig - CONFIG_SPIRAM=y, CONFIG_CAMERA_PSRAM_DMA=y
✓ PASS: build headers - All PSRAM defines properly set
✓ PASS: binary symbols - PSRAM functions compiled in
✓ PASS: source code - PSRAM includes and proper usage
```

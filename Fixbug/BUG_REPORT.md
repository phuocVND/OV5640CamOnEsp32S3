# Bug Report — EdgeVision / OV5640 Camera on ESP32-S3-N16R8

**Project bị lỗi**: `/Desktop/OV5640CamOnEsp32S3/EdgeVision`
**Project tham chiếu (đã fix đúng)**: `/Users/phuocvnd/blink`
**Chip**: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM Octal)
**IDF Version**: ESP-IDF 5.5.4
**Ngày điều tra**: 2026-05-23

---

## So sánh nhanh: blink (đúng) vs EdgeVision (sai)

| Config / File | blink ✅ | EdgeVision ❌ |
|---------------|---------|-------------|
| `CONFIG_SPIRAM` | `y` | `is not set` |
| `CONFIG_SPIRAM_MODE_OCT` | `y` | `is not set` |
| `CONFIG_SPIRAM_BOOT_INIT` | `y` | `is not set` |
| `CONFIG_SPIRAM_USE_CAPS_ALLOC` | `y` | `is not set` |
| Flash mode | `QIO` | `DIO` |
| Flash size | N16 → cần 16MB (defaults chưa set) | `2MB` ← sai |
| `psram_iram.lf` | ✅ Có | ❌ Không có |
| `freertos_iram.lf` | ✅ Có | ❌ Không có |
| `idf_component.yml` | ✅ Có `esp32-camera` | ❌ Không có |
| `CMakeLists.txt LDFRAGMENTS` | ✅ Có | ❌ Không có |
| `MINIMAL_BUILD ON` | ✅ Có | ❌ Không có |
| `sdkconfig.defaults.esp32s3` | ✅ Có | ❌ Không có |

---

## Số lượng bug

| Mức độ | Số lượng |
|--------|---------|
| 🔴 CRITICAL (project không chạy được) | 5 |
| 🟡 MEDIUM (chạy nhưng không ổn định) | 2 |
| 🟢 LOW (không ảnh hưởng chức năng) | 2 |
| **Tổng** | **9** |

---

## 🔴 BUG-001: PSRAM bị tắt hoàn toàn — CRITICAL

**File**: `sdkconfig` dòng 1217
```
# CONFIG_SPIRAM is not set     ← EdgeVision: PSRAM TẮT
```

**So với blink** (`sdkconfig.defaults.esp32s3`):
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

**Hậu quả**:
- Camera config `CAMERA_FB_IN_PSRAM` → nhưng PSRAM tắt → camera init **FAIL**
- `esp_psram_is_initialized()` → luôn `false`
- `heap_caps_malloc(MALLOC_CAP_SPIRAM)` → luôn `NULL`
- Serial monitor sẽ in: `W: PSRAM is DISABLED in config` rồi `CAMERA INIT FAILED`

**Fix**: Tạo file `sdkconfig.defaults.esp32s3` (chép từ blink):
```
# PSRAM - Octal 8MB (ESP32-S3 N8R8 / N16R8)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_40M=y
# CONFIG_SPIRAM_BOOT_HW_INIT is not set
CONFIG_SPIRAM_BOOT_INIT=y
# CONFIG_SPIRAM_MEMTEST is not set
# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set
# CONFIG_SPIRAM_RODATA is not set
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
# CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is not set
```

---

## 🔴 BUG-002: Flash Mode DIO thay vì QIO — CRITICAL

**File**: `sdkconfig` dòng 553-559
```
CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT=y  ← Nguy hiểm: auto detect có thể sai
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y           ← DIO: chỉ 2 data lines
CONFIG_ESPTOOLPY_FLASHMODE="dio"
```

**So với blink** (`sdkconfig.defaults.esp32s3` dòng 23-24):
```
# Flash QIO mode - more bandwidth, less MSPI contention with OPI PSRAM
# CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT is not set
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
```

**Tại sao QIO quan trọng với PSRAM Octal?**
Flash và PSRAM dùng chung bus MSPI. PSRAM dùng OPI (8 lines). Nếu Flash dùng DIO
(2 lines) → chiếm bus lâu hơn → xung đột → crash ngẫu nhiên khi camera đang stream.
QIO (4 lines) = nhanh hơn, ít xung đột hơn.

Blink comment giải thích rõ ràng:
```
# Flash QIO mode - more bandwidth, less MSPI contention with OPI PSRAM
```

**Fix**:
```
# CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT is not set
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
```

---

## 🔴 BUG-003: Flash Size = 2MB (thực tế chip là 16MB) — CRITICAL

**File**: `sdkconfig` dòng 566-573
```
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
```

**Thực tế**: Chip **N16**R8 = **16MB Flash**.

**Hậu quả**:
- ESP-IDF chỉ "thấy" 2MB đầu → 14MB còn lại bị bỏ phí
- Khi app size vượt ~1.5MB (sau khi thêm WiFi, MQTT, v.v.) → không flash được
- Partition table sai → khó dùng OTA, SPIFFS về sau

**Lưu ý**: Blink project cũng chưa set 16MB trong `sdkconfig.defaults.esp32s3`
(file defaults blink chỉ có LED + PSRAM + Flash mode). Tuy nhiên blink dùng
chip N8R8 (8MB Flash) nên chưa cần. EdgeVision dùng **N16R8 (16MB)** → phải add.

**Fix — thêm vào `sdkconfig.defaults.esp32s3`**:
```
# Flash Size 16MB for N16R8
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
```

---

## 🔴 BUG-004: Thiếu psram_iram.lf và freertos_iram.lf — CRITICAL

**File bị thiếu**: `main/psram_iram.lf` và `main/freertos_iram.lf`
**File thiếu khai báo**: `main/CMakeLists.txt` thiếu `LDFRAGMENTS`

**So với blink**:
```
main/psram_iram.lf      ← Có đầy đủ
main/freertos_iram.lf   ← Có đầy đủ
```

Blink `main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "psram_iram.lf" "freertos_iram.lf")
```

EdgeVision `main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")
← Thiếu LDFRAGMENTS!
```

**Hậu quả khi PSRAM được bật (sau BUG-001 fix)**:
```
Guru Meditation Error: Core 0 panic'ed
(Cache disabled but cached memory region accessed)
Backtrace: 0x4038xxxx → 0x4201eb27   ← I2C master bị gọi lúc ICache tắt
```

Blink giải thích trong comment của `psram_iram.lf`:
```
# These TLSF functions are called when adding PSRAM to the heap allocator.
# They must be in IRAM because the instruction cache may be temporarily
# unstable after esp_psram_init() changes MMU mappings.
```
```
# i2c_new_master_bus and friends land on a cold flash page (page 1) that
# is first accessed only during camera init, well after esp_psram_init()
# has corrupted and re-mapped ICache. Placing these in IRAM avoids the
# stale-ICache-fill crash at 0x4201eb27.
```

**Fix — Bước 1**: Tạo `main/psram_iram.lf` (copy từ blink):
```
[mapping:psram_iram]
archive: libheap.a
entries:
    tlsf (noflash)
    multi_heap:multi_heap_register_impl (noflash)
    multi_heap:multi_heap_register (noflash)
    heap_caps_init:heap_caps_add_region_with_caps (noflash)
    heap_caps_init:heap_caps_add_region (noflash)

[mapping:i2c_iram]
archive: libesp_driver_i2c.a
entries:
    i2c_master (noflash)
    i2c_common (noflash)
```

**Fix — Bước 2**: Tạo `main/freertos_iram.lf` (copy từ blink):
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

**Fix — Bước 3**: Cập nhật `main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "psram_iram.lf" "freertos_iram.lf")
```

---

## 🔴 BUG-005: Thiếu idf_component.yml — CRITICAL

**File bị thiếu**: `main/idf_component.yml`
**Thư mục**: `managed_components/` không tồn tại

**So với blink** (`main/idf_component.yml`):
```yaml
dependencies:
  espressif/led_strip: "^3.0.0"
  espressif/esp32-camera: "*"
```

EdgeVision chỉ cần camera (không cần led_strip):
```yaml
dependencies:
  espressif/esp32-camera: "*"
```

**Hậu quả**:
- Build fail: `fatal error: 'esp_camera.h' file not found`
- Project không thể build trên máy mới hoặc sau `idf.py fullclean`

**Fix**:
1. Tạo file `main/idf_component.yml` với nội dung trên
2. Chạy: `idf.py update-dependencies`

---

## 🔴 BUG-006: Thiếu MINIMAL_BUILD trong root CMakeLists.txt — MEDIUM

**File**: `CMakeLists.txt` (root)

**EdgeVision hiện tại**:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EdgeVision)
```

**Blink (đúng)**:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(blink)
```

**Hậu quả**: Không có `MINIMAL_BUILD ON` → ESP-IDF build tất cả component kể cả
không dùng → binary lớn hơn cần thiết, thời gian build lâu hơn.

**Fix**:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(EdgeVision)
```

---

## 🟡 BUG-007: Thiếu sdkconfig.defaults.esp32s3 — MEDIUM

**File bị thiếu**: `sdkconfig.defaults.esp32s3`

Blink có file này chứa toàn bộ PSRAM và Flash config. EdgeVision không có → mọi
config chỉ trong `sdkconfig` (file tự sinh, không nên commit vào git).

**Hậu quả**: Clone project trên máy mới → mất hết config → không biết cần set gì.

**Fix**: Tạo `sdkconfig.defaults.esp32s3` tổng hợp tất cả fix ở trên:
```
# ============================================
# PSRAM - Octal 8MB (ESP32-S3 N16R8)
# ============================================
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_40M=y
# CONFIG_SPIRAM_BOOT_HW_INIT is not set
CONFIG_SPIRAM_BOOT_INIT=y
# CONFIG_SPIRAM_MEMTEST is not set
# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set
# CONFIG_SPIRAM_RODATA is not set
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
# CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is not set

# ============================================
# Flash - QIO mode + 16MB size for N16R8
# ============================================
# CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT is not set
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"

# ============================================
# Camera OV5640 GPIO (đổi nếu wiring khác)
# ============================================
# GPIO38=PWDN, GPIO47=RESET, GPIO15=XCLK
# GPIO4=SDA, GPIO5=SCL
# D7-D0: 16,17,18,12,10,8,9,11
# VSYNC=6, HREF=7, PCLK=13
```

---

## 🟢 BUG-008: Comment sai tên sensor — LOW

**File**: `main/main.c` dòng 21
```c
// Camera Pin Definition for ESP32-S3-CAM (OV2640)  ← SAI
```

**Fix**:
```c
// Camera Pin Definition for OV5640 (DVP parallel interface)
```

---

## 🟢 BUG-009: Kiểm tra PSRAM location dùng sai API — LOW

**File**: `main/main.c` dòng 61-66

`heap_caps_check_integrity_addr()` kiểm tra tính toàn vẹn heap, không xác nhận
buffer ở PSRAM hay SRAM. Message "Frame buffer in PSRAM" có thể SAI.

**Fix**:
```c
// PSRAM address range trên ESP32-S3: 0x3C000000 - 0x3FFFFFFF
bool is_psram = ((uint32_t)fb->buf >= 0x3C000000 &&
                 (uint32_t)fb->buf <  0x40000000);
ESP_LOGI(TAG, "Frame buffer in %s at 0x%08x",
         is_psram ? "PSRAM" : "internal SRAM",
         (uint32_t)fb->buf);
```

---

## Phân tích ảnh từ camera

| Ảnh | Chất lượng | Vấn đề |
|-----|-----------|--------|
| `usb_camera.jpg` | Rất tối, blur | Thiếu exposure config |
| `usb_camera_2.jpg` | Rất tối, blur | Thiếu AWB, AEC config |
| `test_inside.jpeg` | Bình thường (ảnh phòng khách) | Không phải từ camera |
| `test_outside.jpeg` | Bình thường (ảnh ngoài trời) | Không phải từ camera |
| `testimg.jpeg` | Hoa hồng rõ | Không phải từ camera |

**Kết luận ảnh camera** (`usb_camera.jpg`, `usb_camera_2.jpg`):
Ảnh tối và mờ → sensor OV5640 chưa được config AEC (Auto Exposure) và AWB.

**Fix — Thêm sau `esp_camera_init()` trong main.c**:
```c
sensor_t *s = esp_camera_sensor_get();
if (s != NULL) {
    s->set_whitebal(s, 1);       // Bật Auto White Balance
    s->set_awb_gain(s, 1);       // Bật AWB gain
    s->set_wb_mode(s, 0);        // Auto WB mode
    s->set_exposure_ctrl(s, 1);  // Bật Auto Exposure Control
    s->set_aec2(s, 1);           // Bật AEC DSP
    s->set_gain_ctrl(s, 1);      // Bật Auto Gain Control
    s->set_agc_gain(s, 0);       // AGC gain = 0 (auto)
    s->set_gainceiling(s, (gainceiling_t)2);  // Gain ceiling
    s->set_bpc(s, 1);            // Bad pixel correction
    s->set_wpc(s, 1);            // White pixel correction
    s->set_raw_gma(s, 1);        // Gamma correction
    s->set_lenc(s, 1);           // Lens shading correction
    s->set_brightness(s, 0);     // Brightness: -2 to 2
    s->set_contrast(s, 0);       // Contrast: -2 to 2
    s->set_saturation(s, 0);     // Saturation: -2 to 2
    ESP_LOGI(TAG, "Camera sensor controls configured");
}
```

---

## Thứ tự fix (theo blink làm mẫu)

```
Bước 1: Tạo main/idf_component.yml         ← BUG-005
         → idf.py update-dependencies

Bước 2: Tạo main/psram_iram.lf             ← BUG-004 (copy từ blink)
Bước 3: Tạo main/freertos_iram.lf          ← BUG-004 (copy từ blink)

Bước 4: Sửa main/CMakeLists.txt            ← BUG-004
         → Thêm LDFRAGMENTS

Bước 5: Sửa CMakeLists.txt (root)          ← BUG-006
         → Thêm MINIMAL_BUILD ON

Bước 6: Tạo sdkconfig.defaults.esp32s3     ← BUG-001,002,003,007
         → PSRAM + QIO + 16MB flash

Bước 7: idf.py set-target esp32s3          ← Merge defaults vào sdkconfig
Bước 8: idf.py build                        ← Verify build OK
Bước 9: idf.py flash monitor               ← Verify PSRAM + camera

Bước 10: Thêm sensor controls vào main.c   ← Fix ảnh tối/mờ
```

---

## Serial monitor — Đọc output để xác nhận fix đúng

### ✅ Thành công
```
I (xxx) esp_psram: Found 8MB PSRAM device
I (xxx) esp_psram: Speed: 40MHz
I (xxx) camera_example: PSRAM initialized OK - Total: 8388608 bytes
I (xxx) camera_example: ✓ CAMERA INIT SUCCESS!
I (xxx) camera_example: Captured frame: xxxxx bytes, size: 160x120
```

### ❌ BUG-001 chưa fix
```
W (xxx) camera_example: PSRAM is DISABLED in config
E (xxx) camera_example: CAMERA INIT FAILED
```

### ❌ BUG-004 chưa fix (crash lúc boot)
```
Guru Meditation Error: Core 0 panic'ed
(Cache disabled but cached memory region accessed)
Backtrace: ... 0x4201eb27 ...
```

### ❌ BUG-005 chưa fix (build fail)
```
fatal error: 'esp_camera.h' file not found
```

---

## Lệnh debug hữu ích

```bash
# Xem PSRAM config hiện tại
grep SPIRAM /Users/phuocvnd/Desktop/OV5640CamOnEsp32S3/EdgeVision/sdkconfig

# So sánh với blink
diff <(grep SPIRAM /Users/phuocvnd/blink/sdkconfig) \
     <(grep SPIRAM /Users/phuocvnd/Desktop/OV5640CamOnEsp32S3/EdgeVision/sdkconfig)

# Verify symbols trong binary (sau build)
xtensa-esp32s3-elf-nm build/EdgeVision.elf | grep "psram\|tlsf"

# Xóa build và build lại
idf.py fullclean && idf.py set-target esp32s3 && idf.py build
```

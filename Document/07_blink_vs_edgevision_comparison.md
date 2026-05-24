# So sánh blink (đã fix) vs EdgeVision (chưa fix)

> File này so sánh trực tiếp từng file giữa 2 project để bạn biết chính xác
> cần thêm/sửa gì trong EdgeVision.

---

## Cấu trúc file — Blink có, EdgeVision thiếu

```
blink/                                  EdgeVision/
├── CMakeLists.txt ✅                   ├── CMakeLists.txt ⚠️ (thiếu MINIMAL_BUILD)
├── sdkconfig.defaults.esp32s3 ✅       ├── (KHÔNG CÓ) ❌
├── main/
│   ├── CMakeLists.txt ✅               │   ├── CMakeLists.txt ⚠️ (thiếu LDFRAGMENTS)
│   ├── idf_component.yml ✅            │   ├── (KHÔNG CÓ) ❌
│   ├── psram_iram.lf ✅               │   ├── (KHÔNG CÓ) ❌
│   └── freertos_iram.lf ✅            │   └── (KHÔNG CÓ) ❌
```

---

## So sánh từng file

### 1. Root CMakeLists.txt

| | blink | EdgeVision |
|---|---|---|
| cmake_minimum_required | VERSION 3.16 ✅ | VERSION 3.16 ✅ |
| include IDF | ✅ | ✅ |
| **MINIMAL_BUILD** | `idf_build_set_property(MINIMAL_BUILD ON)` ✅ | ❌ **THIẾU** |
| project name | blink | EdgeVision |

**Fix EdgeVision** — Thêm dòng này trước `project(EdgeVision)`:
```cmake
idf_build_set_property(MINIMAL_BUILD ON)
```

---

### 2. main/CMakeLists.txt

| | blink | EdgeVision |
|---|---|---|
| SRCS | "main.c" | "main.c" |
| INCLUDE_DIRS | "." | "." |
| **LDFRAGMENTS** | `"psram_iram.lf" "freertos_iram.lf"` ✅ | ❌ **THIẾU** |

**Fix EdgeVision**:
```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "psram_iram.lf" "freertos_iram.lf")
```

---

### 3. sdkconfig.defaults.esp32s3

| Config | blink ✅ | EdgeVision ❌ |
|---|---|---|
| CONFIG_SPIRAM | y | **NOT SET** |
| CONFIG_SPIRAM_MODE_OCT | y | **NOT SET** |
| CONFIG_SPIRAM_TYPE_AUTO | y | **NOT SET** |
| CONFIG_SPIRAM_SPEED_40M | y | **NOT SET** |
| CONFIG_SPIRAM_BOOT_INIT | y | **NOT SET** |
| CONFIG_SPIRAM_USE_CAPS_ALLOC | y | **NOT SET** |
| CONFIG_ESPTOOLPY_FLASHMODE_QIO | y | **NOT SET** (đang DIO) |
| Flash mode AUTO_DETECT | NOT SET (tắt) | y (nguy hiểm) |

**Fix EdgeVision** — Tạo file `sdkconfig.defaults.esp32s3`:
```
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

# CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT is not set
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y

# Flash size: N16R8 có 16MB (blink là N8R8 có 8MB)
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
```

---

### 4. main/psram_iram.lf

**blink** — Tồn tại với 2 sections:
```
[mapping:psram_iram]  → heap functions vào IRAM
[mapping:i2c_iram]   → I2C driver vào IRAM (tránh crash 0x4201eb27)
```

**EdgeVision** — Không tồn tại ❌

**Fix**: Copy nguyên từ blink sang `EdgeVision/main/psram_iram.lf`

---

### 5. main/freertos_iram.lf

**blink** — Tồn tại, đặt 8 FreeRTOS WithCaps functions vào IRAM

**EdgeVision** — Không tồn tại ❌

**Fix**: Copy nguyên từ blink sang `EdgeVision/main/freertos_iram.lf`

---

### 6. main/idf_component.yml

| | blink | EdgeVision |
|---|---|---|
| espressif/led_strip | "^3.0.0" | Không cần |
| **espressif/esp32-camera** | "*" ✅ | ❌ **THIẾU** |

**Fix EdgeVision** — Tạo `main/idf_component.yml`:
```yaml
dependencies:
  espressif/esp32-camera: "*"
```

---

## Checklist fix theo thứ tự

Thực hiện theo đúng thứ tự này:

```
[ ] 1. Tạo main/idf_component.yml
[ ] 2. idf.py update-dependencies
[ ] 3. Tạo sdkconfig.defaults.esp32s3
[ ] 4. Copy psram_iram.lf từ blink vào EdgeVision/main/
[ ] 5. Copy freertos_iram.lf từ blink vào EdgeVision/main/
[ ] 6. Sửa main/CMakeLists.txt thêm LDFRAGMENTS
[ ] 7. Sửa CMakeLists.txt thêm MINIMAL_BUILD
[ ] 8. idf.py set-target esp32s3
[ ] 9. idf.py build
[10. idf.py flash monitor — verify hoạt động
```

---

## Lệnh copy nhanh từ blink

```bash
# Copy các linker fragment files
cp /Users/phuocvnd/blink/main/psram_iram.lf \
   /Users/phuocvnd/Desktop/OV5640CamOnEsp32S3/EdgeVision/main/

cp /Users/phuocvnd/blink/main/freertos_iram.lf \
   /Users/phuocvnd/Desktop/OV5640CamOnEsp32S3/EdgeVision/main/

# Sau đó sửa CMakeLists.txt và tạo sdkconfig.defaults.esp32s3 thủ công
# (không copy nguyên vì blink dùng N8R8, EdgeVision dùng N16R8 — flash size khác)
```

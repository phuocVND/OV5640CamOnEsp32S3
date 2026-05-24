# Vấn đề hiện tại của project và cách fix

> File này phân tích project EdgeVision hiện tại, chỉ ra những gì đang thiếu
> và hướng dẫn fix từng cái một.

---

## Kiểm tra project hiện tại

### Cấu trúc file hiện có:
```
EdgeVision/
├── CMakeLists.txt              ✅ Có, cơ bản đúng
├── main/
│   ├── CMakeLists.txt          ⚠️  Thiếu LDFRAGMENTS
│   └── main.c                  ✅ Có
├── sdkconfig                   ❌ PSRAM bị tắt (CONFIG_SPIRAM is not set)
└── (thiếu)
    ├── sdkconfig.defaults.esp32s3  ❌ Chưa có
    ├── main/psram_iram.lf          ❌ Chưa có
    ├── main/freertos_iram.lf       ❌ Chưa có
    └── main/idf_component.yml      ❌ Chưa có (nhưng camera component ở đâu?)
```

---

## Vấn đề 1: PSRAM bị tắt trong sdkconfig

**Triệu chứng**: `CONFIG_SPIRAM is not set` trong sdkconfig.

**Hậu quả**: 
- Camera config `CAMERA_FB_IN_PSRAM` nhưng PSRAM không bật → camera init FAIL
- `esp_psram_is_initialized()` trả false
- `heap_caps_malloc(MALLOC_CAP_SPIRAM)` trả NULL

**Fix**: Thêm file `sdkconfig.defaults.esp32s3` với nội dung:
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
```

Sau đó chạy:
```bash
idf.py set-target esp32s3   # Merge defaults vào sdkconfig
idf.py build
```

---

## Vấn đề 2: Thiếu psram_iram.lf

**Triệu chứng**: Không có file `main/psram_iram.lf`.

**Hậu quả**: Khi PSRAM được bật (sau khi fix vấn đề 1), boot có thể crash với:
```
Guru Meditation Error: Core 0 panic'ed (Cache disabled but cached memory region accessed)
```

**Fix**: Tạo file `main/psram_iram.lf`:
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

Và cập nhật `main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "psram_iram.lf"
)
```

---

## Vấn đề 3: Thiếu idf_component.yml

**Triệu chứng**: Không có file `main/idf_component.yml` nhưng code dùng `esp_camera.h`.

**Câu hỏi**: Camera component đến từ đâu? Kiểm tra `managed_components/`:
- Nếu có `managed_components/espressif__esp32-camera/` → đã được tải trước, nhưng
  không có `idf_component.yml` → sẽ mất khi `idf.py fullclean`
- Nếu không có → code sẽ không compile được

**Fix**: Tạo file `main/idf_component.yml`:
```yaml
dependencies:
  espressif/esp32-camera: "*"
```

Sau đó:
```bash
idf.py update-dependencies   # Tải component về managed_components/
idf.py build
```

---

## Vấn đề 4: Comment trong main.c sai sensor

**Triệu chứng**: Comment ghi `// Camera Pin Definition for ESP32-S3-CAM (OV2640)`
nhưng code config `xclk_freq_hz = 20000000` và project tên là OV5640.

**Hậu quả**: Không ảnh hưởng chức năng, nhưng gây nhầm lẫn khi debug.

**Fix**: Sửa comment thành `// Camera Pin Definition for OV5640 DVP`

---

## Thứ tự fix đề xuất

1. Tạo `main/idf_component.yml` với camera dependency
2. Tạo `main/psram_iram.lf`
3. Cập nhật `main/CMakeLists.txt` thêm `LDFRAGMENTS`
4. Tạo `sdkconfig.defaults.esp32s3` với PSRAM config
5. Chạy `idf.py update-dependencies`
6. Chạy `idf.py set-target esp32s3`
7. Chạy `idf.py build`
8. Flash và kiểm tra serial monitor

---

## Cách đọc serial monitor để verify

Kết quả mong đợi khi chạy đúng:
```
I (xxx) esp_psram: Found 8MB PSRAM device         ← PSRAM detect thành công
I (xxx) esp_psram: Speed: 40MHz                   ← Tốc độ PSRAM
I (xxx) camera_example: PSRAM OK - Total: 8388608 bytes, Free: 8xxxxxxx bytes
I (xxx) camera_example: ✓ CAMERA INIT SUCCESS!
I (xxx) camera_example: Captured frame: xxxxx bytes, size: 160x120
```

Nếu thấy:
```
E (xxx) camera_example: PSRAM NOT initialized!    ← Thiếu CONFIG_SPIRAM_BOOT_INIT
E (xxx) camera_example: CAMERA INIT FAILED: 0x...  ← Xem error code
Guru Meditation Error: Cache disabled...           ← Thiếu psram_iram.lf
```

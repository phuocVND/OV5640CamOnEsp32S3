# sdkconfig — File cấu hình trung tâm của ESP-IDF

> **Đọc trước**: `00_ESP32S3_SYSTEM_OVERVIEW.md` (phần 6)

---

## sdkconfig là gì?

`sdkconfig` là file **tự động sinh ra** bởi Kconfig system. Nó chứa **tất cả cấu hình**
của project dưới dạng `CONFIG_XXX=y` hoặc `CONFIG_XXX=value`.

Khi bạn compile code, preprocessor đọc file này và biến mỗi dòng thành `#define`:

```
sdkconfig:                    →   build/config/sdkconfig.h:
CONFIG_SPIRAM=y               →   #define CONFIG_SPIRAM 1
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160  →  #define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
# CONFIG_SPIRAM is not set    →   (không có #define → dùng trong #ifndef để check)
```

Trong code C, bạn dùng:
```c
#ifdef CONFIG_SPIRAM
    // Code này chỉ compile khi PSRAM được bật
#endif
```

---

## sdkconfig vs sdkconfig.defaults — Cái nào cần sửa?

```
sdkconfig.defaults.esp32s3   ← BẠN sửa cái này (commit vào git)
        ↓  (merge vào khi idf.py set-target)
sdkconfig                    ← TỰ SINH, KHÔNG sửa thủ công (thêm vào .gitignore)
        ↓  (sinh ra khi build)
build/config/sdkconfig.h     ← Compiler dùng, đừng đụng vào
```

**Quy trình:**
1. Bạn viết `sdkconfig.defaults.esp32s3`
2. Chạy `idf.py set-target esp32s3` → IDF merge defaults vào `sdkconfig`
3. (Tùy chọn) Chạy `idf.py menuconfig` để chỉnh thêm qua GUI
4. Kết quả cuối cùng là `sdkconfig`

**Tại sao project này chỉ có `sdkconfig` mà không có `sdkconfig.defaults.esp32s3`?**
→ Project này chưa được tổ chức đúng cách. Mọi config đang nằm trong `sdkconfig`
(file tự sinh). Khi reset hoặc `fullclean`, config sẽ bị mất.

---

## Các nhóm CONFIG quan trọng trong sdkconfig

### Nhóm 1: Target và Architecture

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_IDF_TARGET_ESP32S3=y
CONFIG_IDF_TARGET_ARCH_XTENSA=y
CONFIG_IDF_TARGET_ARCH="xtensa"
```

Được set bởi `idf.py set-target esp32s3`. Nói cho compiler biết:
- Dùng toolchain `xtensa-esp32s3-elf-gcc` (không phải ARM hay RISC-V)
- Enable các peripheral đặc trưng của S3 (LCDCAM, USB OTG, v.v.)
- **KHÔNG tự sửa** — luôn dùng lệnh `idf.py set-target`

---

### Nhóm 2: PSRAM / SPIRAM (quan trọng nhất với N16R8)

```
# CONFIG_SPIRAM is not set    ← Hiện tại PSRAM đang TẮT trong project này!
```

**Đây là vấn đề lớn** của project hiện tại. Camera được config dùng PSRAM
(`CAMERA_FB_IN_PSRAM`) nhưng PSRAM không được bật trong sdkconfig!

Các config PSRAM cần thiết cho N16R8:
```
CONFIG_SPIRAM=y               ← Bật PSRAM tổng thể
CONFIG_SPIRAM_MODE_OCT=y      ← Octal mode (8 data lines) cho chip R8
CONFIG_SPIRAM_TYPE_AUTO=y     ← Tự detect loại PSRAM
CONFIG_SPIRAM_SPEED_40M=y     ← Tốc độ bus 40MHz (an toàn cho mọi board)
CONFIG_SPIRAM_BOOT_INIT=y     ← Init PSRAM TRƯỚC app_main() (quan trọng!)
CONFIG_SPIRAM_USE_CAPS_ALLOC=y ← Cho phép heap_caps_malloc(MALLOC_CAP_SPIRAM)
```

Chi tiết từng config:

**`CONFIG_SPIRAM=y`** — Bật toàn bộ PSRAM subsystem. Nếu không có dòng này,
tất cả config SPIRAM khác bị ignore.

**`CONFIG_SPIRAM_MODE_OCT=y`** — Chip R8 dùng PSRAM loại **OPI (Octal SPI)**,
có 8 data lines. Nếu set sai mode (ví dụ QPI = 4 lines), PSRAM sẽ không hoạt động
hoặc bị corrupt data.

**`CONFIG_SPIRAM_TYPE_AUTO=y`** — IDF tự detect PSRAM type. Nếu để manual
mà chọn sai → không init được.

**`CONFIG_SPIRAM_SPEED_40M=y`** — Tốc độ clock của bus MSPI với PSRAM:
- 40MHz: An toàn, hoạt động với mọi board, nhiệt độ
- 80MHz: Nhanh hơn nhưng cần board chất lượng cao
- Khuyến nghị: Bắt đầu với 40MHz, tăng lên 80MHz nếu cần performance

**`CONFIG_SPIRAM_BOOT_INIT=y`** — Init PSRAM trong bootloader TRƯỚC khi gọi
`app_main()`. Nếu không set → PSRAM không được init → `esp_psram_is_initialized()`
trả về false → `heap_caps_malloc(MALLOC_CAP_SPIRAM)` trả về NULL.

**`CONFIG_SPIRAM_USE_CAPS_ALLOC=y`** — Cho phép user allocate từ PSRAM qua
`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`. Nếu không set → không thể dùng PSRAM
cho custom buffers.

Config thường TẮT (không cần thiết):
```
# CONFIG_SPIRAM_BOOT_HW_INIT is not set    ← Hardware init phức tạp hơn, không cần
# CONFIG_SPIRAM_MEMTEST is not set         ← Test RAM lúc boot, làm chậm startup
# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set  ← Đặt code vào PSRAM — KHÔNG nên
# CONFIG_SPIRAM_RODATA is not set          ← Đặt read-only data vào PSRAM — KHÔNG nên
# CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is not set  ← Stack trong PSRAM — nguy hiểm
```

---

### Nhóm 3: Flash Configuration

```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y     ← Flash mode: QIO (4 data lines)
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y    ← Flash size: 16MB
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_ESPTOOLPY_FLASH_FREQ_80M=y    ← Flash clock: 80MHz
```

**`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`** — Quan trọng khi dùng PSRAM Octal:
- DIO (2 lines): Mặc định, an toàn nhưng chậm và hay xung đột với OPI PSRAM
- QIO (4 lines): Nhanh hơn, ít xung đột hơn với OPI PSRAM → **Dùng cái này**
- OIO/OPI: Chỉ PSRAM dùng, Flash không dùng mode này

**`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`** — Phải khớp với phần cứng thực tế!
Nếu set 4MB nhưng chip có 16MB → chỉ dùng được 4MB đầu.
Nếu set 16MB nhưng chip chỉ có 4MB → crash hoặc garbage data.

---

### Nhóm 4: CPU Speed

```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
```

ESP32-S3 hỗ trợ: 80MHz, 160MHz, 240MHz

- **160MHz**: Cân bằng giữa hiệu năng và tiêu thụ điện — đây là mặc định
- **240MHz**: Tối đa, dùng khi cần xử lý nhiều (ví dụ ML inference)
- **80MHz**: Tiết kiệm điện, chỉ dùng cho ứng dụng đơn giản

Để đổi sang 240MHz:
```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
```

---

### Nhóm 5: Partition Table

```
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
```

**`CONFIG_PARTITION_TABLE_SINGLE_APP=y`** — Dùng partition table mặc định với 1 app:
```
nvs      | 24KB  | Lưu NVS data
phy_init | 4KB   | PHY calibration
factory  | ~1MB  | Firmware app
```

Khi nào cần đổi:
- Dùng OTA update → `CONFIG_PARTITION_TABLE_TWO_OTA=y`
- Cần SPIFFS/FAT để lưu file → `CONFIG_PARTITION_TABLE_CUSTOM=y` + file `partitions.csv`
- App lớn hơn 1MB → `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`

---

### Nhóm 6: Bootloader

```
CONFIG_BOOTLOADER_WDT_ENABLE=y
CONFIG_BOOTLOADER_WDT_TIME_MS=9000
CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y
```

**`CONFIG_BOOTLOADER_WDT_ENABLE=y`** — Watchdog timer trong bootloader.
Nếu bootloader bị treo quá 9000ms (9 giây) → tự reset.
Hữu ích khi boot bị stuck (ví dụ PSRAM init fail).

**`CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y`** — Log level của bootloader.
Khi monitor, bạn sẽ thấy message từ bootloader trước khi app chạy.

---

### Nhóm 7: FreeRTOS

```
CONFIG_FREERTOS_HZ=100
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536
# CONFIG_FREERTOS_UNICORE is not set
```

**`CONFIG_FREERTOS_HZ=100`** — Tick rate:
- 100 Hz = 1 tick = 10ms
- `pdMS_TO_TICKS(1000)` = 100 ticks = 1 giây
- Tăng lên 1000 Hz nếu cần timing chính xác hơn (1ms), nhưng tốn CPU hơn

**`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y`** — Đặt "canary value" ở cuối stack.
Nếu stack overflow ghi đè canary → panic với message "Stack smashing detected".
Rất hữu ích khi debug. Tắt đi chỉ khi optimize performance.

**`CONFIG_FREERTOS_UNICORE is not set`** — Dùng cả 2 CPU core (SMP mode).
Nếu set `CONFIG_FREERTOS_UNICORE=y` → chỉ dùng Core 0 (như ESP32 cũ).

---

### Nhóm 8: Log Level

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_LOG_MAXIMUM_LEVEL=5
```

Các level: 0=None, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Verbose

`ESP_LOGI(TAG, "...")` chỉ in ra nếu level ≥ INFO (3).
Để debug chi tiết hơn, set `CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y`.

---

### Nhóm 9: SOC Capabilities (đừng sửa)

```
CONFIG_SOC_SPIRAM_SUPPORTED=y
CONFIG_SOC_PSRAM_DMA_CAPABLE=y
CONFIG_SOC_LCDCAM_SUPPORTED=y
CONFIG_SOC_LCDCAM_CAM_SUPPORTED=y
...
```

Đây là các flag mô tả **phần cứng của chip ESP32-S3** — không phải config của bạn.
IDF tự set khi `idf.py set-target esp32s3`. Dùng trong code để kiểm tra:
```c
#if CONFIG_SOC_PSRAM_DMA_CAPABLE
    // Chỉ chạy trên chip hỗ trợ PSRAM DMA
#endif
```

---

## Tóm tắt: Config nào hay gây bug nhất

| Config | Hậu quả nếu sai |
|--------|----------------|
| `CONFIG_SPIRAM=y` thiếu | Camera crash, `heap_caps_malloc` trả NULL |
| `CONFIG_SPIRAM_MODE_OCT=y` sai | PSRAM không init hoặc data corrupt |
| `CONFIG_SPIRAM_BOOT_INIT=y` thiếu | PSRAM chưa sẵn sàng khi app_main() chạy |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` thiếu | Xung đột MSPI, crash ngẫu nhiên |
| `CONFIG_ESPTOOLPY_FLASHSIZE` sai | Đọc/ghi sai vùng Flash, data corrupt |
| `CONFIG_PARTITION_TABLE_*` sai | App không load được hoặc OTA fail |

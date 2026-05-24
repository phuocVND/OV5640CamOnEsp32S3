# Tổng quan hệ thống ESP32-S3 — Đọc trước khi config

> Đây là nền tảng để hiểu TẠI SAO mỗi file config lại có nội dung như vậy.
> Đọc file này trước, sau đó đọc các file tiếp theo sẽ dễ hiểu hơn nhiều.

---

## 1. ESP32-S3-N16R8 là con chip gì?

```
Tên:   ESP32-S3
Flash: N16  → 16MB Flash SPI (nơi chứa firmware/code)
PSRAM: R8   → 8MB PSRAM Octal SPI (bộ nhớ RAM mở rộng)
CPU:   2 nhân Xtensa LX7, tối đa 240MHz
```

Sơ đồ đơn giản:

```
┌─────────────────────────────────────────────┐
│                  ESP32-S3                   │
│                                             │
│  ┌──────────┐   ┌──────────┐               │
│  │  CPU 0   │   │  CPU 1   │  ← 2 nhân     │
│  │(PRO CPU) │   │(APP CPU) │    Xtensa LX7 │
│  └────┬─────┘   └────┬─────┘               │
│       │              │                     │
│  ┌────▼──────────────▼─────┐               │
│  │     Internal SRAM       │ ← 512KB SRAM  │
│  │  (nhanh, luôn có)       │   bên trong   │
│  └────────────┬────────────┘               │
│               │ Bus MSPI                   │
│  ┌────────────▼────────────┐               │
│  │      PSRAM 8MB          │ ← Octal SPI   │
│  │  (chậm hơn, nhiều hơn)  │   qua bus     │
│  └────────────┬────────────┘               │
│               │ Bus SPI                    │
│  ┌────────────▼────────────┐               │
│  │      Flash 16MB         │ ← Code/Data   │
│  │  (chứa firmware)        │   lưu trữ     │
│  └─────────────────────────┘               │
└─────────────────────────────────────────────┘
```

---

## 2. Ba loại bộ nhớ — phân biệt rõ ràng

### 2a. Internal SRAM (~512KB)
- **Tốc độ**: Nhanh nhất (truy cập 1 clock cycle)
- **Dùng cho**: Stack của task, biến local, code trong IRAM
- **Giới hạn**: Chỉ ~512KB — RẤT ÍT, không đủ cho camera frame buffer
- **Mất khi**: Mất điện / reset

### 2b. PSRAM (8MB — chip R8)
- **Tốc độ**: Chậm hơn SRAM vì phải qua bus MSPI (~40MHz)
- **Dùng cho**: Frame buffer camera, buffer lớn, heap mở rộng
- **QUAN TRỌNG**: PSRAM **không thể chạy code** — chỉ chứa DATA
- **Truy cập**: Qua `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
- **Mất khi**: Mất điện / reset

### 2c. Flash (16MB — chip N16)
- **Tốc độ**: Chậm nhất (nhưng có cache ICache giúp đọc code nhanh hơn)
- **Dùng cho**: Lưu firmware, NVS (WiFi password, settings), OTA images
- **Đặc điểm**: Không mất dữ liệu khi mất điện (non-volatile)

### So sánh nhanh
```
Tốc độ:      Internal SRAM  >  PSRAM  >  Flash
Dung lượng:  SRAM (512KB)   <  PSRAM (8MB)  <  Flash (16MB)
Code chạy:   IRAM (SRAM)    ✗ PSRAM         ✓ Flash (qua ICache)
Lưu dài hạn: ✗              ✗               ✓
```

---

## 3. Quá trình Boot — hiểu để debug crash

Khi cắm điện hoặc nhấn Reset, ESP32-S3 thực hiện theo thứ tự:

```
[Bước 1] ROM Bootloader (trong chip, không thể thay đổi)
          → Đọc Flash tại địa chỉ 0x0 để tìm 2nd stage bootloader
          → Kiểm tra chữ ký nếu Secure Boot bật

[Bước 2] 2nd Stage Bootloader  ← bạn có thể ảnh hưởng qua CONFIG_BOOTLOADER_*
          → Đọc Partition Table (0x8000) để biết Flash chia thế nào
          → Init PSRAM nếu CONFIG_SPIRAM_BOOT_INIT=y  ← QUAN TRỌNG
          → Load firmware từ app partition vào bộ nhớ
          → Jump vào firmware

[Bước 3] ESP-IDF Startup Code (startup.c trong IDF)
          → Init CPU, clocks, cache
          → Init heap allocator (cả Internal + PSRAM)
          → Tạo "main task" FreeRTOS
          → Gọi app_main()

[Bước 4] app_main()  ← Code của bạn bắt đầu chạy ở đây
```

**Tại sao biết điều này giúp debug?**
- Crash trước khi `APP MAIN STARTED` in ra → lỗi ở Bước 2-3 (thường do PSRAM config sai)
- Crash ngay sau khi `APP MAIN STARTED` → lỗi ở Bước 4, xem code bạn viết
- `Guru Meditation Error: Core 0 panic'ed (Cache disabled but cached memory region accessed)` → hàm trong Flash bị gọi lúc ICache đang bị tắt (PSRAM init) → cần IRAM

---

## 4. Bus MSPI — Nguồn gốc mọi xung đột Flash/PSRAM

**MSPI** (Master SPI) là bus duy nhất nối CPU với cả Flash và PSRAM:

```
CPU ──── MSPI Bus ──┬── Flash 16MB (SPI)
                    └── PSRAM  8MB (Octal SPI / OPI)
```

**Vấn đề cốt lõi**: Flash và PSRAM **chia sẻ cùng một bus vật lý**!

Khi init PSRAM:
1. Driver thay đổi MMU mapping (bảng ánh xạ địa chỉ ảo → vật lý)
2. ICache (cache cho Flash instructions) bị **flush và disable tạm thời**
3. Lúc này nếu CPU cần đọc instruction từ Flash → **CRASH** (Cache disabled)
4. Fix: Đặt hàm "nguy hiểm" vào IRAM (không cần đọc Flash)

**Đó chính xác là lý do tại sao cần `psram_iram.lf`**

**Flash Mode ảnh hưởng MSPI bandwidth:**
```
DIO mode: 2 data lines → bandwidth thấp, chiếm bus lâu hơn
QIO mode: 4 data lines → bandwidth cao hơn, ít xung đột với PSRAM hơn
OPI mode: 8 data lines → chỉ dành cho PSRAM, không phải Flash
```
→ Luôn set `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` khi dùng PSRAM Octal

---

## 5. FreeRTOS — Hệ điều hành thời gian thực

ESP-IDF chạy trên **FreeRTOS**. Mọi "task" trong code là một luồng FreeRTOS.

```
app_main() chạy trên "main task" (Core 0 mặc định)
    │
    ├── xTaskCreate(capture_frame_task, ...) → task mới (Core 1 hoặc 0)
    │
    └── while(1) { vTaskDelay(...) }  → nhường CPU cho task khác
```

**Key concepts:**
- `vTaskDelay(pdMS_TO_TICKS(1000))` = ngủ 1 giây, nhường CPU
- `xTaskCreate(func, "name", stack_size, arg, priority, handle)` = tạo task mới
- Stack size tính bằng bytes: 8192 = 8KB stack cho task

**Config FreeRTOS quan trọng:**
```
CONFIG_FREERTOS_HZ=100         → 1 tick = 10ms
CONFIG_FREERTOS_UNICORE=n      → Dùng cả 2 CPU core (default)
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y → Phát hiện stack overflow
```

---

## 6. Chuỗi Build: từ code đến firmware

```
Source files (.c, .h)
       ↓
    CMakeLists.txt   → Biết file nào compile, component nào dùng
       ↓
    sdkconfig        → Biết feature nào bật/tắt (#define CONFIG_xxx)
       ↓
    GCC Compiler     → Compile từng .c → .o (object file)
       ↓
    Linker           → Link .o + libraries → .elf
    (dùng .lf files để biết đặt symbol vào IRAM hay Flash)
       ↓
    esptool.py       → Chuyển .elf → .bin (firmware)
       ↓
    Flash vào chip qua UART hoặc JTAG
```

**File nào ảnh hưởng bước nào:**

| File | Ảnh hưởng bước nào |
|------|-------------------|
| `CMakeLists.txt` | Compiler — biết file nào compile |
| `sdkconfig` | Compiler — `#define CONFIG_xxx` trong code |
| `Kconfig.projbuild` | sdkconfig — định nghĩa config option custom |
| `idf_component.yml` | Compiler — thêm external libraries |
| `psram_iram.lf` | Linker — đặt symbol vào IRAM thay vì Flash |
| `.vscode/settings.json` | Chỉ IDE, không ảnh hưởng build |

---

## 7. Partition Table — Flash chia thế nào?

Flash 16MB không phải một khối liền — nó được chia theo **partition table**:

```
Địa chỉ     Tên         Loại    Kích thước   Dùng để
─────────────────────────────────────────────────────
0x000000    bootloader  -       32KB         2nd stage bootloader
0x008000    ptable      -       4KB          Partition table
0x009000    nvs         data    24KB         WiFi creds, NVS storage
0x00F000    phy_init    data    4KB          PHY calibration data
0x010000    factory     app     ~1MB         Firmware của bạn
...         (còn lại)           ~15MB        OTA / SPIFFS / FAT nếu cần
```

Config liên quan:
```
CONFIG_PARTITION_TABLE_SINGLE_APP=y   → Chỉ 1 app partition (không có OTA)
CONFIG_PARTITION_TABLE_TWO_OTA=y      → 2 app partitions để OTA update
CONFIG_PARTITION_TABLE_CUSTOM=y       → Dùng file partitions.csv tự viết
CONFIG_PARTITION_TABLE_OFFSET=0x8000  → Partition table ở địa chỉ 0x8000
```

---

## 8. GPIO trên ESP32-S3

ESP32-S3 có GPIO 0-48 (không phải tất cả đều dùng được):

```
GPIO 0:       Boot mode (kéo thấp = download mode)  → KHÔNG dùng tùy tiện
GPIO 19-20:   USB D-/D+  → Dùng cho USB CDC
GPIO 26-32:   SPI Flash  → KHÔNG dùng (nội bộ)
GPIO 33-37:   Octal PSRAM → KHÔNG dùng (nội bộ)
GPIO 38:      LED trên DevKitC v1.1
GPIO 43-44:   UART0 TX/RX → Serial monitor

GPIO còn lại: Dùng tự do cho camera, I2C, SPI, v.v.
```

**Camera trong project này dùng GPIO:**
```
PWDN=38, RESET=47, XCLK=15
SIOD=4(SDA), SIOC=5(SCL)   ← I2C giao tiếp với sensor
D0-D7 = 11,9,8,10,12,18,17,16  ← Data lines
VSYNC=6, HREF=7, PCLK=13
```

---

## Tiếp theo — Đọc theo thứ tự này:

1. **`01_CMakeLists_explained.md`** — Hệ thống build, tại sao cần từng dòng
2. **`02_sdkconfig_explained.md`** — File config lớn nhất, từng group quan trọng
3. **`03_PSRAM_deep_dive.md`** — PSRAM từ A-Z, tại sao crash và cách fix
4. **`04_vscode_tools_explained.md`** — IDE, IntelliSense, clangd
5. **`05_main_c_explained.md`** — Giải thích từng dòng code trong main.c

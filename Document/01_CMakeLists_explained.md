# CMakeLists.txt — Hệ thống Build của ESP-IDF

> **Đọc trước**: `00_ESP32S3_SYSTEM_OVERVIEW.md` (phần 6)

---

## CMake là gì? (1 phút)

CMake không compile code — nó tạo ra **"kịch bản"** để GCC compiler làm việc.

```
Bạn viết:       CMakeLists.txt
CMake tạo ra:   build/build.ninja  (kịch bản cho Ninja build tool)
Ninja chạy:     xtensa-esp32s3-elf-gcc compile từng file .c → .bin
```

Lệnh `idf.py build` = gọi CMake + Ninja phía sau hậu trường.

---

## File 1: Root `/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EdgeVision)
```

### Dòng 1: `cmake_minimum_required(VERSION 3.16)`

Yêu cầu CMake phiên bản tối thiểu 3.16. ESP-IDF 5.x cần ít nhất 3.16.
Nếu máy bạn cài CMake cũ hơn → báo lỗi ngay, không build được.

Kiểm tra version CMake của bạn:
```bash
cmake --version
```

### Dòng 2: `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`

Đây là dòng **cốt lõi nhất**. Nó làm 4 việc:

1. Load toàn bộ build system của ESP-IDF vào CMake
2. Kích hoạt các hàm đặc biệt như `idf_component_register()`, `idf_build_set_property()`
3. Scan tự động tìm components trong `components/` và `managed_components/`
4. Setup toolchain cho Xtensa LX7 (compiler, linker flags v.v.)

`$ENV{IDF_PATH}` đọc biến môi trường của hệ thống:
```bash
# Trên máy này (xem .vscode/settings.json):
IDF_PATH = /Users/phuocvnd/.espressif/release-v5.5/esp-idf
```

Nếu `IDF_PATH` không được set → CMake không tìm được file → lỗi.
Lệnh `idf.py build` tự set biến này khi chạy — đó là lý do phải dùng `idf.py` chứ không chạy `cmake` trực tiếp.

### Dòng 3: `project(EdgeVision)`

- Đặt tên project = **"EdgeVision"**
- Tên này sẽ thành tên file output: `build/EdgeVision.bin`, `build/EdgeVision.elf`
- **Phải gọi AFTER `include(...)`** — nếu đặt trước sẽ báo lỗi

### Dòng tùy chọn quan trọng (hiện project chưa có):

```cmake
# Chỉ build component thực sự được dùng — giảm binary size, tăng tốc build
idf_build_set_property(MINIMAL_BUILD ON)
```

Đặt dòng này **trước** `project(EdgeVision)`. Khi không có dòng này, ESP-IDF build tất
cả component kể cả không dùng → binary lớn hơn cần thiết.

---

## File 2: `main/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
```

### Tại sao cần file này?

Trong ESP-IDF, mọi thư mục con đều là một **"component"**. Mỗi component phải tự
khai báo với hệ thống build qua `idf_component_register()`.

Thư mục `main/` là component đặc biệt — nó là **entry point** của app (chứa `app_main()`).

### `SRCS "main.c"`

Danh sách **tất cả file .c** cần compile trong component này.

Khi bạn thêm file mới, phải thêm vào đây:
```cmake
SRCS "main.c" "camera_init.c" "wifi_manager.c"
```

**Lỗi phổ biến nhất**: Tạo file `camera_init.c` mới nhưng quên thêm vào `SRCS`
→ Linker báo `undefined reference to 'camera_init_func'`

### `INCLUDE_DIRS "."`

Thư mục chứa header files `.h` mà các component khác có thể #include.

`"."` = thư mục `main/` hiện tại.

Nếu bạn tạo subfolder:
```cmake
INCLUDE_DIRS "." "include"   # Thêm main/include/ vào include path
```

### Các parameter sẽ cần khi mở rộng:

```cmake
idf_component_register(
    SRCS "main.c" "camera_init.c"
    INCLUDE_DIRS "." "include"
    REQUIRES esp_wifi nvs_flash    # Dependency với IDF built-in components
    LDFRAGMENTS "psram_iram.lf"   # Linker fragments (xem file 03)
)
```

**`REQUIRES`**: Khi `main.c` dùng `#include "esp_wifi.h"`, CMake cần biết header đó
ở đâu. `REQUIRES esp_wifi` nói CMake: "tôi cần component esp_wifi".

Với `managed_components` (trong `idf_component.yml`), thường không cần `REQUIRES`
vì ESP-IDF tự resolve — nhưng nếu có lỗi header not found, thêm vào `REQUIRES`.

---

## Cách ESP-IDF tổ chức Components

```
EdgeVision/
├── CMakeLists.txt                 ← Root: tên project
├── main/
│   └── CMakeLists.txt             ← Component "main": entry point
│
├── components/                    ← (Optional) Component tự viết
│   └── my_sensor/
│       ├── CMakeLists.txt         ← khai báo component
│       ├── my_sensor.c
│       └── include/
│           └── my_sensor.h
│
└── managed_components/            ← Auto-download từ idf_component.yml
    └── espressif__esp32-camera/
        └── CMakeLists.txt         ← ESP-IDF quản lý, đừng sửa
```

Khi `idf.py build`:
1. CMake đọc root `CMakeLists.txt`
2. Scan tìm tất cả `CMakeLists.txt` trong `components/` và `managed_components/`
3. Build dependency graph (ai cần ai)
4. Compile theo đúng thứ tự (dependency trước, dependent sau)

---

## Quy trình build thực tế

```bash
# === Lần đầu tiên ===
idf.py set-target esp32s3    # Tạo sdkconfig mặc định cho target ESP32-S3
idf.py build                 # CMake configure + compile toàn bộ

# === Sau khi chỉ sửa file .c ===
idf.py build                 # Incremental build: chỉ compile file thay đổi

# === Sau khi sửa CMakeLists.txt hoặc sdkconfig ===
idf.py build                 # CMake tự detect thay đổi và re-configure

# === Build sạch hoàn toàn ===
idf.py fullclean             # Xóa toàn bộ build/
idf.py build                 # Build lại từ đầu

# === Flash + xem serial monitor ===
idf.py flash monitor         # Flash firmware và mở monitor cùng lúc

# === Chỉ xem serial monitor (không flash) ===
idf.py monitor
```

---

## Debug khi CMake/Build lỗi

**Lỗi 1: `No such file or directory 'main.c'`**
→ Tên file trong `SRCS` sai — kiểm tra chính tả

**Lỗi 2: `undefined reference to 'esp_camera_init'`**
→ Linker không tìm được function — thiếu dependency trong `idf_component.yml` hoặc `REQUIRES`

**Lỗi 3: `fatal error: 'esp_camera.h' file not found`**
→ Compiler không tìm được header — component chưa được add, hoặc `idf.py update-dependencies` chưa chạy

**Lỗi 4: `IDF_PATH is not set`**
→ Không dùng `idf.py`, hoặc environment chưa được source
→ Fix: `source /path/to/esp-idf/export.sh` trước khi build

**Command xem chi tiết lỗi:**
```bash
idf.py build 2>&1 | grep -E "error:|Error:" | head -20
```

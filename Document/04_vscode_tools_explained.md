# VS Code & Công cụ — IDE, IntelliSense, clangd

---

## Tổng quan: Các file liên quan đến IDE

```
EdgeVision/
├── .vscode/
│   ├── settings.json           ← Config VS Code + ESP-IDF extension
│   ├── c_cpp_properties.json   ← IntelliSense (Microsoft C/C++ extension)
│   └── launch.json             ← Debug configuration (JTAG)
├── .clangd                     ← Config cho clangd language server
└── .devcontainer/
    ├── Dockerfile               ← Docker image để build trong container
    └── devcontainer.json        ← VS Code Dev Container config
```

**Lưu ý**: Tất cả file trong `.vscode/` và `.clangd` **không ảnh hưởng đến build/firmware**.
Chúng chỉ giúp IDE hiểu code để hiển thị IntelliSense, autocomplete, diagnostics.

---

## File 1: `.vscode/settings.json`

```json
{
  "C_Cpp.intelliSenseEngine": "default",
  "idf.extensionActivationMode": "always",
  "idf.openOcdConfigs": [
    "board/esp32s3-bridge.cfg"
  ],
  "idf.port": "/dev/tty.usbserial-210",
  "idf.currentSetup": "/Users/phuocvnd/.espressif/release-v5.5/esp-idf",
  "idf.customExtraVars": {
    "IDF_TARGET": "esp32s3"
  },
  "clangd.path": "/Users/phuocvnd/.espressif/tools/esp-clang/...",
  "clangd.arguments": [
    "--background-index",
    "--query-driver=**",
    "--compile-commands-dir=.../build"
  ],
  "idf.flashType": "UART"
}
```

### Giải thích từng key:

**`"C_Cpp.intelliSenseEngine": "default"`**
Dùng Microsoft IntelliSense engine. Khi dùng clangd, có thể set thành `"disabled"`
để tránh conflict giữa 2 engine. Nhưng để `"default"` thường vẫn ổn.

**`"idf.extensionActivationMode": "always"`**
ESP-IDF VS Code extension luôn active (không chờ mở file .c mới kích hoạt).
Giúp sidebar ESP-IDF luôn hiện ra khi mở project.

**`"idf.openOcdConfigs": ["board/esp32s3-bridge.cfg"]`**
Config cho OpenOCD khi debug bằng JTAG.
`esp32s3-bridge.cfg` = dùng built-in USB-JTAG bridge của ESP32-S3 (không cần adapter ngoài).

Các config khác hay dùng:
```json
"board/esp32s3-builtin.cfg"    // USB JTAG tích hợp trên chip
"board/esp32s3-bridge.cfg"     // USB JTAG qua bridge
"interface/ftdi/esp32_devkitj_v1.cfg"  // FTDI adapter ngoài
```

**`"idf.port": "/dev/tty.usbserial-210"`**
Cổng Serial để flash và monitor. Trên macOS: `/dev/tty.usbserial-XXX` hoặc `/dev/tty.SLAB_USBtoUART`.

Tìm port của bạn:
```bash
ls /dev/tty.usb*     # macOS
ls /dev/ttyUSB*      # Linux
# Windows: COM3, COM4... (xem trong Device Manager)
```

**`"idf.currentSetup": "/Users/phuocvnd/.espressif/release-v5.5/esp-idf"`**
Đường dẫn đến ESP-IDF đã cài đặt. Extension dùng để tìm toolchain, scripts.
Nếu bạn cài nhiều version IDF, đổi dòng này để switch version.

**`"idf.customExtraVars": { "IDF_TARGET": "esp32s3" }`**
Set biến môi trường khi chạy các lệnh IDF từ VS Code. Giúp VS Code biết đang
target chip nào để đề xuất config phù hợp.

**`"clangd.path"` và `"clangd.arguments"`**
Config cho clangd language server (xem phần `.clangd` bên dưới).
`--compile-commands-dir` trỏ đến `build/` nơi chứa `compile_commands.json` sau build.

**`"idf.flashType": "UART"`**
Cách flash firmware: `"UART"` hoặc `"JTAG"`.
- `"UART"`: Qua cổng Serial/USB-to-UART (phổ biến, không cần hardware đặc biệt)
- `"JTAG"`: Qua JTAG (nhanh hơn, có thể debug lúc flash, cần OpenOCD)

---

## File 2: `.vscode/c_cpp_properties.json`

```json
{
  "configurations": [
    {
      "name": "ESP-IDF",
      "compilerPath": "/Users/phuocvnd/.espressif/tools/xtensa-esp-elf/.../xtensa-esp32s3-elf-gcc",
      "compileCommands": "${config:idf.buildPath}/compile_commands.json",
      "intelliSenseMode": "gcc-x86",
      "includePath": ["${workspaceFolder}/**"]
    }
  ]
}
```

File này config cho **Microsoft C/C++ IntelliSense extension**.

**`"compilerPath"`**: Đường dẫn đến GCC compiler cho ESP32-S3 (`xtensa-esp32s3-elf-gcc`).
IntelliSense cần biết compiler nào đang dùng để hiểu compiler-specific extensions.

**`"compileCommands"`**: Trỏ đến `build/compile_commands.json` — file được sinh tự động
khi build. Nó chứa chính xác lệnh compile cho TỪNG file .c, bao gồm tất cả `-I` include
paths và `-D` defines. Đây là cách IntelliSense biết `#include "esp_camera.h"` ở đâu.

**`"intelliSenseMode": "gcc-x86"`**: Hơi sai — đáng lẽ nên là `"gcc-arm"` hoặc
để trống. Nhưng thường không gây vấn đề thực tế vì `compileCommands` đã override.

**Thực tế**: Nếu bạn đang dùng clangd (xem `.clangd`), file này ít quan trọng hơn.
clangd đọc trực tiếp `compile_commands.json` và cho IntelliSense tốt hơn cho ESP-IDF.

---

## File 3: `.clangd`

```yaml
CompileFlags:
    Remove: [-f*, -m*]
```

**clangd** là một language server protocol (LSP) implementation của LLVM. Nó cung cấp:
- IntelliSense thông minh hơn Microsoft C/C++ extension
- Xem định nghĩa hàm (Go to Definition) chính xác hơn
- Error highlighting theo compiler thực sự
- Auto-complete dựa trên context

**`Remove: [-f*, -m*]`**

Clangd dùng compile_commands.json để biết cách compile. Nhưng clangd là LLVM clang,
không phải GCC. Một số flag của GCC (xtensa-esp32s3-elf-gcc) clangd không hiểu và
gây ra warning/error giả:
- `-f*` flags: GCC-specific optimization/behavior flags (vd: `-fno-rtti`, `-finline-functions`)
- `-m*` flags: Architecture-specific flags (vd: `-mlongcalls` cho Xtensa)

`Remove: [-f*, -m*]` = nói clangd **bỏ qua** tất cả flags bắt đầu bằng `-f` và `-m`
khi parse compile_commands.json. Kết quả: ít false-positive error hơn trong IDE.

**Cách hoạt động:**
1. Build project lần đầu: `idf.py build`
2. File `build/compile_commands.json` được tạo ra
3. clangd đọc file này, bỏ qua `-f*` và `-m*` flags
4. clangd index toàn bộ code → IntelliSense hoạt động
5. Bạn sẽ thấy gợi ý autocomplete khi gõ `esp_camera_` hoặc `heap_caps_`

**Lưu ý quan trọng**: Sau mỗi lần:
- Thay đổi `idf_component.yml` + `idf.py update-dependencies`
- Thêm file `.c` mới vào `CMakeLists.txt`

→ Phải build lại để cập nhật `compile_commands.json` → clangd mới nhận ra file/header mới.

---

## File 4: `.vscode/launch.json` (JTAG Debug)

File này config debug session cho VS Code:
```json
{
  "configurations": [
    {
      "type": "gdbtarget",
      "name": "ESP-IDF Debug",
      "gdbPath": "...xtensa-esp32s3-elf-gdb",
      ...
    }
  ]
}
```

Cho phép bạn:
- Đặt breakpoint trong code C
- Step through code từng dòng
- Xem giá trị biến realtime
- Xem backtrace khi crash

**Điều kiện để debug JTAG hoạt động**:
1. `idf.flashType` = `"JTAG"` trong settings.json
2. OpenOCD đang chạy (VS Code extension tự start)
3. Board được kết nối qua USB (ESP32-S3 có built-in USB JTAG)

---

## File 5: `.devcontainer/` (Dev Container)

```
.devcontainer/
├── Dockerfile          ← Image với ESP-IDF đã cài sẵn
└── devcontainer.json   ← Config cho VS Code Dev Container extension
```

Dev Container cho phép bạn build project trong **Docker container** thay vì cài
ESP-IDF lên máy thật. Hữu ích khi:
- Làm việc nhóm — mọi người dùng cùng environment
- CI/CD pipeline
- Máy mới, không muốn cài ESP-IDF

Cách dùng: VS Code → `Reopen in Container` → VS Code build Docker image và mở
project bên trong container với ESP-IDF đã sẵn sàng.

---

## Workflow hàng ngày với VS Code

```
1. Mở project trong VS Code
2. Đảm bảo đã build ít nhất 1 lần (để có compile_commands.json)
3. Viết code → clangd tự động gợi ý và check lỗi
4. Ctrl+Shift+P → "ESP-IDF: Build your project" hoặc dùng terminal: idf.py build
5. Ctrl+Shift+P → "ESP-IDF: Flash your project" hoặc: idf.py flash
6. Ctrl+Shift+P → "ESP-IDF: Monitor your device" hoặc: idf.py monitor
7. Hoặc tất cả cùng lúc: idf.py flash monitor
```

**Phím tắt hay dùng:**
- `F12` hoặc `Ctrl+Click` → Go to Definition (cần clangd đã index)
- `Ctrl+Shift+F` → Search trong toàn bộ project
- `Ctrl+`` ` → Toggle terminal

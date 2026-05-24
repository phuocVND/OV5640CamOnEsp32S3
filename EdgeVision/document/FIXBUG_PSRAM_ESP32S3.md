# BÁO CÁO FIX BUG: PSRAM & Camera trên ESP32-S3 (IDF v5.5)

**Chip:** ESP32-S3 N8R8 (8MB Flash, 8MB PSRAM Octal)  
**IDF Version:** ESP-IDF v5.5  
**Camera:** OV2640 qua interface DVP  
**Vấn đề:** Runtime crash / lỗi khi sử dụng PSRAM với camera

---

## 1. MÔ TẢ LỖI GỐC

Khi chạy firmware trên thiết bị, gặp các lỗi runtime:

- Camera init fail hoặc crash ngay sau `app_main()` bắt đầu
- PSRAM báo không available dù đã enable trong config
- Frame buffer không được cấp phát vào PSRAM như mong muốn
- Trong một số trường hợp: **Guru Meditation Error** do ICache corrupt

---

## 2. PHÂN TÍCH NGUYÊN NHÂN

### 2.1 Nguyên nhân chính: `SPIRAM_BOOT_INIT` không được bật trong `sdkconfig`

**ESP-IDF v5.5** tách PSRAM init ra làm **2 giai đoạn riêng biệt**:

| Config | Thực hiện khi nào | Tác dụng |
|--------|-------------------|----------|
| `CONFIG_SPIRAM_BOOT_HW_INIT=y` | Trong `cpu_start.c` (trước FreeRTOS) | Chỉ init phần cứng PSRAM chip (`esp_psram_chip_init()`) |
| `CONFIG_SPIRAM_BOOT_INIT=y` | Cũng trong `cpu_start.c`, SAU hardware init | Gọi `esp_psram_init()` đầy đủ: mapping vào MMU, add vào heap allocator |

Tình trạng thực tế trong `sdkconfig`:
```
CONFIG_SPIRAM_BOOT_HW_INIT=y
# CONFIG_SPIRAM_BOOT_INIT is not set    ← LỖI: thiếu dòng này
```

Dù `sdkconfig.defaults.esp32s3` đã khai báo đúng `CONFIG_SPIRAM_BOOT_INIT=y`, khi generate lại `sdkconfig` lần đầu bị sai, và file `sdkconfig` không được sync lại.

**Hậu quả:** PSRAM phần cứng được init nhưng **không được add vào heap**. Khi `app_main()` chạy, `MALLOC_CAP_SPIRAM` không có vùng nhớ nào → camera driver không allocate được frame buffer → crash.

---

### 2.2 Nguyên nhân phụ: Manual `esp_psram_init()` trong `app_main()` gây ICache corrupt

Để "workaround" PSRAM chưa được init, code đã thực hiện init thủ công trong `app_main()`:

```c
// ❌ CODE SAI - Phiên bản cũ
#include "esp_private/esp_psram_extram.h"
#include "esp32s3/rom/cache.h"

// Gọi thủ công trong app_main()
esp_err_t psram_ret = esp_psram_init();         // Vấn đề 1
flush_icache_all();                              // Vấn đề 2
esp_psram_extram_add_to_heap_allocator();        // Vấn đề 3
```

**Vấn đề 1 - Gọi `esp_psram_init()` lần 2:**  
Khi `SPIRAM_BOOT_HW_INIT=y`, chip đã được init hardware trong `cpu_start.c`. Gọi `esp_psram_init()` lần nữa từ app có thể gây conflict trạng thái internal state (`s_psram_ctx.is_chip_initialised`).

**Vấn đề 2 - Gọi `Cache_Invalidate_ICache_All()` từ flash:**  
Hàm này invalidate TOÀN BỘ ICache. Nếu bản thân code đang execute từ flash (không phải IRAM), việc flush ICache trong khi đang fetch instruction từ flash = **corrupt instruction stream** → Guru Meditation.  
Dù được đặt trong `IRAM_ATTR`, caller và callee chain vẫn có thể có code ở flash.

**Vấn đề 3 - Private API `esp_psram_extram.h`:**  
`esp_private/esp_psram_extram.h` là internal API, không stable. Khi `SPIRAM_BOOT_INIT=y` được bật đúng, IDF đã tự gọi `esp_psram_extram_add_to_heap_allocator()` qua `ESP_SYSTEM_INIT_FN(add_psram_to_heap, ...)`. Gọi thêm lần nữa = double-add heap region.

---

### 2.3 Nguyên nhân phụ: PSRAM Speed không đúng

File backup `sdkconfig.defaults.esp32s3.bak` có:
```
CONFIG_SPIRAM_SPEED_80M=y   ← Quá cao cho octal PSRAM mode ban đầu
```

Đã đổi xuống:
```
CONFIG_SPIRAM_SPEED_40M=y   ← An toàn, ổn định hơn
```

ESP32-S3 với Octal PSRAM ở 80MHz đòi hỏi timing chặt chẽ hơn. Ở 40MHz hoạt động ổn định, đủ bandwidth cho camera frame buffer.

---

## 3. CÁC FIX ĐÃ THỰC HIỆN

### Fix 1: Bật `CONFIG_SPIRAM_BOOT_INIT` trong `sdkconfig`

**File:** `sdkconfig` (dòng 1046)

```diff
 CONFIG_SPIRAM_BOOT_HW_INIT=y
-# CONFIG_SPIRAM_BOOT_INIT is not set
+CONFIG_SPIRAM_BOOT_INIT=y
```

Đây là fix cốt lõi. Sau fix này, IDF tự động:
1. Gọi `esp_psram_init()` trong startup trước `app_main()`
2. Gọi `esp_psram_extram_add_to_heap_allocator()` qua system init function
3. PSRAM hoàn toàn available khi `app_main()` bắt đầu

---

### Fix 2: Xóa manual PSRAM init trong `app_main()`

**File:** `main/main.c`

**Xóa các include không cần thiết:**
```diff
-#include "esp_private/esp_psram_extram.h"
-#include "esp32s3/rom/cache.h"
```

**Xóa toàn bộ block init thủ công:**
```diff
-    // --- Deferred PSRAM Init ---
-    ESP_LOGI(TAG, "Initialising PSRAM...");
-    esp_err_t psram_ret = esp_psram_init();
-    if (psram_ret != ESP_OK) {
-        ESP_LOGE(TAG, "esp_psram_init failed: %s", esp_err_to_name(psram_ret));
-    } else {
-        flush_icache_all();
-        esp_err_t heap_ret = esp_psram_extram_add_to_heap_allocator();
-        if (heap_ret != ESP_OK) {
-            ESP_LOGE(TAG, "add_to_heap failed: %s", esp_err_to_name(heap_ret));
-        }
-    }
-    // ---------------------------
```

**Xóa hàm `flush_icache_all()`:**
```diff
-static void IRAM_ATTR flush_icache_all(void)
-{
-    Cache_Invalidate_ICache_All();
-}
```

---

### Fix 3: Thêm PSRAM status check đúng cách

**File:** `main/main.c`

Thay vì log trực tiếp, dùng `esp_psram_is_initialized()` để verify trạng thái:

```c
// ✅ CODE ĐÚNG
#ifdef CONFIG_SPIRAM
ESP_LOGI(TAG, "PSRAM is ENABLED in config");
if (esp_psram_is_initialized()) {
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM initialized OK - Total: %lu bytes, Free: %lu bytes",
             (unsigned long)psram_total, (unsigned long)psram_free);
} else {
    ESP_LOGE(TAG, "PSRAM NOT initialized - check SPIRAM_BOOT_INIT config!");
}
#endif
```

---

### Fix 4: Giảm PSRAM speed từ 80MHz xuống 40MHz

**File:** `sdkconfig.defaults.esp32s3`

```diff
-CONFIG_SPIRAM_SPEED_80M=y
+CONFIG_SPIRAM_SPEED_40M=y
```

---

## 4. TRẠNG THÁI SAU KHI FIX

### Config cuối cùng (`sdkconfig` / `build/config/sdkconfig.h`)

```
# PSRAM Hardware
CONFIG_SPIRAM=1
CONFIG_SPIRAM_MODE_OCT=1           # Octal mode (8MB)
CONFIG_SPIRAM_TYPE_AUTO=1          # Auto-detect chip type
CONFIG_SPIRAM_CLK_IO=30            # GPIO30
CONFIG_SPIRAM_CS_IO=26             # GPIO26
CONFIG_SPIRAM_SPEED_40M=1          # 40MHz - ổn định

# PSRAM Init Flow
CONFIG_SPIRAM_BOOT_HW_INIT=1       # Phase 1: init hardware trong cpu_start
CONFIG_SPIRAM_BOOT_INIT=1          # Phase 2: full init + add to heap ← FIX CHÍNH

# PSRAM Allocation
CONFIG_SPIRAM_USE_CAPS_ALLOC=1     # Cho phép heap_caps_malloc(MALLOC_CAP_SPIRAM)

# Camera
CONFIG_CAMERA_PSRAM_DMA=1          # Camera DMA hỗ trợ PSRAM
```

### Kết quả build

```
blink.bin binary size 0x448c0 bytes (73% free space)
Project build complete. ✓
```

---

## 5. SƠ ĐỒ PSRAM INIT FLOW (ESP-IDF v5.5)

```
Bootloader
    │
    ▼
cpu_start.c (trước FreeRTOS)
    ├── [SPIRAM_BOOT_HW_INIT=y] → esp_psram_chip_init()
    │       Khởi động hardware PSRAM (clock, pin config, octal mode)
    │
    └── [SPIRAM_BOOT_INIT=y]   → esp_psram_init()
            Mapping PSRAM vào virtual address space (MMU)
            Set up vùng nhớ

FreeRTOS khởi động
    │
    ▼
ESP_SYSTEM_INIT_FN(add_psram_to_heap)  [priority 103]
    └── esp_psram_extram_add_to_heap_allocator()
            PSRAM region → heap với MALLOC_CAP_SPIRAM

    │
    ▼
app_main()  ← PSRAM đã sẵn sàng 100%, không cần làm gì thêm
    ├── heap_caps_malloc(size, MALLOC_CAP_SPIRAM)  ✓
    ├── esp_camera_init() với fb_location=CAMERA_FB_IN_PSRAM  ✓
    └── Frame buffer cấp phát trong PSRAM  ✓
```

---

## 6. OUTPUT RUNTIME KỲ VỌNG

Sau khi flash firmware đã fix, serial monitor sẽ hiển thị:

```
========== APP MAIN STARTED ==========

I (xxx) camera_example: ==== PSRAM Information ====
I (xxx) camera_example: PSRAM is ENABLED in config
I (xxx) camera_example: PSRAM initialized OK - Total: 8388608 bytes, Free: 8xxxxxx bytes
I (xxx) camera_example: ===========================
I (xxx) camera_example: Testing GPIO...
I (xxx) camera_example: GPIO set OK
I (xxx) camera_example: Now init camera with PSRAM frame buffer...
I (xxx) camera_example: Calling esp_camera_init with PSRAM frame buffer...
I (xxx) camera_example: ✓ CAMERA INIT SUCCESS! Frame buffer in PSRAM enabled!
I (xxx) camera_example: Starting frame capture task
I (xxx) camera_example: Captured frame: XXXXX bytes, size: 320x240
```

---

## 7. DANH SÁCH FILES ĐÃ CHỈNH SỬA

| File | Thay đổi |
|------|----------|
| `sdkconfig` | Bật `CONFIG_SPIRAM_BOOT_INIT=y` (dòng 1046) |
| `main/main.c` | Xóa manual PSRAM init block, xóa `flush_icache_all()`, xóa private headers, thêm `esp_psram_is_initialized()` check |
| `sdkconfig.defaults.esp32s3` | Đổi `SPIRAM_SPEED_80M` → `SPIRAM_SPEED_40M` |

---

## 8. BÀI HỌC KINH NGHIỆM

1. **Không bao giờ gọi `esp_psram_init()` thủ công** khi `SPIRAM_BOOT_HW_INIT` hoặc `SPIRAM_BOOT_INIT` đã được bật — IDF đã tự lo.

2. **Không gọi `Cache_Invalidate_ICache_All()` từ application code** — đây là ROM API cấp thấp, chỉ an toàn khi gọi từ IRAM trong giai đoạn startup đặc biệt.

3. **`esp_private/` headers là internal API** — không dùng trong application code, dễ bị break qua các version IDF.

4. **`sdkconfig` vs `sdkconfig.defaults`**: `sdkconfig` là file thực sự được dùng khi build. `sdkconfig.defaults` chỉ là template khi tạo mới. Nếu hai file lệch nhau, phải regenerate `sdkconfig` bằng `idf.py set-target <chip>` hoặc sửa trực tiếp `sdkconfig`.

5. **IDF v5.5 thay đổi PSRAM init flow** so với các version cũ hơn (v4.x, v5.0). Khi upgrade IDF, cần review lại PSRAM configuration.

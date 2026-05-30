#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "camera/camera_drv.h"
#include "network/wifi_sta.h"
#include "network/tcp_client.h"
#include "servo/servo_ctrl.h"
#include "lcd/lcd_drv.h"
#include "jpeg_decoder.h"

static const char *TAG = "main";

/* ─── Shared queues (created once in app_main, used across tasks) ────────── */
QueueHandle_t g_frame_queue;   /* camera_task  → tcp_send_task  (camera_fb_t*) */
QueueHandle_t g_cmd_queue;     /* tcp_recv_task → servo_task    (servo_cmd_t)  */

/* ─── LCD pipeline globals ───────────────────────────────────────────────── */
/* camera_task writes JPEG copy → signals g_lcd_sem → lcd_task decodes + draws */
static uint8_t  *g_lcd_jpeg_buf = NULL;  /* PSRAM: JPEG frame copy, LCD_JPEG_BUF_SIZE     */
static size_t    g_lcd_jpeg_len = 0;     /* valid bytes in g_lcd_jpeg_buf                 */
static uint16_t *g_lcd_rgb_buf  = NULL;  /* PSRAM: decoded RGB565 LCD_DECODE_W×H (150KB)  */
static uint16_t *g_lcd_crop_buf = NULL;  /* PSRAM: center-cropped LCD_H_RES×H (82KB)      */
static SemaphoreHandle_t g_lcd_sem   = NULL;  /* binary: new JPEG ready for lcd_task       */
static SemaphoreHandle_t g_lcd_mutex = NULL;  /* mutex:  protects g_lcd_jpeg_buf/len       */

/* ═══════════════════════════════════════════════════════════════════════════
 * TASK: camera_task  — Core 1
 *
 * Why Core 1: Camera DMA is memory-bus intensive. Isolating it from the WiFi
 * stack (Core 0) prevents interrupt latency spikes that cause frame drops.
 *
 * Flow: capture → try push to g_frame_queue → if queue full, return fb
 *       (depth=1 means: always send the freshest frame, never buffer stale)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void camera_task(void *arg)
{
    ESP_LOGI(TAG, "[Core %d] camera_task started", xPortGetCoreID());

    uint32_t n_captured = 0, n_null = 0, n_evicted = 0;

    while (1) {
        camera_fb_t *fb = camera_drv_capture();
        if (!fb) {
            n_null++;
            ESP_LOGW(TAG, "[CAM] capture=NULL null_count=%lu (buffer pool empty?)", n_null);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        n_captured++;

        /* Feed LCD pipeline: copy every LCD_FRAME_SKIP-th frame to g_lcd_jpeg_buf.
         * Non-blocking mutex try — if lcd_task is decoding, skip this frame.
         * Camera performance is unaffected (no blocking wait). */
        static int s_lcd_skip = 0;
        if (++s_lcd_skip >= LCD_FRAME_SKIP && g_lcd_jpeg_buf) {
            s_lcd_skip = 0;
            if (fb->len <= LCD_JPEG_BUF_SIZE &&
                xSemaphoreTake(g_lcd_mutex, 0) == pdTRUE) {
                memcpy(g_lcd_jpeg_buf, fb->buf, fb->len);
                g_lcd_jpeg_len = fb->len;
                xSemaphoreGive(g_lcd_mutex);
                xSemaphoreGive(g_lcd_sem);  /* notify lcd_task (binary: no-op if already pending) */
            }
        }

        /* Ring buffer: try push, if full → evict OLDEST frame first,
         * then push new one — always keep freshest frames in queue */
        UBaseType_t q_waiting = uxQueueMessagesWaiting(g_frame_queue);
        if (xQueueSend(g_frame_queue, &fb, 0) != pdTRUE) {
            camera_fb_t *old = NULL;
            if (xQueueReceive(g_frame_queue, &old, 0) == pdTRUE) {
                camera_drv_fb_return(old);   /* return evicted buffer */
                n_evicted++;
            }
            xQueueSend(g_frame_queue, &fb, 0);  /* now has room */
        }

        /* Print stats every 30 frames */
        if (n_captured % 30 == 0) {
            ESP_LOGI(TAG, "[CAM-STATS] captured=%lu null=%lu evicted=%lu queue=%u/%d",
                     n_captured, n_null, n_evicted, (unsigned)q_waiting, FRAME_QUEUE_DEPTH);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TASK: tcp_send_task  — Core 0
 *
 * Why Core 0: Network I/O calls into lwip which is pinned to Core 0 with
 * the WiFi stack. Running here avoids cross-core context switches on every
 * send() call.
 *
 * Flow: wait for frame pointer → send over TCP → return fb to camera driver
 * ═══════════════════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════════════════
 * TASK: lcd_task  — Core 1
 *
 * Waits for a JPEG frame copy from camera_task (via binary semaphore), then:
 *  1. Decodes JPEG at 1/2 scale → 320×240 RGB565 (espressif__esp_jpeg)
 *  2. Crops center 170 columns of 320 → 170×240 contiguous buffer
 *  3. Draws 170×240 centered on 170×320 portrait LCD (y_offset=40)
 *
 * Display layout on 170×320 LCD (portrait):
 *   Row 0-39:   black border (top)
 *   Row 40-279: camera preview 170×240
 *   Row 280-319: black border (bottom)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void lcd_task(void *arg)
{
    ESP_LOGI(TAG, "[Core %d] lcd_task started", xPortGetCoreID());

    esp_err_t lcd_ret = lcd_drv_init();
    if (lcd_ret != ESP_OK) {
        ESP_LOGE(TAG, "[LCD] init FAILED: %s — check wiring (SCL=%d MOSI=%d DC=%d CS=%d RST=%d)",
                 esp_err_to_name(lcd_ret),
                 LCD_PIN_SCL, LCD_PIN_SDA, LCD_PIN_DC, LCD_PIN_CS, LCD_PIN_RES);
        vTaskDelete(NULL);
        return;
    }

    /* Black background */
    lcd_drv_fill(0x0000);

    /* Static header — drawn once */
    /* Row 0 (y=0):  title in CYAN */
    lcd_drv_puts(0,  0, "EdgeVision", 0x07FF, 0x0000);
    /* Row 1 (y=16): subtitle in YELLOW */
    lcd_drv_puts(0, 16, "ST7789 170x320", 0xFFE0, 0x0000);
    /* Row 2 (y=32): separator */
    lcd_drv_puts(0, 32, "----------", 0x4208, 0x0000);

    /* Row 9 (y=144): separator */
    lcd_drv_puts(0, 144, "----------", 0x4208, 0x0000);
    /* Row 10 (y=160): color test squares */
    /* Draw colored rectangles to verify color accuracy */
    {
        /* Fill 5 colored squares at y=162, each 16×16 px */
        static const uint16_t test_colors[] = {
            0xF800, /* RED   */
            0x07E0, /* GREEN */
            0x001F, /* BLUE  */
            0xFFFF, /* WHITE */
            0xFFE0, /* YELLOW */
        };
        static const char *test_labels[] = {"R","G","B","W","Y"};
        for (int i = 0; i < 5; i++) {
            lcd_drv_puts((uint16_t)(i * 32), 160, test_labels[i],
                         test_colors[i], 0x0000);
        }
    }
    /* Row 11 (y=176): label */
    lcd_drv_puts(0, 176, "colors above^", 0x4208, 0x0000);

    ESP_LOGI(TAG, "[LCD] init OK — entering text display loop");

    uint32_t uptime_sec = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uptime_sec++;

        size_t free_heap  = esp_get_free_heap_size();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        /* Row 3 (y=48):  uptime  — GREEN */
        lcd_drv_printf(0,  48, 0x07E0, 0x0000, "Up: %5lus   ", uptime_sec);
        /* Row 4 (y=64):  heap    — WHITE */
        lcd_drv_printf(0,  64, 0xFFFF, 0x0000, "Heap:%4uKB  ", (unsigned)(free_heap  / 1024));
        /* Row 5 (y=80):  psram   — WHITE */
        lcd_drv_printf(0,  80, 0xFFFF, 0x0000, "PSRM:%4uKB  ", (unsigned)(free_psram / 1024));
        /* Row 6 (y=96):  core    — WHITE */
        lcd_drv_printf(0,  96, 0xFFFF, 0x0000, "Core: %d       ", xPortGetCoreID());
        /* Row 7 (y=112): task stack high-water — WHITE */
        lcd_drv_printf(0, 112, 0xFFFF, 0x0000, "Stk:%4uB    ",
                       (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

        if (uptime_sec % 10 == 1) {
            ESP_LOGI(TAG, "[LCD-TXT] up=%lus heap=%uKB psram=%uKB",
                     uptime_sec, (unsigned)(free_heap/1024), (unsigned)(free_psram/1024));
        }
    }
}


static void tcp_send_task(void *arg)
{
    ESP_LOGI(TAG, "[Core %d] tcp_send_task started", xPortGetCoreID());
    tcp_send_connect();

    uint32_t n_sent = 0, n_fail = 0;
    TickType_t last_stats = xTaskGetTickCount();

    while (1) {
        camera_fb_t *fb = NULL;
        /* Block until a frame is available */
        if (xQueueReceive(g_frame_queue, &fb, portMAX_DELAY) != pdTRUE) continue;

        esp_err_t ret = tcp_send_frame(fb);

        /* CRITICAL: return buffer BEFORE reconnect to prevent buffer pool starvation */
        camera_drv_fb_return(fb);

        if (ret == ESP_OK) {
            n_sent++;
            ESP_LOGD(TAG, "[SEND] frame=%lu size=%u bytes queue=%u",
                     n_sent, (unsigned)fb->len,
                     (unsigned)uxQueueMessagesWaiting(g_frame_queue));
        } else {
            n_fail++;
            ESP_LOGW(TAG, "[SEND-FAIL] sent=%lu fail=%lu — reconnecting AFTER buffer returned",
                     n_sent, n_fail);
            /* Reconnect only AFTER fb is returned — buffer pool safe now */
            tcp_send_reconnect();
        }

        /* Periodic stats every 10 seconds */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stats) >= pdMS_TO_TICKS(10000)) {
            ESP_LOGI(TAG, "[SEND-STATS] sent=%lu fail=%lu queue=%u/%d",
                     n_sent, n_fail,
                     (unsigned)uxQueueMessagesWaiting(g_frame_queue),
                     FRAME_QUEUE_DEPTH);
            last_stats = now;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TASK: tcp_recv_task  — Core 0
 *
 * Why Core 0: Same reason as tcp_send_task (lwip affinity).
 * Uses a separate TCP connection (TCP_PORT_CMD) so recv never blocks send.
 *
 * Flow: recv servo command → push to g_cmd_queue
 * ═══════════════════════════════════════════════════════════════════════════ */
static void tcp_recv_task(void *arg)
{
    ESP_LOGI(TAG, "[Core %d] tcp_recv_task started", xPortGetCoreID());
    tcp_recv_connect();

    while (1) {
        servo_cmd_t cmd;
        if (tcp_recv_cmd(&cmd) == ESP_OK) {
            /* Overwrite queue if full — always use latest command */
            xQueueOverwrite(g_cmd_queue, &cmd);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TASK: servo_task  — Core 1
 *
 * Why Core 1: PWM update calls are fast (register writes). Keeping servo on
 * Core 1 with highest app priority ensures smooth movement even when network
 * tasks are busy on Core 0.
 *
 * Flow: wait for command → update all 6 servo angles
 * ═══════════════════════════════════════════════════════════════════════════ */
static void servo_task(void *arg)
{
    ESP_LOGI(TAG, "[Core %d] servo_task started", xPortGetCoreID());

    while (1) {
        servo_cmd_t cmd;
        if (xQueueReceive(g_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            servo_ctrl_set_all(cmd.angle);
            ESP_LOGD(TAG, "Servo: %.1f %.1f %.1f %.1f %.1f %.1f",
                     cmd.angle[0], cmd.angle[1], cmd.angle[2],
                     cmd.angle[3], cmd.angle[4], cmd.angle[5]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * app_main — runs on Core 0 at startup
 * ═══════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG, "========== EdgeVision starting ==========");

    /* 1. Create inter-task queues */
    g_frame_queue = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(camera_fb_t *));
    g_cmd_queue   = xQueueCreate(CMD_QUEUE_DEPTH,   sizeof(servo_cmd_t));
    configASSERT(g_frame_queue && g_cmd_queue);

    /* 2. Allocate LCD pipeline buffers in PSRAM */
    g_lcd_jpeg_buf = heap_caps_malloc(LCD_JPEG_BUF_SIZE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_lcd_rgb_buf  = heap_caps_malloc(LCD_DECODE_W * LCD_DECODE_H * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_lcd_crop_buf = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_lcd_sem   = xSemaphoreCreateBinary();
    g_lcd_mutex = xSemaphoreCreateMutex();
    configASSERT(g_lcd_jpeg_buf && g_lcd_rgb_buf && g_lcd_crop_buf);
    configASSERT(g_lcd_sem && g_lcd_mutex);
    ESP_LOGI(TAG, "LCD buffers: jpeg=%uKB rgb=%uKB crop=%uKB",
             LCD_JPEG_BUF_SIZE / 1024,
             (LCD_DECODE_W * LCD_DECODE_H * 2) / 1024,
             (LCD_H_RES * LCD_V_RES * 2) / 1024);

    /* 3. Init hardware: camera + servos */
    ESP_ERROR_CHECK(camera_drv_init());
    ESP_ERROR_CHECK(servo_ctrl_init());

    /* 4a. Spawn camera + LCD tasks immediately — they do NOT need WiFi.
     *     LCD shows live preview regardless of network status. */
    xTaskCreatePinnedToCore(camera_task, "cam", TASK_STACK_CAMERA, NULL, TASK_PRIO_CAMERA, NULL, TASK_CORE_CAMERA);
    xTaskCreatePinnedToCore(lcd_task,    "lcd", TASK_STACK_LCD,    NULL, TASK_PRIO_LCD,    NULL, TASK_CORE_LCD);

    /* 4b. Start WiFi and wait for connection before spawning network tasks */
    wifi_sta_init();
    if (!wifi_sta_wait_connected()) {
        ESP_LOGE(TAG, "WiFi failed — network tasks skipped, LCD/camera continue.");
        return;  /* lcd_task and camera_task already running — they continue */
    }

    /* 5. Spawn network + servo tasks after WiFi is up */
    xTaskCreatePinnedToCore(tcp_send_task, "tcp_send", TASK_STACK_TCP_SEND, NULL, TASK_PRIO_TCP_SEND, NULL, TASK_CORE_TCP_SEND);
    xTaskCreatePinnedToCore(tcp_recv_task, "tcp_recv", TASK_STACK_TCP_RECV, NULL, TASK_PRIO_TCP_RECV, NULL, TASK_CORE_TCP_RECV);
    xTaskCreatePinnedToCore(servo_task,    "servo",    TASK_STACK_SERVO,    NULL, TASK_PRIO_SERVO,    NULL, TASK_CORE_SERVO);

    ESP_LOGI(TAG, "All tasks launched");
    /* app_main returns — FreeRTOS scheduler takes over */
}

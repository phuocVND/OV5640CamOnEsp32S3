#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "app_config.h"
#include "camera/camera_drv.h"
#include "network/wifi_sta.h"
#include "network/tcp_client.h"
#include "servo/servo_ctrl.h"

static const char *TAG = "main";

/* ─── Shared queues (created once in app_main, used across tasks) ────────── */
QueueHandle_t g_frame_queue;   /* camera_task  → tcp_send_task  (camera_fb_t*) */
QueueHandle_t g_cmd_queue;     /* tcp_recv_task → servo_task    (servo_cmd_t)  */

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

    /* 2. Init hardware: camera + servos */
    ESP_ERROR_CHECK(camera_drv_init());
    ESP_ERROR_CHECK(servo_ctrl_init());

    /* 3. Start WiFi and wait for connection before spawning network tasks */
    wifi_sta_init();
    if (!wifi_sta_wait_connected()) {
        ESP_LOGE(TAG, "WiFi failed. Halting.");
        return;
    }

    /* 4. Spawn tasks, pinned to their respective cores */
    xTaskCreatePinnedToCore(camera_task,   "cam",      TASK_STACK_CAMERA,   NULL, TASK_PRIO_CAMERA,   NULL, TASK_CORE_CAMERA);
    xTaskCreatePinnedToCore(tcp_send_task, "tcp_send", TASK_STACK_TCP_SEND, NULL, TASK_PRIO_TCP_SEND, NULL, TASK_CORE_TCP_SEND);
    xTaskCreatePinnedToCore(tcp_recv_task, "tcp_recv", TASK_STACK_TCP_RECV, NULL, TASK_PRIO_TCP_RECV, NULL, TASK_CORE_TCP_RECV);
    xTaskCreatePinnedToCore(servo_task,    "servo",    TASK_STACK_SERVO,    NULL, TASK_PRIO_SERVO,    NULL, TASK_CORE_SERVO);

    ESP_LOGI(TAG, "All tasks launched");
    /* app_main returns — FreeRTOS scheduler takes over */
}

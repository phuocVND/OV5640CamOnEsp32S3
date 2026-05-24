#include "tcp_client.h"
#include "app_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "tcp_client";

static int s_send_sock = -1;
static int s_recv_sock = -1;

/* ─── Internal helpers ───────────────────────────────────────────────────── */

static int tcp_connect(const char *ip, uint16_t port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return -1;
    }

    /* Disable Nagle algorithm — send frames immediately, no buffering delay */
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec  = TCP_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (TCP_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "connect(%s:%d) failed: errno=%d", ip, port, errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Connected to %s:%d", ip, port);
    return sock;
}

/* Send exactly 'len' bytes — retries on partial write */
static esp_err_t send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t remaining = len;

    while (remaining > 0) {
        int sent = send(sock, p, remaining, 0);
        if (sent < 0) {
            ESP_LOGE(TAG, "send() failed: errno=%d", errno);
            return ESP_FAIL;
        }
        p         += sent;
        remaining -= sent;
    }
    return ESP_OK;
}

/* Receive exactly 'len' bytes — retries on partial read */
static esp_err_t recv_all(int sock, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t remaining = len;

    while (remaining > 0) {
        int got = recv(sock, p, remaining, 0);
        if (got <= 0) {
            ESP_LOGE(TAG, "recv() failed: got=%d errno=%d", got, errno);
            return ESP_FAIL;
        }
        p         += got;
        remaining -= got;
    }
    return ESP_OK;
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

void tcp_send_connect(void)
{
    while (1) {
        s_send_sock = tcp_connect(TCP_SERVER_IP, TCP_PORT_IMAGE);
        if (s_send_sock >= 0) return;
        ESP_LOGW(TAG, "Retrying image socket in 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void tcp_recv_connect(void)
{
    while (1) {
        s_recv_sock = tcp_connect(TCP_SERVER_IP, TCP_PORT_CMD);
        if (s_recv_sock >= 0) return;
        ESP_LOGW(TAG, "Retrying cmd socket in 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t tcp_send_frame(const camera_fb_t *fb)
{
    if (!fb || !fb->buf || fb->len == 0) return ESP_ERR_INVALID_ARG;

    /* Protocol: [4-byte length BE][JPEG bytes] */
    uint32_t len_be = htonl((uint32_t)fb->len);

    if (send_all(s_send_sock, &len_be, sizeof(len_be)) != ESP_OK ||
        send_all(s_send_sock, fb->buf, fb->len)        != ESP_OK) {

        /* BUG FIX: Do NOT reconnect here — caller must return fb first!
         * Reconnecting inside here holds the fb buffer during entire reconnect
         * duration (~5s), starving the camera buffer pool → capture returns NULL */
        ESP_LOGW(TAG, "Send failed (size=%u) — caller must return fb THEN reconnect",
                 (unsigned)fb->len);
        close(s_send_sock);
        s_send_sock = -1;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void tcp_send_reconnect(void)
{
    ESP_LOGW(TAG, "Reconnecting image socket...");
    tcp_send_connect();
}

esp_err_t tcp_recv_cmd(servo_cmd_t *cmd)
{
    /* Protocol: 6 × float32 = 24 bytes */
    if (recv_all(s_recv_sock, cmd, sizeof(servo_cmd_t)) != ESP_OK) {

        ESP_LOGW(TAG, "Recv failed, reconnecting cmd socket...");
        close(s_recv_sock);
        s_recv_sock = -1;
        tcp_recv_connect();
        return ESP_FAIL;
    }

    /* Basic sanity: clamp angles to [0, 180] */
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (cmd->angle[i] < 0.0f)   cmd->angle[i] = 0.0f;
        if (cmd->angle[i] > 180.0f) cmd->angle[i] = 180.0f;
    }

    return ESP_OK;
}

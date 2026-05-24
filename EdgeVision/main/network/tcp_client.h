#pragma once

#include "esp_camera.h"
#include "esp_err.h"
#include <stdint.h>

/* ─── Protocol ───────────────────────────────────────────────────────────────
 * TX  (ESP → server, port TCP_PORT_IMAGE):
 *     [uint32_t length BE][length bytes JPEG data]
 *
 * RX  (server → ESP, port TCP_PORT_CMD):
 *     [servo_cmd_t] = 6 × float32 servo angles (0.0 – 180.0 degrees)
 * ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    float angle[6];   /* degrees, index 0-5 maps to SERVO_PIN_0…5 */
} servo_cmd_t;

/**
 * Connect to TCP_SERVER_IP:TCP_PORT_IMAGE for sending frames.
 * Retries indefinitely on failure (blocks until connected).
 */
void tcp_send_connect(void);

/**
 * Connect to TCP_SERVER_IP:TCP_PORT_CMD for receiving commands.
 * Retries indefinitely on failure (blocks until connected).
 */
void tcp_recv_connect(void);

/**
 * Send one JPEG frame. Returns ESP_FAIL if socket error (does NOT reconnect).
 * Frame buffer is NOT freed here — caller is responsible.
 * On ESP_FAIL: caller must 1) return fb to camera driver, 2) call tcp_send_reconnect().
 *
 * @param fb  Frame buffer captured by camera_drv_capture()
 * @return ESP_OK on success, ESP_FAIL on socket error
 */
esp_err_t tcp_send_frame(const camera_fb_t *fb);

/**
 * Reconnect the image socket (TCP_PORT_IMAGE). Blocks until connected.
 * Must be called AFTER returning the frame buffer to avoid buffer starvation.
 */
void tcp_send_reconnect(void);

/**
 * Receive one servo command (blocks until data arrives or error).
 * Handles reconnect on socket error.
 *
 * @param cmd  Output servo command
 * @return ESP_OK on success
 */
esp_err_t tcp_recv_cmd(servo_cmd_t *cmd);

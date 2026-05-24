#pragma once

#include "esp_camera.h"
#include "esp_err.h"

/**
 * Initialize OV5640 camera. Frame buffers are allocated in PSRAM.
 * Requires CONFIG_SPIRAM_BOOT_INIT=y — PSRAM must be ready before calling.
 *
 * @return ESP_OK on success
 */
esp_err_t camera_drv_init(void);

/**
 * Capture one frame. Caller MUST call camera_drv_fb_return() after use.
 * The frame buffer lives in PSRAM — do not copy unless necessary.
 *
 * @return Pointer to frame buffer, NULL on failure
 */
camera_fb_t *camera_drv_capture(void);

/**
 * Return frame buffer to the driver (re-enables DMA for that buffer slot).
 */
void camera_drv_fb_return(camera_fb_t *fb);

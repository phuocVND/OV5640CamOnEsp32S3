#include "camera_drv.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"

static const char *TAG = "camera_drv";

static const camera_config_t s_cam_cfg = {
    .pin_pwdn     = CAM_PIN_PWDN,
    .pin_reset    = CAM_PIN_RESET,
    .pin_xclk     = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d0       = CAM_PIN_D0,  .pin_d1 = CAM_PIN_D1,
    .pin_d2       = CAM_PIN_D2,  .pin_d3 = CAM_PIN_D3,
    .pin_d4       = CAM_PIN_D4,  .pin_d5 = CAM_PIN_D5,
    .pin_d6       = CAM_PIN_D6,  .pin_d7 = CAM_PIN_D7,
    .pin_vsync    = CAM_PIN_VSYNC,
    .pin_href     = CAM_PIN_HREF,
    .pin_pclk     = CAM_PIN_PCLK,

    .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = CAM_FRAME_SIZE,
    .jpeg_quality = CAM_JPEG_QUALITY,
    .fb_count     = CAM_FB_COUNT,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

esp_err_t camera_drv_init(void)
{
#ifdef CONFIG_SPIRAM
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not ready — check CONFIG_SPIRAM_BOOT_INIT=y");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PSRAM free before camera init: %luKB",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
#endif

    /* Deassert power-down (active-low) */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CAM_PIN_PWDN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(CAM_PIN_PWDN, 0);

    esp_err_t err = esp_camera_init(&s_cam_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Camera init OK (frame=%dx%d, q=%d, fb_count=%d)",
             /* resolution lookup: esp_camera_sensor_get()->status.framesize */
             0, 0, CAM_JPEG_QUALITY, CAM_FB_COUNT);
    return ESP_OK;
}

camera_fb_t *camera_drv_capture(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "esp_camera_fb_get returned NULL");
    }
    return fb;
}

void camera_drv_fb_return(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

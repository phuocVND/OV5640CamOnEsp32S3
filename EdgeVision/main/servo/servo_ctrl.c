#include "servo_ctrl.h"
#include "app_config.h"
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "servo_ctrl";

/* GPIO → LEDC channel mapping */
static const int s_servo_pins[SERVO_COUNT] = {
    SERVO_PIN_0, SERVO_PIN_1, SERVO_PIN_2,
    SERVO_PIN_3, SERVO_PIN_4, SERVO_PIN_5,
};

/* ─── Angle → LEDC duty conversion ──────────────────────────────────────────
 *
 * LEDC 50Hz, 14-bit resolution:
 *   period   = 20 ms = 20,000 µs
 *   steps    = 2^14  = 16,384
 *   µs/step  = 20,000 / 16,384 ≈ 1.22 µs
 *
 *   pulse_µs = SERVO_PULSE_MIN_US + angle/180 × (MAX−MIN)
 *   duty     = pulse_µs / 20,000 × 16,384
 * ─────────────────────────────────────────────────────────────────────────── */
static uint32_t angle_to_duty(float angle)
{
    if (angle < SERVO_ANGLE_MIN) angle = SERVO_ANGLE_MIN;
    if (angle > SERVO_ANGLE_MAX) angle = SERVO_ANGLE_MAX;

    float pulse_us = SERVO_PULSE_MIN_US
                   + (angle / SERVO_ANGLE_MAX)
                   * (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US);

    /* duty = pulse_us / period_us × 2^resolution */
    float period_us = 1000000.0f / SERVO_LEDC_FREQ_HZ;
    uint32_t max_duty = (1u << 14) - 1;
    return (uint32_t)(pulse_us / period_us * max_duty);
}

esp_err_t servo_ctrl_init(void)
{
    /* One shared timer for all 6 channels */
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_1,          /* timer 0 used by camera XCLK */
        .duty_resolution = SERVO_LEDC_RESOLUTION,
        .freq_hz         = SERVO_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        ledc_channel_config_t ch = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)(LEDC_CHANNEL_2 + i), /* ch2-7, ch0/1 for camera */
            .timer_sel  = LEDC_TIMER_1,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = s_servo_pins[i],
            .duty       = angle_to_duty(90.0f),   /* start at 90° neutral */
            .hpoint     = 0,
        };
        err = ledc_channel_config(&ch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ledc_channel_config ch%d failed: %s", i, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Servo init OK — %d servos at 90° neutral", SERVO_COUNT);
    return ESP_OK;
}

void servo_ctrl_set_angle(uint8_t index, float angle)
{
    if (index >= SERVO_COUNT) {
        ESP_LOGW(TAG, "servo index %d out of range", index);
        return;
    }
    uint32_t duty = angle_to_duty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(LEDC_CHANNEL_2 + index), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(LEDC_CHANNEL_2 + index));
}

void servo_ctrl_set_all(const float angles[6])
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        servo_ctrl_set_angle(i, angles[i]);
    }
}

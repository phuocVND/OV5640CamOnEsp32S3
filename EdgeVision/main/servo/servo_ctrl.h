#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Initialize LEDC peripheral for 6 servo channels.
 * All servos start at 90° (neutral position).
 *
 * @return ESP_OK on success
 */
esp_err_t servo_ctrl_init(void);

/**
 * Set angle for one servo.
 *
 * @param index  Servo index 0-5 (maps to SERVO_PIN_0…SERVO_PIN_5)
 * @param angle  Target angle in degrees [0.0, 180.0]
 */
void servo_ctrl_set_angle(uint8_t index, float angle);

/**
 * Set all 6 servo angles at once.
 *
 * @param angles  Array of 6 floats [degrees], index 0-5
 */
void servo_ctrl_set_all(const float angles[6]);

#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialize ST7789 display via SPI (pins from app_config.h).
 *        Must be called before any draw function.
 */
esp_err_t lcd_drv_init(void);

/**
 * @brief Draw a RGB565 bitmap to a rectangular region of the display.
 *
 * @param x     Left pixel column (0 = leftmost)
 * @param y     Top pixel row    (0 = topmost)
 * @param w     Width  in pixels
 * @param h     Height in pixels
 * @param buf   Pointer to w×h RGB565 pixels (big-endian, MSB first)
 */
esp_err_t lcd_drv_draw_rgb565(uint16_t x, uint16_t y,
                               uint16_t w, uint16_t h,
                               const uint16_t *buf);

/**
 * @brief Fill entire screen with a solid RGB565 color.
 *
 * @param color  RGB565 value (e.g. 0x0000 = black, 0xFFFF = white)
 */
void lcd_drv_fill(uint16_t color);

/**
 * @brief Draw a null-terminated ASCII string using the built-in 8×8 font
 *        rendered at 2× scale (each glyph occupies 16×16 pixels on screen).
 *
 * @param x    Left pixel column (0 = leftmost)
 * @param y    Top pixel row    (0 = topmost)
 * @param str  Null-terminated printable ASCII string (0x20–0x7E)
 * @param fg   Foreground RGB565 color (text pixels)
 * @param bg   Background RGB565 color (cell fill)
 */
void lcd_drv_puts(uint16_t x, uint16_t y,
                  const char *str,
                  uint16_t fg, uint16_t bg);

/**
 * @brief printf-formatted text at (x, y), 2× glyph scale, max 64 chars.
 *
 * @param x, y   Top-left pixel position
 * @param fg     Foreground color (RGB565)
 * @param bg     Background color (RGB565)
 * @param fmt    printf format string
 */
void lcd_drv_printf(uint16_t x, uint16_t y,
                    uint16_t fg, uint16_t bg,
                    const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

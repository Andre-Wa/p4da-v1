#pragma once

#include "esp_lcd_touch.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializa o barramento I2C compartilhado e o controlador de touch
 * GT911. Devolve o handle "cru" (esp_lcd_touch_handle_t) que o
 * slint_esp_init() espera.
 */
esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif

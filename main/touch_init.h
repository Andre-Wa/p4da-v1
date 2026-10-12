#pragma once

#include "esp_lcd_touch.h"
#include "driver/i2c_master.h"
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

/** M5c.1: o barramento I2C compartilhado (touch + ES8311). */
i2c_master_bus_handle_t board_i2c_bus(void);

#ifdef __cplusplus
}
#endif

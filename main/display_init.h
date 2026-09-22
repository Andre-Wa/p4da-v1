#pragma once
#include <stdbool.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializa o barramento MIPI-DSI + painel ST7701S.
 *
 * Devolve o handle "cru" do painel (esp_lcd_panel_handle_t), sem passar
 * por nenhum framework de UI — é isso que o slint_esp_init() espera
 * receber.
 */
esp_err_t board_display_init(esp_lcd_panel_handle_t *out_panel);

/** Liga o backlight em 100% (se BOARD_LCD_BL_GPIO estiver definido). */
void board_display_backlight_on(void);

/** Ajusta o brilho do backlight em porcentagem (0..100) via PWM/LEDC.
 *  0 = backlight totalmente off (usado pelo standby). Valores 1..9 são
 *  elevados para ~10% para a tela nunca ficar "acesa porém preta". */
void board_display_backlight_set(uint8_t percent);

/** Brilho atual (0..100), conforme último set/on. */
uint8_t board_display_backlight_get(void);

/** Blank/unblank do painel (DISPOFF/DISPON), p/ standby. */
void board_display_panel_blank(bool blank);

#ifdef __cplusplus
}
#endif

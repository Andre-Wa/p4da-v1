#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * M5c.1 — saída local: codec ES8311 (I2S + I2C compartilhado c/ GT911)
 * + amplificador NS4150 (PA_EN GPIO11). Rodada de prova: senoide de
 * teste pelo botão Config > Sistema; o player entra no sink em M5c.2.
 */

esp_err_t audio_i2s_init(void);
bool audio_i2s_ready(void);

/** Senoide 440 Hz, estéreo, ~1,2 s, volume conservador (prova de caminho). */
esp_err_t audio_i2s_beep(int ms);

/** Volume do codec 0–100 (mapa default do esp_codec_dev). */
void audio_i2s_set_vol(int pct);

#ifdef __cplusplus
}
#endif

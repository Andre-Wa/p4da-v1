#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gerenciamento de energia em degraus (ver docs/POWER.md):
 *
 *   ACTIVE   -> tudo ligado
 *   DIM      -> ocioso >= dim_after_s: backlight ~12%
 *   STANDBY  -> ocioso >= screen_off_after_s: backlight 0 + DISPOFF no
 *               painel + light sleep. Wake por QUALQUER GPIO (botão
 *               BOOT GPIO35 e/ou INT do touch GPIO21) — resume <100 ms.
 *   HIBERNATE-> deep sleep com sessão salva no SD. Somente manual
 *               (menu Config) ou ocioso >= deep_sleep_after_s (se >0).
 *               ⚠️ No ESP32-P4, deep sleep SÓ acorda por LP GPIO
 *               (GPIO0-15) ou timer — e nenhum botão/wake de usuário
 *               desta placa está em LP GPIO. Na prática, hibernar =
 *               "desligar": volta no próximo power-on/reset, com a
 *               sessão restaurada do cartão.
 *
 * Por que não deep sleep automático como degrau padrão: o botão BOOT
 * (GPIO35) e o INT do GT911 (GPIO21) estão fora do domínio LP
 * (LP_GPIO0..15), então o aparelho "sumiria" até tirar/recarregar a
 * bateria. Light sleep com tela apagada é o máximo economizável que
 * ainda acorda ao toque.
 */
typedef enum {
    PDA_PWR_ACTIVE = 0,
    PDA_PWR_DIM,
    PDA_PWR_STANDBY,
} pda_power_state_t;

/** entering=true: prestes a dormir (UI pode persistir algo leve);
 *  entering=false: acabou de acordar (restaurar brilho etc.). */
typedef void (*pda_power_standby_cb)(bool entering, void *ctx);
/** Chamado imediatamente antes do deep sleep (gravar sessão no SD). */
typedef void (*pda_power_hibernate_save_cb)(void *ctx);

esp_err_t power_mgmt_init(void);

/** Qualquer interação (tecla USB, callback de UI, toque válido). */
void power_mgmt_activity(void);

pda_power_state_t power_mgmt_state(void);

/** "Dormir agora" (menu Config): força o próximo degrau = STANDBY. */
void power_mgmt_request_standby(void);

/** Hiberna agora: salva sessão, desmonta SD, deep sleep. */
void power_mgmt_hibernate(void);

/** true se este boot veio de um deep sleep (sessão a restaurar). */
bool power_mgmt_woke_from_hibernate(void);

void power_mgmt_set_standby_cb(pda_power_standby_cb cb, void *ctx);
void power_mgmt_set_hibernate_save_cb(pda_power_hibernate_save_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

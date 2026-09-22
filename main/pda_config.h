#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configurações do sistema, serializadas como Lua em
 * <raiz>/config/system.lua (ver docs/LUA.md). O struct em C é a fonte
 * da verdade em runtime; o arquivo é lido no boot e reescrito em
 * pda_config_save() (raiz ativa + espelho na outra raiz montada).
 */
typedef struct {
    /* display */
    int brightness;             /* 0..100 (backlight PWM)            */
    /* power (degraus de standby, ver docs/POWER.md) */
    int dim_after_s;            /* ocioso -> dimeriza backlight      */
    int screen_off_after_s;     /* ocioso -> tela off + light sleep  */
    int deep_sleep_after_s;     /* ocioso -> deep sleep + restore SD */
    bool wake_on_touch;         /* [HW?] INT do GT911 como wake source */
    /* locale / rede */
    char timezone[48];          /* ex. "America/Sao_Paulo" (uso futuro) */
    char ntp_server[64];        /* uso futuro (fase Wi-Fi)            */
    /* ui */
    bool onscreen_keyboard_auto;/* teclado virtual só sem HID presente */
} pda_settings_t;

/** Carrega <raiz>/config/system.lua; se não existir, cria com defaults.
 * Deve rodar depois de board_storage_init(). */
esp_err_t pda_config_init(void);

/** Ponteiro para o struct atual (não modificar direto; use update). */
const pda_settings_t *pda_settings(void);

/** Substitui o struct inteiro e marca como sujo (precisa salvar). */
void pda_settings_update(const pda_settings_t *s);

/** Campo único, por nome canônico Lua ("display.brightness",
 *  "power.dim_after_s", "power.screen_off_after_s",
 *  "power.deep_sleep_after_s", "power.wake_on_touch",
 *  "locale.timezone", "locale.ntp_server", "ui.onscreen_keyboard_auto").
 *  Usado pela API Lua pda.settings.* e pela tela de Config. */
bool pda_settings_get(const char *key, double *out_num, bool *out_bool, char *out_str, size_t str_sz);
bool pda_settings_set(const char *key, double num, bool is_bool, bool bool_val, const char *str);

bool pda_config_dirty(void);

/** Reescreve system.lua na raiz ativa e (melhor esforço) na outra raiz. */
esp_err_t pda_config_save(void);

/** Restaura defaults em memória (não salva). */
void pda_config_reset_defaults(void);

#ifdef __cplusplus
}
#endif

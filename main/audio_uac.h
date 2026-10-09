#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * M5a — saída de áudio USB (UAC host, speaker/DAC/fone USB-C).
 *
 * Segundo client da USB Host Library (o primeiro é o HID do teclado):
 * a biblioteca aceita clients simultâneos, cada um com sua task de
 * eventos — a do UAC é a background task do próprio driver
 * (create_background_task=true).
 *
 * Round 1 (M5a.1): PCM 16 bits, 1–2 canais, qualquer sample rate que o
 * dispositivo anuncie num alt setting compatível com o WAV (44k1/48k
 * estéreo cobrem o acervo típico). Sem hub, teclado e DAC não cabem
 * juntos no mesmo porto — hub support é o round 2.
 */

/** Instala o driver (retry interno até a Host Library subir). Idempotente. */
esp_err_t audio_uac_init(void);

/** Há um speaker/DAC UAC conectado agora? */
bool audio_uac_present(void);

/** Nome do produto (UTF-8 best-effort) ou "" se ausente. */
const char *audio_uac_dev_name(void);

/** Pede reprodução de um WAV (PCM16). Fila de 1 comando; trocadireto. */
esp_err_t audio_uac_play(const char *path);

/** M5a.2.3: play SEM retomar ponto de unplug (tap explícito na lista). */
esp_err_t audio_uac_play_fresh(const char *path);

/** Para a reprodução atual (device fica fechado até o próximo play). */
void audio_uac_stop(void);

bool audio_uac_playing(void);

/** M5a.2: pause sem perder a posição (suspend/resume do stream). */
bool audio_uac_paused(void);
void audio_uac_pause(void);
void audio_uac_resume(void);

/** v5.8: volume/mute via control transfer em task descartável
 *  (devices sem feature unit timeoutam ~5 s sem congelar UI/stream). */
void audio_uac_volume_step(int delta);
void audio_uac_mute_toggle(void);

/** Basename da faixa atual ("" se nenhuma). */
const char *audio_uac_track(void);

/** Estado humano p/ UI: "sem dispositivo" | "pronto" | "tocando" |
 *  "parado" | "erro: <motivo>". */
const char *audio_uac_state(void);

/** Chama (de qualquer contexto) quando algo muda p/ a UI refrescar. */
typedef void (*audio_uac_event_cb)(void *ctx);
void audio_uac_set_event_cb(audio_uac_event_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

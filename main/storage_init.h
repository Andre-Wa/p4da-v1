#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Onde a árvore /pda está vivendo neste boot. */
typedef enum {
    PDA_STORE_SD = 0,       /* /sdcard/pda  — armazenamento principal */
    PDA_STORE_INTERNAL = 1, /* /internal/pda — fallback sem cartão    */
} pda_store_t;

/**
 * Monta LittleFS interno em /internal e o MicroSD (se presente) em
 * /sdcard via SDMMC 4-bit @ 40 MHz (com fallback automático para
 * 20 MHz e 400 kHz em cartões problemáticos).
 *
 * Define a "raiz ativa" do sistema (pda_root()): /sdcard/pda quando há
 * cartão, /internal/pda caso contrário, e garante a árvore de diretórios
 * (config/, scripts/, notes/, music/, logs/, .state/) nessa raiz.
 *
 * Se o cartão aparecer pela primeira vez com o interno já populado, as
 * configurações existentes são promovidas (copiadas) para o cartão.
 * Nunca falha o boot por ausência de cartão.
 */
esp_err_t board_storage_init(void);

/** Desmonta o SD e corta o rail TF_VCC (usado antes de hibernar).
 *  Após chamar, storage_sd_mounted() passa a retornar false até o
 *  próximo boot (remontar em runtime fica p/ uma fase futura). */
void storage_shutdown_sd(void);

/* --- estado ------------------------------------------------------- */
bool storage_sd_mounted(void);
pda_store_t storage_active_store(void);
/** Raiz ativa, ex. "/sdcard/pda" (sem barra no fim). */
const char *pda_root(void);
/** Descreve o cartão p/ status bar, ex. "SD 14.8 GB" ou "sem cartao". */
void storage_sd_describe(char *out, size_t out_sz);

/** Monta "<pda_root()>/<rel>" em out. rel pode ser "" p/ a própria raiz. */
esp_err_t pda_path(char *out, size_t out_sz, const char *rel);

/* --- helpers de arquivo (qualquer mount: /internal, /sdcard) ------- */
esp_err_t storage_read_text_file(const char *path, char *out_buf, size_t buf_size, size_t *out_len);
/** Lê o arquivo inteiro num buffer alocado (malloc; caller libera). */
esp_err_t storage_read_file_alloc(const char *path, char **out_data, size_t *out_len);
esp_err_t storage_write_text_file(const char *path, const char *data, size_t len);
esp_err_t storage_delete_file(const char *path);
esp_err_t storage_ensure_dir(const char *path);
bool storage_file_exists(const char *path);
bool storage_is_dir(const char *path);
int64_t storage_file_size(const char *path);
esp_err_t storage_copy_file(const char *src, const char *dst);

#ifdef __cplusplus
}
#endif

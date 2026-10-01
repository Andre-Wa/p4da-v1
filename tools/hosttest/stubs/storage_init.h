/* stub: só o subconjunto que pda_config.c usa */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
bool storage_sd_mounted(void);
esp_err_t pda_path(char *out, size_t out_sz, const char *rel);
esp_err_t storage_read_file_alloc(const char *path, char **out_data, size_t *out_len);
esp_err_t storage_write_text_file(const char *path, const char *data, size_t len);
esp_err_t storage_delete_file(const char *path);
bool storage_file_exists(const char *path);
esp_err_t storage_copy_file(const char *src, const char *dst);
/* gancho do teste: 2ª leitura devolve conteúdo corrompido (simula mídia instável) */
extern int hosttest_flaky_reads;

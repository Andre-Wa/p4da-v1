/* stub p/ compilar pda_config.c no host (tools/hosttest) */
#pragma once
#include <stdint.h>
#include <stddef.h>
/* glibc < 2.38 não declara strlcpy; o shim vive em host_cfg_test.c */
size_t strlcpy(char *dst, const char *src, size_t sz);
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_ERR_STUB"; }

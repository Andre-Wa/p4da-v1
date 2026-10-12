/* stub de esp_adc/adc_oneshot.h p/ host tests. As funções falham de
 * propósito: os testes exercitam battery_ingest() direto, sem ADC. */
#pragma once
#include "esp_err.h"
#include "hal/adc_types.h"

typedef struct adc_oneshot_unit_t *adc_oneshot_unit_handle_t;

typedef enum { ADC_ULP_MODE_DISABLE = 0, ADC_ULP_MODE_ENABLE } adc_ulp_mode_t;

typedef struct {
    adc_unit_t unit_id;
    adc_ulp_mode_t ulp_mode;
} adc_oneshot_unit_init_cfg_t;

typedef struct {
    adc_atten_t atten;
    int bitwidth;
} adc_oneshot_chan_cfg_t;

static inline esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c,
                                             adc_oneshot_unit_handle_t *out)
{ (void)c; if (out) *out = NULL; return ESP_FAIL; }

static inline esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h,
                                                   adc_channel_t ch,
                                                   const adc_oneshot_chan_cfg_t *c)
{ (void)h; (void)ch; (void)c; return ESP_FAIL; }

static inline esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h,
                                         adc_channel_t ch, int *out)
{ (void)h; (void)ch; if (out) *out = 0; return ESP_FAIL; }

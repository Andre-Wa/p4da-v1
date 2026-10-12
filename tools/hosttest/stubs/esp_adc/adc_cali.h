/* stub de esp_adc/adc_cali.h p/ host tests: sem eFuse no host, todas as
 * criações de esquema falham -> battery.c cai no fallback linear. */
#pragma once
#include "esp_err.h"
#include "hal/adc_types.h"

typedef struct adc_cali_handle_t *adc_cali_handle_t;

typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    int bitwidth;
} adc_cali_curve_fitting_config_t;

typedef struct {
    adc_unit_t unit_id;
    adc_atten_t atten;
    int bitwidth;
} adc_cali_line_fitting_config_t;

static inline esp_err_t adc_cali_create_scheme_curve_fitting(
    const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *out)
{ (void)c; if (out) *out = NULL; return ESP_ERR_NOT_FOUND; }

static inline esp_err_t adc_cali_create_scheme_line_fitting(
    const adc_cali_line_fitting_config_t *c, adc_cali_handle_t *out)
{ (void)c; if (out) *out = NULL; return ESP_ERR_NOT_FOUND; }

static inline esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h,
                                                int raw, int *mv)
{ (void)h; (void)raw; if (mv) *mv = 0; return ESP_FAIL; }

static inline esp_err_t adc_cali_delete_handle(adc_cali_handle_t h)
{ (void)h; return ESP_OK; }

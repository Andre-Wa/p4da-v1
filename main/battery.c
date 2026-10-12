/**
 * battery.c — BAT.0 (v5.12.0): leitura de bateria via divisor da folha 03.
 *
 * Topologia (esquemático GUITION JC4880P443C_I_W, folha 03 "Expand IO"):
 *   BAT+ --R52 68K--+--R57 100K--GND   (C61 10nF no nó)
 *                   +--R55 0R-- GPIO53  (ADC2_CH4, atten 12 dB)
 *
 * Implementação:
 *  - adc_oneshot (ADC2) com calibração eFuse line-fitting QUANDO o chip
 *    tiver (SOC_ADC_CALIBRATION_V1_SUPPORTED); sem eFuse calibrado, cai no
 *    fallback linear documentado (FS ~3.1 V @ 12 dB, 12 bits) — o padrão
 *    do exemplo oficial examples/peripherals/adc/oneshot_read do IDF.
 *  - 8 amostras por poll (2 descartadas p/ settling), média, ×(68+100)/100
 *    = tensão da bateria, EMA 1/4 entre polls p/ não piscar na UI.
 *  - Task própria ("battery", 4 s): NUNCA tocar I2C/ADC no caminho da UI.
 *    A UI só lê os caches volatile.
 *
 * Por que task e não leitura sob demanda: a 1ª conversão oneshot após
 * acordar o ADC leva ~ms e o Slint roda em task de evento; manter o poll
 * aqui deixa a status bar sempre com valor fresco sem bloquear ninguém.
 */

#include "battery.h"
#include "board_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "battery";

/* statics no topo (regra de REGRESSOES_BUILD.md: uso-antes-definição e
 * escopo já custaram bugs aqui). */
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static volatile bool s_calibrated = false;
static volatile bool s_ready = false;
static volatile int s_pct = -1;
static volatile int s_batt_mv = -1;
static volatile int s_node_mv = -1;
static volatile int s_raw = -1;
static int s_ema_mv = -1;          /* só a task battery escreve */
static int s_polls = 0;            /* só a task battery escreve */
static int s_bad = 0;              /* leituras inválidas seguidas */
static TaskHandle_t s_task = NULL;

#define BATT_SAMPLES     8
#define BATT_POLL_MS     4000
#define BATT_LOG_EVERY   15        /* ~60 s de linha I, como os hwm */
#define BATT_FS_MV       3100      /* fallback linear: FS @ 12 dB */
#define BATT_FS_RAW      4095      /* 12 bits (ADC_BITWIDTH_DEFAULT no P4) */
#define BATT_MV_MIN      2000      /* abaixo disso: divisor/leitura ruim */
#define BATT_MV_MAX      4500      /* acima: impossível p/ 1 célula LiPo */

/* Curva LiPo típica SEM carga (mV -> %), piecewise-linear, descendente.
 * Aproximação de 1ª rodada; calibrar contra multímetro em BAT.1
 * (docs/POWER.md). Durante carga a tensão lê alto de propósito. */
static const struct { int mv; int pct; } s_curve[] = {
    { 4200, 100 }, { 4100, 92 }, { 4000, 82 }, { 3900, 68 }, { 3800, 53 },
    { 3700, 38 },  { 3600, 24 }, { 3500, 12 }, { 3400, 5 },  { 3300, 0 },
};
#define CURVE_N ((int)(sizeof(s_curve) / sizeof(s_curve[0])))

static int curve_pct(int mv)
{
    if (mv >= s_curve[0].mv) return 100;
    if (mv <= s_curve[CURVE_N - 1].mv) return 0;
    for (int i = 0; i + 1 < CURVE_N; i++) {
        if (mv <= s_curve[i].mv && mv >= s_curve[i + 1].mv) {
            const int span = s_curve[i].mv - s_curve[i + 1].mv;
            const int dp = s_curve[i].pct - s_curve[i + 1].pct;
            return s_curve[i + 1].pct + (mv - s_curve[i + 1].mv) * dp / span;
        }
    }
    return 0;
}

/* ---- calibração: MESMO padrão do exemplo oficial do IDF 5.5.1 ----
 * (examples/peripherals/adc/oneshot_read): tenta curve fitting, depois
 * line fitting; sem eFuse calibrado devolve false e o caller usa o
 * fallback linear. No P4 o gate de linha é SOC_ADC_CALIBRATION_V1. */
static bool cali_init(adc_unit_t unit, adc_channel_t chan, adc_atten_t atten,
                      adc_cali_handle_t *out)
{
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;
    esp_err_t ret = ESP_FAIL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cc = {
            .unit_id = unit,
            .chan = chan,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cc, &handle);
        if (ret == ESP_OK) calibrated = true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cc = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cc, &handle);
        if (ret == ESP_OK) calibrated = true;
    }
#endif
    *out = handle;
    if (!calibrated) {
        ESP_LOGW(TAG, "sem calibração eFuse p/ ADC (fallback linear %d mV FS)",
                 BATT_FS_MV);
    }
    return calibrated;
}

/* Média de amostras oneshot -> mV no NÓ do divisor (-1 se tudo falhar). */
static int read_node_mv(void)
{
    int sum = 0, n = 0, raw_avg = -1;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int r = 0;
        if (adc_oneshot_read(s_adc, BOARD_BATT_ADC_CHANNEL, &r) != ESP_OK) continue;
        if (i < 2) continue;   /* settling do SAR/capacitor C61 */
        sum += r;
        n++;
    }
    if (n == 0) return -1;
    raw_avg = sum / n;
    s_raw = raw_avg;
    if (s_calibrated && s_cali) {
        int mv = -1;
        if (adc_cali_raw_to_voltage(s_cali, raw_avg, &mv) == ESP_OK && mv > 0) {
            return mv;
        }
    }
    return raw_avg * BATT_FS_MV / BATT_FS_RAW;
}

static void battery_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int nmv = read_node_mv();
        if (nmv > 0) {
            const int bmv = nmv * (BOARD_BATT_DIV_R_TOP_K + BOARD_BATT_DIV_R_BOT_K)
                                / BOARD_BATT_DIV_R_BOT_K;
            if (bmv >= BATT_MV_MIN && bmv <= BATT_MV_MAX) {
                s_bad = 0;
                s_ema_mv = (s_ema_mv < 0) ? bmv : (s_ema_mv * 3 + bmv) / 4;
                s_node_mv = nmv;
                s_batt_mv = s_ema_mv;
                s_pct = curve_pct(s_ema_mv);
                if (!s_ready) {
                    s_ready = true;
                    ESP_LOGI(TAG, "bateria: %d%% | %d mV | nó %d mV | raw %d | %s",
                             s_pct, s_ema_mv, nmv, s_raw,
                             s_calibrated ? "calibrado (eFuse)" : "fallback linear");
                } else if (++s_polls % BATT_LOG_EVERY == 0) {
                    ESP_LOGI(TAG, "bateria: %d%% | %d mV | nó %d mV | raw %d",
                             s_pct, s_ema_mv, nmv, s_raw);
                }
            } else if (++s_bad == 3) {
                ESP_LOGW(TAG, "leituras fora da faixa (%d mV no nó) — divisor/ADC?",
                         bmv);
                s_bad = 0;
            }
        } else if (!s_ready && ++s_bad < 4) {
            ESP_LOGW(TAG, "adc_oneshot_read falhou (tentativa %d)", s_bad);
        }
        vTaskDelay(pdMS_TO_TICKS(BATT_POLL_MS));
    }
}

esp_err_t battery_start(void)
{
    if (s_task) return ESP_OK;

    const adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = BOARD_BATT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit(ADC%d) falhou",
                 (int)BOARD_BATT_ADC_UNIT);
        return ESP_FAIL;
    }
    const adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, BOARD_BATT_ADC_CHANNEL, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "config do canal ADC falhou");
        return ESP_FAIL;
    }
    s_calibrated = cali_init(BOARD_BATT_ADC_UNIT, BOARD_BATT_ADC_CHANNEL,
                             ADC_ATTEN_DB_12, &s_cali);

    if (xTaskCreate(battery_task, "battery", 4096, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "sem RAM p/ task battery");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sentido de bateria: GPIO%d = ADC%d_CH%d, divisor %dK/%dK",
             (int)BOARD_BATT_SENSE_GPIO, (int)BOARD_BATT_ADC_UNIT,
             (int)BOARD_BATT_ADC_CHANNEL, BOARD_BATT_DIV_R_TOP_K,
             BOARD_BATT_DIV_R_BOT_K);
    return ESP_OK;
}

bool battery_ready(void) { return s_ready; }
int battery_percent(void) { return s_pct; }
int battery_mv(void) { return s_batt_mv; }
int battery_node_mv(void) { return s_node_mv; }
bool battery_calibrated(void) { return s_calibrated; }

void battery_status_str(char *buf, int sz)
{
    if (!buf || sz <= 0) return;
    if (!s_ready) snprintf(buf, sz, "--");
    else snprintf(buf, sz, "%d%%", s_pct);
}

void battery_describe(char *buf, int sz)
{
    if (!buf || sz <= 0) return;
    if (!s_ready) {
        snprintf(buf, sz, "lendo (ADC%d_CH%d/GPIO%d)...",
                 (int)BOARD_BATT_ADC_UNIT, (int)BOARD_BATT_ADC_CHANNEL,
                 (int)BOARD_BATT_SENSE_GPIO);
        return;
    }
    snprintf(buf, sz, "%d%% (%d mV, ADC%d_CH%d/GPIO%d, %s)",
             s_pct, s_batt_mv, (int)BOARD_BATT_ADC_UNIT,
             (int)BOARD_BATT_ADC_CHANNEL, (int)BOARD_BATT_SENSE_GPIO,
             s_calibrated ? "cal. eFuse" : "fallback linear");
}

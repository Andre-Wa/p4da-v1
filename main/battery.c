/**
 * battery.c — BAT.0 (v5.12.0) + BAT.2 (v5.13.0): bateria via divisor da
 * folha 03, com máquina de estados de carga/assentamento.
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
 *    = tensão da bateria, EMA entre polls p/ não piscar na UI.
 *  - Task própria ("battery", 4 s): NUNCA tocar I2C/ADC no caminho da UI.
 *    A UI só lê os caches volatile.
 *  - BAT.2 (v5.13.0): máquina de estados BATERIA/CARREGANDO/ASSENTANDO.
 *    Sem pino de status do IP5306 roteado ao P4, a carga é detectada por
 *    SALTO de tensão (+40 mV num poll de 4 s é fisicamente impossível só
 *    com química). Ao desconectar o carregador o % exibido CONGELA
 *    ("assentando") até a tensão de carga dissipar (sag >= 90 mV do pico,
 *    ou 10 min), então o % reassume pela curva — é o que mata o efeito
 *    sanfona 97→94→97 relatado ao tirar/recolocar o carregador. Durante a
 *    carga o % segue a tensão de terminal (lê alto: é a física do CC-CV,
 *    ver docs/POWER.md §BAT.2).
 *
 * Por que task e não leitura sob demanda: a 1ª conversão oneshot após
 * acordar o ADC leva ~ms e o Slint roda em task de evento; manter o poll
 * aqui deixa a status bar sempre com valor fresco sem bloquear ninguém.
 *
 * Nota de exibição: adc_unit_t é enum 0-based (ADC_UNIT_2 == 1); todo log
 * imprime unit+1. Na v5.12.0 o log dizia "ADC1_CH4" por imprimir o enum
 * cru — o canal certo sempre foi ADC2_CH4 (GPIO53).
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
static int s_ema_mv = -1;          /* só battery_ingest() escreve */
static int s_prev_mv = -1;         /* só battery_ingest() escreve */
static int s_polls = 0;            /* só a task battery escreve */
static int s_bad = 0;              /* leituras inválidas seguidas */
static int s_state = 0;            /* BSTAT_*; só battery_ingest() escreve */
static int s_pre_chg_mv = -1;      /* ema ao entrar em carga (p/ sag) */
static int s_settle_peak = -1;     /* pico de ema na sessão de carga */
static int s_settle_polls = 0;     /* polls em ASSENTANDO */
static TaskHandle_t s_task = NULL;

#define BATT_SAMPLES     8
#define BATT_POLL_MS     4000
#define BATT_LOG_EVERY   15        /* ~60 s de linha I, como os hwm */
#define BATT_FS_MV       3100      /* fallback linear: FS @ 12 dB */
#define BATT_FS_RAW      4095      /* 12 bits (ADC_BITWIDTH_DEFAULT no P4) */
#define BATT_MV_MIN      2000      /* abaixo disso: divisor/leitura ruim */
#define BATT_MV_MAX      4500      /* acima: impossível p/ 1 célula LiPo */

/* BAT.2: limiares da máquina de estados (por poll de 4 s). Uma LiPo em
 * repouso/carga muda < ~5 mV/poll; +40 mV/poll só com carregador CC-CV. */
#define BATT_RISE_MV       40      /* salto p/ detectar "carregador ligou" */
#define BATT_DROP_MV       25      /* queda p/ detectar "carregador saiu" */
#define BATT_SETTLE_POLLS  150     /* 10 min máx. segurando o % */
#define BATT_SETTLE_SAG_MV 90      /* sag do pico que encerra o assentamento */

/* Estados (s_state). Exposos p/ UI/log como texto via battery_state_str(). */
#define BSTAT_BATTERY   0   /* na bateria: % segue a curva */
#define BSTAT_CHARGING  1   /* salto detectado: % segue a tensão de carga */
#define BSTAT_SETTLING  2   /* carregador saiu: % congelado até assentar */

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

/* ---- BAT.2: máquina de estados carga/assentamento ----
 * Entra com a tensão JA compensada pelo divisor (mV de bateria).
 * Única dona de s_ema_mv/s_pct/s_state. Em host test (BATTERY_UNIT_TEST)
 * é não-static p/ o harness injetar perfis de tensão direto. */
#ifdef BATTERY_UNIT_TEST
#define BATT_VIS /* visível p/ tools/hosttest/host_batt_test.c */
#else
#define BATT_VIS static
#endif

BATT_VIS void battery_ingest(int bmv)
{
    const int delta = (s_prev_mv < 0) ? 0 : bmv - s_prev_mv;
    s_prev_mv = bmv;

    if (s_ema_mv < 0) {           /* 1ª leitura válida: ancora tudo */
        s_ema_mv = bmv;
        s_pct = curve_pct(bmv);
        s_state = BSTAT_BATTERY;
        return;
    }

    switch (s_state) {
    case BSTAT_CHARGING:
        if (delta <= -BATT_DROP_MV) {
            s_state = BSTAT_SETTLING;
            s_settle_polls = 0;
            s_settle_peak = s_ema_mv;
            ESP_LOGI(TAG, "carregador saiu: segurando %d%% enquanto a "
                     "tensão de carga assenta (%d mV)", s_pct, s_ema_mv);
            s_ema_mv = (s_ema_mv * 7 + bmv) / 8;   /* lenta p/ não vazar p/ % */
            break;
        }
        s_ema_mv = (s_ema_mv + bmv) / 2;           /* segue a carga rápido */
        if (s_ema_mv > s_settle_peak) s_settle_peak = s_ema_mv;
        s_pct = curve_pct(s_ema_mv);
        break;

    case BSTAT_SETTLING:
        /* Reconexão só se a tensão VOLTA ao nível de carga (pico - 30 mV):
         * um rebound pós-unplug (+40..60 mV de dissipação parcial) não é
         * carregador de volta — era o que confundia a v5.13.0-rc. */
        if (delta >= BATT_RISE_MV && bmv >= s_settle_peak - 30) {
            s_state = BSTAT_CHARGING;
            ESP_LOGI(TAG, "carregador de volta: pct segue a tensão de carga");
            s_ema_mv = (s_ema_mv + bmv) / 2;
            s_pct = curve_pct(s_ema_mv);
            break;
        }
        s_ema_mv = (s_ema_mv * 7 + bmv) / 8;       /* lenta: só diagnóstico */
        s_settle_polls++;
        if (s_settle_polls >= BATT_SETTLE_POLLS ||
            s_ema_mv <= s_settle_peak - BATT_SETTLE_SAG_MV ||
            (s_pre_chg_mv > 0 && s_ema_mv <= s_pre_chg_mv + 15)) {
            s_state = BSTAT_BATTERY;
            s_pct = curve_pct(s_ema_mv);           /* snap honesto p/ curva */
            ESP_LOGI(TAG, "tensão assentada: %d%% (%d mV) após %d polls",
                     s_pct, s_ema_mv, s_settle_polls);
        }
        break;

    default: /* BSTAT_BATTERY */
        if (delta >= BATT_RISE_MV) {
            s_state = BSTAT_CHARGING;
            s_pre_chg_mv = s_ema_mv;
            s_settle_peak = bmv;
            ESP_LOGI(TAG, "salto de +%d mV num poll: carregador presente "
                     "(pct segue a tensão de carga)", delta);
        }
        s_ema_mv = (s_ema_mv * 3 + bmv) / 4;
        s_pct = curve_pct(s_ema_mv);
        break;
    }
}

const char *battery_state_str(void)
{
    switch (s_state) {
    case BSTAT_CHARGING: return "carregando";
    case BSTAT_SETTLING: return "assentando";
    default:             return "bateria";
    }
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
                const bool first = !s_ready;
                battery_ingest(bmv);
                s_node_mv = nmv;
                s_batt_mv = s_ema_mv;
                if (first) {
                    s_ready = true;
                    ESP_LOGI(TAG, "bateria: %d%% | %d mV | nó %d mV | raw %d | %s",
                             s_pct, s_ema_mv, nmv, s_raw,
                             s_calibrated ? "calibrado (eFuse)" : "fallback linear");
                } else if (++s_polls % BATT_LOG_EVERY == 0) {
                    ESP_LOGI(TAG, "bateria: %d%% | %d mV | nó %d mV | raw %d | %s",
                             s_pct, s_ema_mv, nmv, s_raw, battery_state_str());
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
             (int)BOARD_BATT_SENSE_GPIO, (int)BOARD_BATT_ADC_UNIT + 1,
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
                 (int)BOARD_BATT_ADC_UNIT + 1, (int)BOARD_BATT_ADC_CHANNEL,
                 (int)BOARD_BATT_SENSE_GPIO);
        return;
    }
    /* adc_unit_t é 0-based: imprime unit+1 (v5.12.0 dizia "ADC1" por bug
     * de exibição; o canal certo sempre foi ADC2_CH4/GPIO53). */
    snprintf(buf, sz, "%d%% (%d mV, ADC%d_CH%d/GPIO%d, %s%s%s)",
             s_pct, s_batt_mv, (int)BOARD_BATT_ADC_UNIT + 1,
             (int)BOARD_BATT_ADC_CHANNEL, (int)BOARD_BATT_SENSE_GPIO,
             s_calibrated ? "cal. eFuse" : "fallback linear",
             s_state == BSTAT_BATTERY ? "" : ", ",
             s_state == BSTAT_BATTERY ? "" : battery_state_str());
}

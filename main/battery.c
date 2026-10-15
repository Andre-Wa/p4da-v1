/**
 * battery.c — BAT.0 (v5.12.0) + BAT.2 (v5.13.0) + BAT.1a (v5.14.0):
 * bateria via divisor da folha 03, máquina de estados de carga/assentamento
 * e âncoras mín/máx aprendidas em campo (remapeamento da curva).
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
 *  - BAT.1a (v5.14.0): ÂNCORAS aprendidas em campo. A curva s_curve é uma
 *    LiPo genérica 3300-4200 mV; a célula real pode cortar mais alto
 *    (relaxo pós-carga medido em campo: 4156 mV em carga -> 3.9 V no
 *    multímetro, célula 14500 com resistência interna alta). O módulo
 *    aprende sozinho: vmin = menor tensão vista PERTO do vazio (estado
 *    bateria, pct <= 30), vmax = maior tensão vista em CARGA; persiste no
 *    NVS (ns "batt") e remapeia a curva padrão p/ o domínio [vmin,vmax]
 *    (std_mv). Botão "Zerar calibração da bateria" no Config→Sistema
 *    restaura os padrões. Procedimento de campo sem serial: usar até
 *    desligar + carregar até o fim; as âncoras aparecem na tela e no log.
 *    Detalhes: docs/POWER.md §BAT.1.
 *  - BAT.1c (v5.15.0): ESTABILIDADE SOB CARGA. Medição de campo (14ª
 *    rodada): brilho/Wi-Fi/falante movem a tensão de terminal ±50 mV ou
 *    mais (célula 14500 com resistência interna alta + boost do IP5306);
 *    o % dançava vários pontos durante o uso e um sag profundo chegou a
 *    2815 mV com tudo ligado. Três respostas:
 *      (1) EMA LENTA (α=1/16, τ≈64 s) é a fonte do % na bateria — a EMA
 *          rápida (α=1/4) continua dona da lógica de carga/assentamento;
 *      (2) PASSO no % exibido: cai no máx. 1 ponto a cada 3 polls e só
 *          sobe se a curva superar o exibido em >2 pontos (banda morta),
 *          1 a cada 6 polls — a física do sag continua visível no log,
 *          mas o número da UI deriva devagar e dentro de uma banda;
 *      (3) o salto "carregador entrou" exige CONFIRMAÇÃO no 2º poll
 *          (nível sustentado E acima da EMA lenta +40 mV): alívio de
 *          carga (dim do backlight, fim de beep) recupera 50-150 mV num
 *          poll e antes fantasiava de carregador ("+" fantasma).
 *    O "Zerar calibração" agora religa o % na hora (flag consumida no
 *    poll seguinte) em vez de esperar o passo descer até a nova curva.
 *    Detalhes e números de campo: docs/POWER.md §BAT.1c.
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
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "battery";

/* Em host test (BATTERY_UNIT_TEST) as funções de lógica pura viram não-
 * static p/ tools/hosttest/host_batt_test.c injetar perfis de tensão. */
#ifdef BATTERY_UNIT_TEST
#define BATT_VIS /* visível p/ tools/hosttest/host_batt_test.c */
#else
#define BATT_VIS static
#endif

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
static int s_slow_mv = -1;         /* BAT.1c: EMA lenta — fonte do % na bateria */
static int s_pct_step = 0;         /* BAT.1c: contador de polls p/ passo do % */
static int s_rise_pend = -1;       /* BAT.1c: candidato a carregador (mV), -1 = nenhum */
static int s_rise_slow = -1;       /* s_slow_mv no poll do candidato */
static int s_rise_ema = -1;        /* s_ema_mv no poll do candidato (vira pre_chg) */
static volatile bool s_recal = false;  /* BAT.1a/1c: botão zerar → religar % */
static int s_polls = 0;            /* só a task battery escreve */
static int s_bad = 0;              /* leituras inválidas seguidas */
static int s_state = 0;            /* BSTAT_*; só battery_ingest() escreve */
static int s_pre_chg_mv = -1;      /* ema ao entrar em carga (p/ sag) */
static int s_settle_peak = -1;     /* pico de ema na sessão de carga */
static int s_settle_polls = 0;     /* polls em ASSENTANDO */
static TaskHandle_t s_task = NULL;
/* BAT.1a: âncoras do remapeamento da curva (aprendidas em campo, NVS). */
static int s_vmin = 3300;          /* = extremo 0% da s_curve (padrão) */
static int s_vmax = 4200;          /* = extremo 100% da s_curve (padrão) */

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

/* BAT.1c (v5.15.0): estabilidade do % sob carga. Campo (14ª rodada):
 * brilho/Wi-Fi/falante movem a tensão ±50 mV; sag extremo 2815 mV. */
#define BATT_SLOW_SHIFT      4     /* EMA lenta: (15·velho+novo)/16, τ≈64 s @4 s */
#define BATT_PCT_DROP_POLLS  3     /* % cai no máx. 1 ponto a cada 12 s */
#define BATT_PCT_RISE_POLLS  6     /* % sobe no máx. 1 ponto a cada 24 s… */
#define BATT_PCT_RISE_HYST   2     /* …e só se a curva passar do exibido +2 */
#define BATT_RISE_TOL        15    /* candidato a carregador: tolerância (mV) */

/* BAT.1a: âncoras aprendidas (domínio da s_curve = 3300-4200 mV). */
#define BATT_STD_MV_MIN    3300    /* extremo 0% da curva padrão */
#define BATT_STD_MV_MAX    4200    /* extremo 100% da curva padrão */
#define BATT_ANCH_ABS_MIN  2800    /* vmin abaixo disso = leitura inválida */
#define BATT_ANCH_ABS_MAX  4400    /* vmax acima disso = leitura inválida */
#define BATT_ANCH_SPAN     600     /* vão mín. entre âncoras (sanidade) */
#define BATT_LEARN_HYST    15      /* só move âncora além desta margem */
#define BATT_LEARN_PCT     30      /* vmin só aprende com pct <= 30% */
#define BATT_NVS_NS        "batt"  /* chaves i32: "vmin", "vmax" */

/* Estados (s_state). Exposos p/ UI/log como texto via battery_state_str(). */
#define BSTAT_BATTERY   0   /* na bateria: % segue a curva */
#define BSTAT_CHARGING  1   /* salto detectado: % segue a tensão de carga */
#define BSTAT_SETTLING  2   /* carregador saiu: % congelado até assentar */

/* Curva LiPo típica SEM carga (mV -> %), piecewise-linear, descendente.
 * Domínio padrão 3300-4200 mV; BAT.1a remapeia a entrada p/ este domínio
 * usando as âncoras aprendidas (std_mv) — a FORMA da curva segue a LiPo
 * genérica, os EXTREMOS se ajustam à célula real. Refino de forma contra
 * multímetro continua em BAT.1b (docs/POWER.md). Durante carga a tensão
 * lê alto de propósito. */
static const struct { int mv; int pct; } s_curve[] = {
    { 4200, 100 }, { 4100, 92 }, { 4000, 82 }, { 3900, 68 }, { 3800, 53 },
    { 3700, 38 },  { 3600, 24 }, { 3500, 12 }, { 3400, 5 },  { 3300, 0 },
};
#define CURVE_N ((int)(sizeof(s_curve) / sizeof(s_curve[0])))

/* BAT.1a: mV real -> domínio padrão da curva (identidade c/ âncoras de
 * fábrica). Ex.: âncoras 3450-4260 e entrada 3825 -> 3300 + (3825-3450)
 * * 900/810 = 3716. */
static int std_mv(int bmv)
{
    if (s_vmin == BATT_STD_MV_MIN && s_vmax == BATT_STD_MV_MAX) return bmv;
    return BATT_STD_MV_MIN + (bmv - s_vmin) * (BATT_STD_MV_MAX - BATT_STD_MV_MIN)
                             / (s_vmax - s_vmin);
}

BATT_VIS int curve_pct(int mv)
{
    const int smv = std_mv(mv);
    if (smv >= s_curve[0].mv) return 100;
    if (smv <= s_curve[CURVE_N - 1].mv) return 0;
    for (int i = 0; i + 1 < CURVE_N; i++) {
        if (smv <= s_curve[i].mv && smv >= s_curve[i + 1].mv) {
            const int span = s_curve[i].mv - s_curve[i + 1].mv;
            const int dp = s_curve[i].pct - s_curve[i + 1].pct;
            return s_curve[i + 1].pct + (smv - s_curve[i + 1].mv) * dp / span;
        }
    }
    return 0;
}

/* ---- BAT.1a: âncoras aprendidas (NVS ns "batt", chaves vmin/vmax) ---- */

static void calib_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(BATT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "vmin", s_vmin);
    nvs_set_i32(h, "vmax", s_vmax);
    nvs_commit(h);
    nvs_close(h);
}

BATT_VIS void battery_calib_load(void)
{
    s_vmin = BATT_STD_MV_MIN;
    s_vmax = BATT_STD_MV_MAX;
    nvs_handle_t h;
    if (nvs_open(BATT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "vmin", &v) == ESP_OK &&
            v >= BATT_ANCH_ABS_MIN && v <= 3700) s_vmin = v;
        if (nvs_get_i32(h, "vmax", &v) == ESP_OK &&
            v >= 3900 && v <= BATT_ANCH_ABS_MAX) s_vmax = v;
        nvs_close(h);
    }
    if (s_vmax - s_vmin < BATT_ANCH_SPAN) {   /* par inválido: de fábrica */
        s_vmin = BATT_STD_MV_MIN;
        s_vmax = BATT_STD_MV_MAX;
    }
    if (s_vmin != BATT_STD_MV_MIN || s_vmax != BATT_STD_MV_MAX) {
        ESP_LOGI(TAG, "âncoras da bateria (NVS): %d-%d mV", s_vmin, s_vmax);
    }
}

void battery_reset_calibration(void)
{
    s_vmin = BATT_STD_MV_MIN;
    s_vmax = BATT_STD_MV_MAX;
    calib_persist();
    /* BAT.1c: chamado da thread da UI — o % é religado pela task no poll
     * seguinte (flag volatile; nada de escrever s_pct fora da task). */
    s_recal = true;
    ESP_LOGI(TAG, "âncoras resetadas p/ padrão da curva: %d-%d mV "
             "(%% religado no próximo poll)", s_vmin, s_vmax);
}

int battery_anchor_min(void) { return s_vmin; }
int battery_anchor_max(void) { return s_vmax; }

/* Aprende com a leitura do poll (chamado pela task APÓS battery_ingest,
 * pois usa s_state/s_pct já atualizados). Pura sob BATTERY_UNIT_TEST:
 * o harness chama direto. Regras (docs/POWER.md §BAT.1):
 *  - vmin: estado BATERIA e pct <= 30 (perto do vazio: é onde o corte
 *    real acontece; fora disso um sag transitório de falante/tela
 *    envenenaria a âncora). Simétrico: célula que corta em 3.45 V
 *    SOBE o vmin de fábrica — senão o PDA "morre aos 15%".
 *  - vmax: estado CARREGANDO e acima do atual (só sobe: topo de carga
 *    CC-CV; meia-carga não move a âncora porque exige > vmax+15).
 *  - Sanidade: [2800,4400] absoluto e vão >= 600 mV; histerese 15 mV
 *    limita escritas NVS (cada movimento > 15 mV grava; bounded). */
BATT_VIS void battery_learn(int bmv)
{
    if (s_state == BSTAT_CHARGING) {
        if (bmv > BATT_ANCH_ABS_MAX) return;              /* leitura inválida */
        if (bmv > s_vmax + BATT_LEARN_HYST && bmv - s_vmin >= BATT_ANCH_SPAN) {
            s_vmax = bmv;
            calib_persist();
            ESP_LOGI(TAG, "âncora máx aprendida em carga: %d mV", bmv);
        }
    } else if (s_state == BSTAT_BATTERY && s_pct >= 0 && s_pct <= BATT_LEARN_PCT) {
        if (bmv < BATT_ANCH_ABS_MIN) return;              /* leitura inválida */
        const int d = bmv - s_vmin;
        if ((d < 0 ? -d : d) > BATT_LEARN_HYST && s_vmax - bmv >= BATT_ANCH_SPAN) {
            s_vmin = bmv;
            calib_persist();
            ESP_LOGI(TAG, "âncora mín aprendida perto do vazio: %d mV (pct %d)",
                     bmv, s_pct);
        }
    }
}

/* ---- BAT.1c: passo do % exibido na bateria ----
 * A curva dá um "alvo" (cur); o exibido anda na direção dele com ritmo e
 * banda morta: cai no máx. 1 ponto a cada BATT_PCT_DROP_POLLS; sobe só se
 * cur passar de exibido+BATT_PCT_RISE_HYST, no máx. 1 ponto a cada
 * BATT_PCT_RISE_POLLS. Uma descarga REAL (~0.5-1%/min) é muito mais lenta
 * que o teto de queda (5%/min) — o passo nunca esconde o fim da bateria;
 * o que ele filtra é a dança de ±5% que brilho/Wi-Fi/falante causavam. */
static void batt_pct_paced(int cur)
{
    if (s_pct < 0) { s_pct = cur; s_pct_step = 0; return; }
    s_pct_step++;
    if (cur < s_pct && s_pct_step >= BATT_PCT_DROP_POLLS) {
        s_pct--;
        s_pct_step = 0;
    } else if (cur > s_pct + BATT_PCT_RISE_HYST &&
               s_pct_step >= BATT_PCT_RISE_POLLS) {
        s_pct++;
        s_pct_step = 0;
    }
}

/* ---- BAT.2: máquina de estados carga/assentamento ----
 * Entra com a tensão JA compensada pelo divisor (mV de bateria).
 * Única dona de s_ema_mv/s_slow_mv/s_pct/s_state. Não-static sob
 * BATTERY_UNIT_TEST (BATT_VIS no topo) p/ o harness injetar perfis de
 * tensão direto.
 * BAT.1c: DOIS filtros — s_ema_mv (α=1/4, rápida: lógica de carga/
 * assentamento da BAT.2, intocada) e s_slow_mv (α=1/16, τ≈64 s: fonte do
 * % NA BATERIA, via batt_pct_paced). O % em carga segue a EMA rápida como
 * antes (comportamento validado em campo na 13ª rodada). */
BATT_VIS void battery_ingest(int bmv)
{
    const int delta = (s_prev_mv < 0) ? 0 : bmv - s_prev_mv;
    s_prev_mv = bmv;

    if (s_ema_mv < 0) {           /* 1ª leitura válida: ancora tudo */
        s_ema_mv = bmv;
        s_slow_mv = bmv;
        s_pct = curve_pct(bmv);
        s_pct_step = 0;
        s_state = BSTAT_BATTERY;
        return;
    }

    /* BAT.1c: EMA lenta sempre atualizada (round-to-nearest: >> em inteiro
     * truncaria p/ -inf e deixaria o filtro estacionado ~8 mV abaixo). */
    {
        const int d = bmv - s_slow_mv;
        s_slow_mv += (d >= 0 ? d + 8 : d - 8) >> BATT_SLOW_SHIFT;
    }

    if (s_recal) {                /* BAT.1a/1c: botão zerar → religa o % */
        s_recal = false;
        s_pct_step = 0;
        s_pct = curve_pct(s_state == BSTAT_CHARGING ? s_ema_mv : s_slow_mv);
        ESP_LOGI(TAG, "âncoras zeradas: %% religado na curva nova (%d%%)",
                 s_pct);
    }

    switch (s_state) {
    case BSTAT_CHARGING:
        if (delta <= -BATT_DROP_MV) {
            s_state = BSTAT_SETTLING;
            s_settle_polls = 0;
            s_settle_peak = s_ema_mv;
            s_rise_pend = -1;
            ESP_LOGI(TAG, "carregador saiu: segurando %d%% enquanto a "
                     "tensão de carga assenta (%d mV)", s_pct, s_ema_mv);
            s_ema_mv = (s_ema_mv * 7 + bmv) / 8;   /* lenta p/ não vazar p/ % */
            break;
        }
        s_ema_mv = (s_ema_mv + bmv) / 2;           /* segue a carga rápido */
        if (s_ema_mv > s_settle_peak) s_settle_peak = s_ema_mv;
        s_pct = curve_pct(s_ema_mv);
        s_pct_step = 0;
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
            s_pct_step = 0;
            break;
        }
        s_ema_mv = (s_ema_mv * 7 + bmv) / 8;       /* lenta: só diagnóstico */
        s_settle_polls++;
        if (s_settle_polls >= BATT_SETTLE_POLLS ||
            s_ema_mv <= s_settle_peak - BATT_SETTLE_SAG_MV ||
            (s_pre_chg_mv > 0 && s_ema_mv <= s_pre_chg_mv + 15)) {
            s_state = BSTAT_BATTERY;
            s_pct = curve_pct(s_ema_mv);           /* snap honesto p/ curva */
            s_slow_mv = s_ema_mv;   /* BAT.1c: religa a lenta no valor assentado */
            s_pct_step = 0;
            ESP_LOGI(TAG, "tensão assentada: %d%% (%d mV) após %d polls",
                     s_pct, s_ema_mv, s_settle_polls);
        }
        break;

    default: /* BSTAT_BATTERY */
        /* BAT.1c: salto de carregador COM CONFIRMAÇÃO. +40 mV num poll só
         * não basta mais: alívio de carga (dim do backlight, fim de beep,
         * rajada Wi-Fi) recupera 50-150 mV num poll e fantasiava de
         * carregador. Candidato: nível + salto ≥ 40. Confirma no poll
         * seguinte se (a) o nível se SUSTENTOU (≥ candidato-15) e (b) está
         * ACIMA da EMA lenta +40 — recuperação de sag volta AO nível do
         * filtro; carregador levanta ACIMA dele (CC/CV). Custo: o sufixo
         * "+" aparece 4 s depois — aceitável p/ evento de minutos. */
        if (s_rise_pend >= 0) {
            if (bmv >= s_rise_pend - BATT_RISE_TOL &&
                bmv >= s_rise_slow + BATT_RISE_MV) {
                s_state = BSTAT_CHARGING;
                s_pre_chg_mv = s_rise_ema;
                s_settle_peak = bmv;
                ESP_LOGI(TAG, "carregador confirmado: +%d mV sustentado "
                         "(pct segue a tensão de carga)", bmv - s_rise_ema);
                s_rise_pend = -1;
                s_ema_mv = (s_ema_mv + bmv) / 2;
                s_pct = curve_pct(s_ema_mv);
                s_pct_step = 0;
                break;
            }
            ESP_LOGI(TAG, "salto p/ %d mV descartado: transiente de carga, "
                     "não carregador", s_rise_pend);
            s_rise_pend = -1;
        } else if (delta >= BATT_RISE_MV) {
            s_rise_pend = bmv;
            s_rise_slow = s_slow_mv;
            s_rise_ema = s_ema_mv;
            ESP_LOGI(TAG, "salto candidato +%d mV (%d mV): confirmando no "
                     "próximo poll", delta, bmv);
        }
        s_ema_mv = (s_ema_mv * 3 + bmv) / 4;
        batt_pct_paced(curve_pct(s_slow_mv));      /* BAT.1c */
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
                battery_learn(bmv);   /* BAT.1a: usa s_state/s_pct do ingest */
                s_node_mv = nmv;
                /* BAT.1c: a UI mostra o filtro do qual o % deriva (lenta na
                 * bateria/assentando, rápida em carga) — mV e % coerentes. */
                s_batt_mv = (s_state == BSTAT_CHARGING) ? s_ema_mv : s_slow_mv;
                if (first) {
                    s_ready = true;
                    ESP_LOGI(TAG, "bateria: %d%% | %d mV | lenta %d mV | "
                             "nó %d mV | raw %d | %s",
                             s_pct, s_ema_mv, s_slow_mv, nmv, s_raw,
                             s_calibrated ? "calibrado (eFuse)" : "fallback linear");
                } else if (++s_polls % BATT_LOG_EVERY == 0) {
                    ESP_LOGI(TAG, "bateria: %d%% | %d mV | lenta %d mV | "
                             "nó %d mV | raw %d | %s",
                             s_pct, s_ema_mv, s_slow_mv, nmv, s_raw,
                             battery_state_str());
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
    battery_calib_load();   /* BAT.1a: âncoras aprendidas em campo (NVS) */

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

#ifdef BATTERY_UNIT_TEST
/* BAT.1c: accessor só p/ o harness de host (seções L–O verificam a EMA
 * lenta contra o % exibido). Não existe no firmware. */
int battery_slow_mv(void) { return s_slow_mv; }
#endif
int battery_node_mv(void) { return s_node_mv; }
bool battery_calibrated(void) { return s_calibrated; }

void battery_status_str(char *buf, int sz)
{
    if (!buf || sz <= 0) return;
    if (!s_ready) { snprintf(buf, sz, "--"); return; }
    /* BAT.1a: sufixo de estado p/ leitura SEM serial (na bateria o log
     * USB não existe): "+" = carregando, "~" = assentando, nada = bateria.
     * Ambos ASCII — sem risco de glifo fora do charset da PDA Mono. */
    if (s_state == BSTAT_CHARGING)       snprintf(buf, sz, "%d%%+", s_pct);
    else if (s_state == BSTAT_SETTLING)  snprintf(buf, sz, "%d%%~", s_pct);
    else                                 snprintf(buf, sz, "%d%%", s_pct);
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
     * de exibição; o canal certo sempre foi ADC2_CH4/GPIO53).
     * BAT.1a: âncoras no meio p/ coleta de campo sem serial. */
    snprintf(buf, sz, "%d%% (%d mV, ânc %d-%d mV, ADC%d_CH%d/GPIO%d, %s%s%s)",
             s_pct, s_batt_mv, s_vmin, s_vmax,
             (int)BOARD_BATT_ADC_UNIT + 1,
             (int)BOARD_BATT_ADC_CHANNEL, (int)BOARD_BATT_SENSE_GPIO,
             s_calibrated ? "cal. eFuse" : "fallback linear",
             s_state == BSTAT_BATTERY ? "" : ", ",
             s_state == BSTAT_BATTERY ? "" : battery_state_str());
}

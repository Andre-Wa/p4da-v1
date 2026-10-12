/*
 * idfinit_check_adc.c — repro/validação HOST dos inicializadores de
 * esp_adc usados em main/battery.c (BAT.0, v5.12.0).
 *
 * Por que existe: ver validacoes/REGRESSOES_BUILD.md — todo inicializador
 * de struct IDF é validado num TU de repro no host contra o layout COPIADO
 * dos headers PINADOS (aqui: IDF v5.5.1), porque erros de campo/enum só
 * apareceriam no build do usuário.
 *
 * Fontes copiadas (v5.5.1):
 *   components/hal/include/hal/adc_types.h
 *       (adc_unit_t, adc_channel_t, adc_atten_t, adc_bitwidth_t,
 *        adc_ulp_mode_t; P4: clk_src = soc_periph_adc_rtc_clk_src_t, aqui
 *        simplificado p/ int porque battery.c NÃO seta clk_src)
 *   components/esp_adc/include/esp_adc/adc_oneshot.h
 *       (adc_oneshot_unit_init_cfg_t, adc_oneshot_chan_cfg_t)
 *   components/esp_adc/include/esp_adc/adc_cali_scheme.h
 *       (adc_cali_line_fitting_config_t: SEM campo .chan;
 *        adc_cali_curve_fitting_config_t: COM campo .chan)
 *
 * Uso (forma correta — deve compilar LIMPO):
 *   gcc -std=gnu17 -Wall -Wextra -Werror=missing-braces \
 *       -c tools/idfinit_check_adc.c -o /tmp/adc_ok.o
 *
 * Uso (reproduzir o bug de propósito — deve falhar com "unknown field
 * 'chan'" no inicializador line-fitting):
 *   gcc -std=gnu17 -Wall -Wextra -Werror=missing-braces -DREPRODUZ_BUG \
 *       -c tools/idfinit_check_adc.c -o /tmp/adc_bug.o
 *
 * Armadilhas que este TU trava:
 *  1) '.ulp_mode' usa ADC_ULP_MODE_DISABLE (NÃO existe
 *     "ADC_ONESHOT_ULP_MODE_DISABLE" no IDF 5.5.1).
 *  2) adc_cali_line_fitting_config_t NÃO tem .chan (só curve fitting tem);
 *     confundir as duas structs = unknown field / campo deslocado.
 *  3) aten/bitwidth: ADC_ATTEN_DB_12 = 3, ADC_BITWIDTH_DEFAULT = 0.
 */
#include <stdint.h>

/* ---- hal/adc_types.h v5.5.1 (enums reais, valores reais) ---- */
typedef enum {
    ADC_UNIT_1,
    ADC_UNIT_2,
} adc_unit_t;

typedef enum {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
    ADC_CHANNEL_8, ADC_CHANNEL_9,
} adc_channel_t;

typedef enum {
    ADC_ATTEN_DB_0   = 0,
    ADC_ATTEN_DB_2_5 = 1,
    ADC_ATTEN_DB_6   = 2,
    ADC_ATTEN_DB_12  = 3,
} adc_atten_t;

typedef enum {
    ADC_BITWIDTH_DEFAULT = 0,
    ADC_BITWIDTH_12  = 12,
} adc_bitwidth_t;

typedef enum {
    ADC_ULP_MODE_DISABLE = 0,
    ADC_ULP_MODE_FSM     = 1,
    ADC_ULP_MODE_RISCV   = 2,
} adc_ulp_mode_t;

/* P4: SOC_ADC_DIG_CTRL_SUPPORTED && SOC_ADC_RTC_CTRL_SUPPORTED ->
 * adc_oneshot_clk_src_t = soc_periph_adc_rtc_clk_src_t (enum de clocks).
 * battery.c não atribui clk_src (fica 0 = default), então int basta p/ o
 * layout do inicializador. */
typedef int adc_oneshot_clk_src_t;

/* ---- esp_adc/adc_oneshot.h v5.5.1 ---- */
typedef struct {
    adc_unit_t unit_id;
    adc_oneshot_clk_src_t clk_src;
    adc_ulp_mode_t ulp_mode;
} adc_oneshot_unit_init_cfg_t;

typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;

/* ---- esp_adc/adc_cali_scheme.h v5.5.1 ---- */
typedef struct {
    adc_unit_t unit_id;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_line_fitting_config_t;      /* NOTE: sem .chan */

typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;     /* curve fitting TEM .chan */

/* ---- board_config.h (valores que battery.c usa) ---- */
#define BOARD_BATT_ADC_UNIT     ADC_UNIT_2      /* GPIO53 = ADC2_CH4 no P4 */
#define BOARD_BATT_ADC_CHANNEL  ADC_CHANNEL_4
#define BOARD_BATT_ATTEN        ADC_ATTEN_DB_12

/* ---- espelho EXATO dos inicializadores de battery.c ---- */
static const adc_oneshot_unit_init_cfg_t s_ucfg = {
    .unit_id = BOARD_BATT_ADC_UNIT,
    .ulp_mode = ADC_ULP_MODE_DISABLE,
};

static const adc_oneshot_chan_cfg_t s_ccfg = {
    .atten = BOARD_BATT_ATTEN,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};

static const adc_cali_line_fitting_config_t s_line = {
    .unit_id = BOARD_BATT_ADC_UNIT,
    .atten = BOARD_BATT_ATTEN,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};

static const adc_cali_curve_fitting_config_t s_curve = {
    .unit_id = BOARD_BATT_ADC_UNIT,
    .chan = BOARD_BATT_ADC_CHANNEL,
    .atten = BOARD_BATT_ATTEN,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};

#ifdef REPRODUZ_BUG
/* O bug que NÃO podemos cometer: .chan no config de LINE fitting. */
static const adc_cali_line_fitting_config_t s_bug = {
    .unit_id = BOARD_BATT_ADC_UNIT,
    .chan = BOARD_BATT_ADC_CHANNEL,
    .atten = BOARD_BATT_ATTEN,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};
#endif

/* uso-para-o-linker-não-reclamar e p/ assert de valores em tempo de
 * compilação (enum errado vira erro aqui, não no build do usuário). */
int idfinit_check_adc(void);
int idfinit_check_adc(void)
{
    _Static_assert(ADC_ATTEN_DB_12 == 3, "atten 12dB != 3");
    _Static_assert(ADC_BITWIDTH_DEFAULT == 0, "bitwidth default != 0");
    _Static_assert(ADC_ULP_MODE_DISABLE == 0, "ulp disable != 0");
    _Static_assert(ADC_CHANNEL_4 == 4, "canal 4 != 4");
    _Static_assert(ADC_UNIT_2 == 1, "ADC_UNIT_2 != 1");
    return (int)s_ucfg.unit_id + (int)s_ccfg.atten + (int)s_line.unit_id +
           (int)s_curve.chan
#ifdef REPRODUZ_BUG
           + (int)s_bug.unit_id
#endif
           ;
}

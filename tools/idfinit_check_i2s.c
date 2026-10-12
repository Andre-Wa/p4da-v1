/*
 * idfinit_check_i2s.c — repro/validação HOST do inicializador de
 * i2s_std_config_t usado em main/audio_i2s.c.
 *
 * Por que existe: a família de erros "-Werror=missing-braces em arquivo
 * IDF-only" (ver validacoes/REGRESSOES_BUILD.md) só aparece no build do
 * usuário. Este TU copia o layout exato das structs E dos macros do IDF
 * PINADO (v5.5.1, components/esp_driver_i2s/include/driver/i2s_std.h, ramo
 * #else = não-ESP32, ou seja P4: SOC_I2S_HW_VERSION_2=1) e espelha o
 * inicializador do audio_i2s.c. Se compilar limpo aqui com os MESMOS flags,
 * compila limpo lá.
 *
 * Uso (forma correta — deve compilar LIMPO):
 *   gcc -std=gnu17 -Wall -Wextra -Werror=missing-braces -c tools/idfinit_check_i2s.c -o /tmp/x.o
 *
 * Uso (reproduzir o bug de propósito — deve dar 2x missing-braces):
 *   gcc -std=gnu17 -Wall -Wextra -Werror=missing-braces -DREPRODUZ_BUG \
 *       -c tools/idfinit_check_i2s.c -o /tmp/x.o
 *
 * Histórico:
 *   v5.11.2 -> '.invert_flags = false' atribuía escalar a uma STRUCT de
 *              bitfields {mclk_inv,bclk_inv,ws_inv} → 2x "missing braces
 *              around initializer" apontando na CHAVE externa do std_cfg
 *              (linha do '= {'), o que é enganoso: o erro NÃO está nas
 *              sub-chaves clk/slot/gpio, está no invert_flags.
 *   v5.11.3 -> invert_flags omitido (zero-init). Limpo, mas verbose.
 *   v5.11.4 -> clk/slot voltam aos MACROS OFICIAIS (I2S_STD_CLK_DEFAULT_CONFIG
 *              / I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG), que se adaptam sozinhos
 *              à divergência de campos entre targets; gpio_cfg usa
 *              '.invert_flags = {0}' EXPLÍCITO (a forma correta). Limpo.
 */
#include <stdbool.h>
#include <stdint.h>

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef int i2s_data_bit_width_t;
typedef int i2s_slot_bit_width_t;
typedef int i2s_slot_mode_t;
typedef int i2s_std_slot_mask_t;
typedef int i2s_clock_src_t;
typedef int i2s_mclk_multiple_t;

#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_BIT_WIDTH_AUTO   0
#define I2S_SLOT_MODE_STEREO      1
#define I2S_STD_SLOT_BOTH         3
#define I2S_CLK_SRC_DEFAULT       0
#define I2S_MCLK_MULTIPLE_256   256

/* ---- macros REAIS de i2s_std.h v5.5.1, ramo #else (não-ESP32 = P4) ---- */
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { \
    .sample_rate_hz = rate, \
    .clk_src = I2S_CLK_SRC_DEFAULT, \
    .ext_clk_freq_hz = 0, \
    .mclk_multiple = I2S_MCLK_MULTIPLE_256, \
    .bclk_div = 8, \
}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) { \
    .data_bit_width = bits_per_sample, \
    .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, \
    .slot_mode = mono_or_stereo, \
    .slot_mask = (mono_or_stereo == I2S_SLOT_MODE_MONO) ? \
                I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH, \
    .ws_width = bits_per_sample, \
    .ws_pol = false, \
    .bit_shift = true, \
    .left_align = true, \
    .big_endian = false, \
    .bit_order_lsb = false \
}
#define I2S_SLOT_MODE_MONO 0
#define I2S_STD_SLOT_LEFT  1

/* ---- structs copiadas de i2s_std.h v5.5.1 (P4) ---- */
typedef struct {
    uint32_t                sample_rate_hz;
    i2s_clock_src_t         clk_src;
    uint32_t                ext_clk_freq_hz;   /* SOC_I2S_HW_VERSION_2 */
    i2s_mclk_multiple_t     mclk_multiple;
    uint32_t                bclk_div;
} i2s_std_clk_config_t;

typedef struct {
    i2s_data_bit_width_t    data_bit_width;
    i2s_slot_bit_width_t    slot_bit_width;
    i2s_slot_mode_t         slot_mode;
    i2s_std_slot_mask_t     slot_mask;
    uint32_t                ws_width;
    bool                    ws_pol;
    bool                    bit_shift;
    bool                    left_align;        /* !SOC_I2S_HW_VERSION_1 */
    bool                    big_endian;
    bool                    bit_order_lsb;
} i2s_std_slot_config_t;

typedef struct {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    struct {
        uint32_t   mclk_inv: 1;
        uint32_t   bclk_inv: 1;
        uint32_t   ws_inv: 1;
    } invert_flags;                            /* STRUCT, não bool! */
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t    clk_cfg;
    i2s_std_slot_config_t   slot_cfg;
    i2s_std_gpio_config_t   gpio_cfg;
} i2s_std_config_t;

#define BEEP_RATE 44100
#define BOARD_I2S_MCLK_GPIO 13
#define BOARD_I2S_BCLK_GPIO 12
#define BOARD_I2S_LRCK_GPIO 10
#define BOARD_I2S_DOUT_GPIO  9

void idfinit_check_i2s(void);
void idfinit_check_i2s(void)
{
#ifdef REPRODUZ_BUG
    /* FORMA QUEBRADA (v5.11.2): '.invert_flags = false' → 2x missing-braces.
     * Mantida aqui só para documentar/reproduzir o erro byte-a-byte. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_LRCK_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = GPIO_NUM_NC,
            .invert_flags = false,             /* <<< BUG: escalar em struct */
        },
    };
#else
    /* FORMA CORRETA (v5.11.4): espelho 1:1 do std_cfg de main/audio_i2s.c */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_LRCK_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = GPIO_NUM_NC,
            .invert_flags = {0},               /* STRUCT de bitfields — nunca '= false' */
        },
    };
#endif
    (void)std_cfg;
}

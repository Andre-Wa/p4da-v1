/**
 * audio_i2s.c — M5c.1: bring-up do caminho local ES8311 + NS4150.
 *
 * Topologia (board_config.h / esquemático GUITION):
 *   I2S0: MCLK 13, BCLK 12, LRCK 10, DOUT 9 (DIN 48 fica p/ mic em M5c.x)
 *   I2C0 compartilhado com o GT911 (bus_handle do touch_init)
 *   PA_EN 11 → NS4150 (o es8311_codec liga/desliga no open/close)
 *
 * Endereço: macro ES8311_CODEC_DEFAULT_ADDR (0x30, forma 8-bit; o
 * componente desloca >>1 internamente).
 */

#include "audio_i2s.h"
#include "board_config.h"
#include "touch_init.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <string.h>

static const char *TAG = "audio_i2s";

static i2s_chan_handle_t s_tx = NULL;
static esp_codec_dev_handle_t s_dev = NULL;
static bool s_ready = false;
/* v5.13.0: serializa beeps. O botão "teste de som" da UI spawna uma task
 * por clique; dois cliques rápidos empilhavam duas senoides no MESMO
 * canal I2S (log: "Adev_Codec: Input already open" + som dobrado/
 * distorcido). Com o mutex, o 2º clique dentro da janela do 1º é DESCARTADO
 * (try-take de 50 ms), não enfileirado — teste de som é idempotente. */
static SemaphoreHandle_t s_beep_mtx = NULL;
/* Espelho do estado enable/disable do canal I2S TX. NÓS habilitamos antes
 * de cada esp_codec_dev_open() e o componente desabilita no close(); sem
 * este espelho o disable INTERNO do set_fmt() (audio_codec_data_i2s.c:
 * "disable internally") bate num canal já desabilitado e loga
 * "E i2s_common: i2s_channel_disable: the channel has not been enabled
 * yet" a cada beep (visto no log de 2026-10-11). Ruído puro, mas some. */
static bool s_chan_on = false;

#define BEEP_RATE 44100

esp_err_t audio_i2s_init(void)
{
    if (s_ready) return ESP_OK;

    /* ---- I2S0 TX (playback) ---- */
    i2s_chan_config_t ch_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&ch_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel falhou");
        return ESP_FAIL;
    }
    /* M5c.1.2 (v5.11.4): macros OFICIAIS do IDF para clk/slot — eles se
     * adaptam sozinhos à divergência de campos entre targets (P4 =
     * SOC_I2S_HW_VERSION_2 usa left_align/big_endian/bit_order_lsb; ESP32
     * clássico usa msb_right). gpio_cfg com chaves explícitas.
     *
     * ATENÇÃO (causa raiz do bug v5.11.x): no IDF 5.5.1 '.invert_flags' é uma
     * STRUCT de bitfields {mclk_inv,bclk_inv,ws_inv}, NÃO um bool. Escrever
     * '.invert_flags = false' = atribuir escalar a struct → GCC dispara 2x
     * -Werror=missing-braces apontando na CHAVE de abertura do std_cfg,
     * o que é enganoso. O certo é '= {0}' (zero-init = nenhuma inversão).
     * Repro host byte-a-byte: tools/idfinit_check_i2s.c */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_LRCK_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = GPIO_NUM_NC,        /* mic entra em M5c.x */
            .invert_flags = {0},        /* STRUCT de bitfields — nunca '= false' */
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "init do canal I2S falhou");
        return ESP_FAIL;
    }
    /* NÃO habilitar aqui de propósito: esp_codec_dev_open() faz
     * disable→reconfig→enable internamente; habilitar agora só adiava o
     * E-log do 2º beep em diante (ver s_chan_on). O beep habilita logo
     * antes do open e o close devolve o canal desabilitado. */

    /* ---- interfaces do codec ---- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = board_i2c_bus(),   /* MESMO bus do GT911 */
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) { ESP_LOGE(TAG, "i2c ctrl if falhou"); return ESP_FAIL; }
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) { ESP_LOGE(TAG, "gpio if falhou"); return ESP_FAIL; }

    es8311_codec_cfg_t es_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = BOARD_AMP_PA_EN_GPIO,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) { ESP_LOGE(TAG, "es8311_codec_new falhou (I2C?)"); return ESP_FAIL; }

    audio_codec_i2s_cfg_t i2s_cfg = { .tx_handle = s_tx, .rx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) { ESP_LOGE(TAG, "i2s data if falhou"); return ESP_FAIL; }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) { ESP_LOGE(TAG, "esp_codec_dev_new falhou"); return ESP_FAIL; }

    esp_codec_dev_set_out_vol(s_dev, BOARD_SPK_VOL_DEFAULT);
    s_beep_mtx = xSemaphoreCreateMutex();
    if (!s_beep_mtx) { ESP_LOGE(TAG, "sem RAM p/ mutex do beep"); return ESP_ERR_NO_MEM; }
    s_ready = true;
    ESP_LOGI(TAG, "ES8311 + NS4150 prontos (I2S0, I2C condiv., PA=GPIO%d, vol=%d)",
             (int)BOARD_AMP_PA_EN_GPIO, BOARD_SPK_VOL_DEFAULT);
    return ESP_OK;
}

bool audio_i2s_ready(void) { return s_ready; }

void audio_i2s_set_vol(int pct)
{
    if (s_dev) esp_codec_dev_set_out_vol(s_dev, (float)pct);
}

esp_err_t audio_i2s_beep(int ms)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    /* v5.13.0: clique duplo não empilha mais duas senoides (ruído alto e
     * distorcido até o fim do teste). Janela de 50 ms: cliques nessa
     * janela são o caso patológico (spawn de task quase simultâneo);
     * descartamos em vez de enfileirar p/ o som não "atrasar". */
    if (xSemaphoreTake(s_beep_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "beep descartado: outro beep em andamento (clique duplo?)");
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BEEP_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (!s_chan_on) {
        if (i2s_channel_enable(s_tx) == ESP_OK) s_chan_on = true;
    }
    if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open falhou");
        xSemaphoreGive(s_beep_mtx);
        return ESP_FAIL;
    }
    s_chan_on = true;   /* open religa o canal após o reconfig interno */
    /* v5.14.0: log no INÍCIO do som. Na v5.13.0 esta linha saía DEPOIS do
     * write bloqueante de 1200 ms (fim da função) e a leitura de campo
     * ficava enganosa: "Open codec device OK" → (1,2 s de silêncio no log)
     * → "beep ... no falante" → W "descartado" de cliques que na verdade
     * aconteceram DURANTE o beep. A ordem do log agora bate com o ouvido. */
    ESP_LOGI(TAG, "beep 440 Hz %d ms no falante", ms);
    const int total = BEEP_RATE * ms / 1000;
    int16_t buf[512];           /* 256 amostras estéreo */
    int pos = 0;
    while (pos < total) {
        int n = total - pos < 256 ? total - pos : 256;
        for (int i = 0; i < n; i++) {
            /* envelope simples p/ não clicar: 5 ms de rampa nas pontas */
            int g = BOARD_BEEP_GAIN;
            if (pos + i < BEEP_RATE / 200) g = (pos + i) * BOARD_BEEP_GAIN / (BEEP_RATE / 200);
            int rem = total - (pos + i);
            if (rem < BEEP_RATE / 200) g = rem * BOARD_BEEP_GAIN / (BEEP_RATE / 200);
            int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * 440.0f *
                                       (float)(pos + i) / (float)BEEP_RATE) * (float)g);
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        if (esp_codec_dev_write(s_dev, buf, (uint32_t)n * 4) != ESP_OK) break;
        pos += n;
    }
    esp_codec_dev_close(s_dev);   /* desabilita o canal internamente */
    s_chan_on = false;
    xSemaphoreGive(s_beep_mtx);
    return ESP_OK;
}

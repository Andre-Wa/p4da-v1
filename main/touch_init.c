/**
 * touch_init.c
 *
 * Inicializa o I2C compartilhado e o controlador de touch GT911.
 * Pinos I2C (SDA=7, SCL=8) confirmados pelo BSP de referência da
 * comunidade (ver board_config.h).
 */

#include "touch_init.h"
#include "board_config.h"
#include "power_mgmt.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"

static const char *TAG = "touch_init";

/* M4.14.3: cegueira de toque no STANDBY com wake_on_touch OFF. O caminho
 * de wake pela ISR já era gateado (M4.14), mas o toque em ELEMENTO
 * INTERATIVO chegava ao Slint (a integração slint-esp polla o driver por
 * conta própria): o callback do elemento rodava com a tela "desligada"
 * (action fantasma: ex.: tocar onde estava o Voltar navegava) e acordava
 * por "atividade UI/USB" (relato + log da validação de 2026-10-04).
 * Proxy em get_xy: read_data segue drenando o GT911 (sem flood de pontos
 * velhos no wake), mas o Slint recebe 0 pontos enquanto cego. */
static bool (*s_real_get_xy)(esp_lcd_touch_handle_t, uint16_t *, uint16_t *,
                             uint16_t *, uint8_t *, uint8_t) = NULL;
/* M4.15: com o GT911 dormindo no standby (chave OFF), read_data não pode
 * bater no I2C (NACK a cada poll = spam + bus ocupado): enquanto cego,
 * o read é fingido; get_xy já devolvia 0 pontos (M4.14.3). */
static esp_err_t (*s_real_read_data)(esp_lcd_touch_handle_t) = NULL;

static esp_err_t blind_read_data(esp_lcd_touch_handle_t tp)
{
    if (power_mgmt_touch_blind()) return ESP_OK;
    return s_real_read_data(tp);
}

static bool blind_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                         uint16_t *strength, uint8_t *point_num,
                         uint8_t max_point_num)
{
    if (power_mgmt_touch_blind()) {
        if (point_num) *point_num = 0;
        return false;
    }
    return s_real_get_xy(tp, x, y, strength, point_num, max_point_num);
}

esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch)
{
    /* rst_gpio_num/int_gpio_num ficam em NC de propósito. O GT911
     * seleciona o próprio endereço I2C (0x5D ou 0x14) com base no estado
     * do pino INT durante o pulso de reset — ao fornecer rst_gpio_num
     * real (testado: GPIO22) sem sequenciar o INT corretamente durante
     * esse pulso, o chip ficou num endereço diferente do esperado e toda
     * comunicação I2C passou a falhar com NACK (testado e confirmado
     * quebrando o boot). Com NC em ambos, o driver nunca mexe nesses
     * pinos e o GT911 fica no endereço que assume sozinho no power-on —
     * que é o que comprovadamente funciona. Se algum dia valer a pena
     * revisitar isso, precisa implementar a sequência de reset completa
     * (INT como saída, nível definido, timing certo do datasheet do
     * GT911) em vez de só passar os pinos pro driver genérico. */
    i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, /* placa já tem pull-up externo */
    };
    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_handle), TAG, "criar barramento I2C");

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = BOARD_I2C_CLK_HZ;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus_handle, &tp_io_config, &tp_io_handle),
                         TAG, "criar panel_io I2C do GT911");

    /* x_max/y_max na orientação NATIVA (retrato) do painel — o GT911
     * sempre reporta coordenadas cruas nessa orientação, mesmo com a UI
     * rotacionada via Slint (Rotate90). swap_xy=1 e mirror_x=1 foram
     * confirmados testando toque nos 4 cantos da tela já rotacionada;
     * são o complemento necessário da rotação Rotate90 configurada em
     * main.cpp — se a rotação mudar, esses flags provavelmente também
     * precisam mudar. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES_NATIVE,
        .y_max = BOARD_LCD_V_RES_NATIVE,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, out_touch),
                         TAG, "criar driver GT911");

    s_real_get_xy = (*out_touch)->get_xy;   /* M4.14.3: proxy de cegueira */
    (*out_touch)->get_xy = blind_get_xy;
    s_real_read_data = (*out_touch)->read_data;   /* M4.15: sem I2C cego */
    (*out_touch)->read_data = blind_read_data;

    ESP_LOGI(TAG, "GT911 inicializado no I2C%d (SDA=%d SCL=%d)",
             BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    return ESP_OK;
}

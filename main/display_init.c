/**
 * display_init.c
 *
 * Bring-up do painel ST7701S via MIPI-DSI, SEM passar por nenhum
 * framework de UI (nem LVGL, nem Slint) — só devolve um
 * esp_lcd_panel_handle_t "cru", pronto pra qualquer coisa desenhar nele.
 *
 * A sequência de comandos de inicialização (s_st7701_init_cmds) e os
 * timings de DPI abaixo foram extraídos diretamente do código-fonte do
 * BSP de referência para essa placa:
 *   https://github.com/NickyDark1/esp32_p4_jc4880p433c_bsp
 * (src/bsp_display.c, licença Apache 2.0). Sem essa sequência
 * específica, o painel não respondia (a inicialização padrão genérica
 * do componente esp_lcd_st7701 trava esperando uma leitura que o painel
 * nunca responde).
 */

#include "display_init.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_init";

static esp_ldo_channel_handle_t s_ldo_mipi_phy = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

/* Sequência de init vendor-specific desse painel (copiada do BSP de
 * referência — ver comentário no topo do arquivo). */
static const st7701_lcd_init_cmd_t s_st7701_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},

    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},

    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},

    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

static esp_err_t power_on_mipi_phy(void)
{
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BOARD_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = BOARD_MIPI_DSI_PHY_LDO_MV,
    };
    return esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_mipi_phy);
}

esp_err_t board_display_init(esp_lcd_panel_handle_t *out_panel)
{
    esp_err_t ret;

    ret = power_on_mipi_phy();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "falha ao ligar LDO do PHY MIPI-DSI: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 1. Barramento DSI */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BOARD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BOARD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), TAG, "criar barramento DSI");

    /* Dá tempo da PHY estabilizar antes de mandar qualquer comando —
     * sem isso, o primeiro comando/leitura pode travar esperando uma
     * resposta que o painel ainda não está pronto pra dar (foi
     * exatamente o que aconteceu antes dessa correção). */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Canal de comando (DBI) */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &io), TAG, "criar canal DBI");

    /* 3. Canal de vídeo (DPI) — timings confirmados para esse painel
     * (480x800 @ ~60Hz). */
    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_LCD_DPI_CLOCK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = BOARD_LCD_NUM_FB,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES_NATIVE,
            .v_size = BOARD_LCD_V_RES_NATIVE,
            .hsync_pulse_width = 12,
            .hsync_back_porch = 42,
            .hsync_front_porch = 42,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 166,
        },
        .flags = { .use_dma2d = true },
    };

    /* Atribuição sequencial em vez de inicializador agregado — ver nota
     * abaixo sobre por que isso é necessário nesse toolchain. */
    st7701_vendor_config_t vendor_config = {0};
    vendor_config.flags.use_mipi_interface = 1;
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.init_cmds = s_st7701_init_cmds;
    vendor_config.init_cmds_size = sizeof(s_st7701_init_cmds) / sizeof(s_st7701_init_cmds[0]);

    /* NOTA sobre atribuição sequencial: usar inicializador agregado aqui
     * (`= { .flags.use_mipi_interface = 1, .mipi_config = {...} }`) fez
     * a flag `use_mipi_interface` chegar como 0 em tempo de execução
     * nesse toolchain (GCC 14.2.0 + flags de hardening do ESP-IDF).
     * Atribuição sequencial elimina qualquer ambiguidade. */

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_config, &panel), TAG, "criar painel ST7701S");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset do painel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init do painel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "ligar display");

    ESP_LOGI(TAG, "painel ST7701S inicializado (%dx%d nativo)",
             BOARD_LCD_H_RES_NATIVE, BOARD_LCD_V_RES_NATIVE);

    s_panel = panel;
    *out_panel = panel;
    return ESP_OK;
}

void board_display_panel_blank(bool blank)
{
    if (!s_panel) return;
    /* DISPOFF/DISPON: para de mostrar conteúdo (o stream DPI continua,
     * mas com a tela blank + backlight 0 o consumo visível é zero e o
     * wake é instantâneo — ver docs/POWER.md). */
    esp_lcd_panel_disp_on_off(s_panel, !blank);
}

static bool s_bl_ready = false;
static uint8_t s_bl_pct = 100;

static void backlight_init_once(void)
{
    if (s_bl_ready) return;
    /* Backlight é PWM via LEDC (não um GPIO digital simples). */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BOARD_LCD_BL_LEDC_TIMER,
        .freq_hz = BOARD_LCD_BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BOARD_LCD_BL_LEDC_CHANNEL,
        .timer_sel = BOARD_LCD_BL_LEDC_TIMER,
        .duty = 0,
    };
    ledc_channel_config(&ch_cfg);
    s_bl_ready = true;
}

void board_display_backlight_set(uint8_t percent)
{
    backlight_init_once();
    if (percent > 100) percent = 100;
    s_bl_pct = percent;
    uint32_t duty = 0;
    if (percent > 0) {
        /* piso de ~10%: tela "acesa" nunca fica preta total */
        uint8_t eff = (percent < 10) ? 10 : percent;
        duty = (1023U * eff) / 100U;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BL_LEDC_CHANNEL);
}

void board_display_backlight_on(void)
{
    board_display_backlight_set(100);
}

uint8_t board_display_backlight_get(void)
{
    return s_bl_pct;
}

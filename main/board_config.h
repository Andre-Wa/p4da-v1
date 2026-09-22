/**
 * board_config.h
 *
 * Parâmetros de hardware da GUITION JC4880P443C_I_W (módulo JC-ESP32P4-M3,
 * ESP32-P4 + ESP32-C6).
 *
 * ATENÇÃO — este arquivo foi reconciliado contra o BSP comunitário da placa
 * EXATA (ultramcu/guition-jc4880p4-bsp) e as notas de campo
 * (ultramcu/guition-jc4880p443c-i-w), que publicam o esquemático do
 * fabricante. Onde o protótipo anterior (JC4880P433) divergia, o valor
 * correto para ESTA placa prevalece e está marcado com [FIX].
 *
 * Legenda:
 *   [OK]   validado em hardware no protótipo anterior
 *   [FIX]  corrigido neste projeto (protótipo tinha valor errado/NC)
 *   [NEW]  bloco novo, ainda não exercitado neste firmware
 *   [HW?]  depende de confirmação no esquemático/hardware antes de usar
 */

#pragma once

#include "driver/gpio.h"

/* ---------------------------------------------------------------------
 * I2C compartilhado (touch GT911 + codec ES8311)              [OK]
 * --------------------------------------------------------------------- */
#define BOARD_I2C_PORT        0
#define BOARD_I2C_SDA_GPIO    7
#define BOARD_I2C_SCL_GPIO    8
#define BOARD_I2C_CLK_HZ      400000

#define BOARD_GT911_ADDR_1    0x5D   /* [FIX] primário nesta placa */
#define BOARD_GT911_ADDR_2    0x14   /* fallback (probe) */

/* [FIX] No protótipo: RST=22 / INT=21 (herdados da JC4880P433) e NÃO
 * usados. No esquemático da JC4880P443C_I_W o RST do GT911 é GPIO3; o
 * INT segue sem uso confiável documentado (comunidade cita GPIO21).
 * Mantemos RST como NC no driver (funciona via power-on-reset, testado)
 * mas deixamos o pino real definido para quando implementarmos a
 * sequência de reset completa do datasheet (necessária p/ wake-on-touch
 * no deep sleep — ver docs/POWER.md). */
#define BOARD_TOUCH_RST_GPIO   GPIO_NUM_3
#define BOARD_TOUCH_INT_GPIO   GPIO_NUM_21   /* [HW?] não usado ainda */

/* ---------------------------------------------------------------------
 * Painel MIPI-DSI (ST7701S)                                   [OK]
 * --------------------------------------------------------------------- */
#define BOARD_MIPI_DSI_LANE_NUM           2
#define BOARD_MIPI_DSI_LANE_BITRATE_MBPS  500

/* Resolução NATIVA do painel é retrato (480x800); a UI roda em paisagem
 * (800x480) via rotação de software do Slint (Rotate90). */
#define BOARD_LCD_H_RES_NATIVE   480
#define BOARD_LCD_V_RES_NATIVE   800

#define BOARD_LCD_DPI_CLOCK_MHZ   34
#define BOARD_LCD_NUM_FB          2

#define BOARD_MIPI_DSI_PHY_LDO_CHAN   3
#define BOARD_MIPI_DSI_PHY_LDO_MV     2500

#define BOARD_LCD_RST_GPIO     GPIO_NUM_5

/* Backlight: PWM (LEDC) no GPIO23 alimentando o boost MP3202. O BSP
 * comunitário trata como GPIO ativo-alto simples; PWM funciona e ainda
 * nos dá dimerização para o modo standby (nível mínimo ~10%). */
#define BOARD_LCD_BL_GPIO           GPIO_NUM_23
#define BOARD_LCD_BL_LEDC_TIMER     LEDC_TIMER_1
#define BOARD_LCD_BL_LEDC_CHANNEL   LEDC_CHANNEL_1
#define BOARD_LCD_BL_PWM_FREQ_HZ    20000

/* ---------------------------------------------------------------------
 * Cartão MicroSD (SDMMC 4-bit) + LDO de alimentação       [FIX][OK]
 *
 * [FIX] CRÍTICO: o protótipo montava o cartão em 1-bit @ 400 kHz
 * (SDMMC_FREQ_PROBING, width=1) e declarava D2=GPIO46 / D3=GPIO45.
 * No esquemático desta placa D2/D3 são GPIO41/GPIO42 e o GPIO45 é NC
 * (R10 não populado — o "power enable" documentado não existe).
 * O modo 1-bit "funcionava" porque nunca toca D1..D3, mas a ~400 kHz
 * o cartão rende dezenas de KB/s — inviável p/ player de música e
 * gerenciador de arquivos. Aqui: 4-bit @ 40 MHz com fallback.
 * --------------------------------------------------------------------- */
#define BOARD_SD_LDO_CHAN    4
#define BOARD_SD_LDO_MV      3300

#define BOARD_SD_CLK_GPIO    GPIO_NUM_43
#define BOARD_SD_CMD_GPIO    GPIO_NUM_44
#define BOARD_SD_D0_GPIO     GPIO_NUM_39
#define BOARD_SD_D1_GPIO     GPIO_NUM_40
#define BOARD_SD_D2_GPIO     GPIO_NUM_41   /* [FIX] era 46 */
#define BOARD_SD_D3_GPIO     GPIO_NUM_42   /* [FIX] era 45 (NC!) */

/* ---------------------------------------------------------------------
 * Wi-Fi / Bluetooth — ESP32-C6 via ESP-Hosted (SDIO)         [NEW]
 * Barramento SEPARADO do SD, então SD + Wi-Fi coexistem.
 * O C6 precisa rodar firmware slave ESP-Hosted compatível com o
 * componente host (ver tools/flash_c6_wifi.sh): o que vem de fábrica
 * (2.3.0) casa com host ~2.12 => reflashear p/ network_adapter 2.12.9.
 * --------------------------------------------------------------------- */
#define BOARD_WIFI_SDIO_CLK_GPIO   GPIO_NUM_18
#define BOARD_WIFI_SDIO_CMD_GPIO   GPIO_NUM_19
#define BOARD_WIFI_SDIO_D0_GPIO    GPIO_NUM_14
#define BOARD_WIFI_SDIO_D1_GPIO    GPIO_NUM_15
#define BOARD_WIFI_SDIO_D2_GPIO    GPIO_NUM_16
#define BOARD_WIFI_SDIO_D3_GPIO    GPIO_NUM_17
#define BOARD_C6_RESET_GPIO        GPIO_NUM_54   /* ativo-alto */

/* ---------------------------------------------------------------------
 * Áudio — codec ES8311 (I2S) + amplificador NS4150           [NEW]
 * Controle do codec (I2C) compartilha o barramento do touch (7/8).
 * --------------------------------------------------------------------- */
#define BOARD_I2S_MCLK_GPIO    GPIO_NUM_13
#define BOARD_I2S_BCLK_GPIO    GPIO_NUM_12
#define BOARD_I2S_LRCK_GPIO    GPIO_NUM_10
#define BOARD_I2S_DOUT_GPIO    GPIO_NUM_9    /* P4 -> codec (playback) */
#define BOARD_I2S_DIN_GPIO     GPIO_NUM_48   /* codec -> P4 (mic/line) */
#define BOARD_AMP_PA_EN_GPIO   GPIO_NUM_11   /* NS4150 enable */

/* ---------------------------------------------------------------------
 * Botões / LED / UART                                        [NEW]
 * --------------------------------------------------------------------- */
#define BOARD_BOOT_BTN_GPIO    GPIO_NUM_35   /* wake source do deep sleep */
#define BOARD_LED_GPIO         GPIO_NUM_26   /* compartilha c/ RS485 TX */
#define BOARD_UART0_TX_GPIO    GPIO_NUM_37
#define BOARD_UART0_RX_GPIO    GPIO_NUM_38

/* ---------------------------------------------------------------------
 * USB Host (teclado)                                          [OK]
 * Porta "High Speed USB" = USB-OTG nativo do P4 (host p/ teclado).
 * Porta "Full Speed USB" = USB-Serial-JTAG (flash/monitor) — NÃO serve.
 * --------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Bateria — IP5306                                           [HW?]
 * O IP5306 NÃO tem I2C nem telemetria: só LEDs de carga e pino KEY.
 * Não há divisor de VOUT-BAT roteado a um ADC do P4 na folha 02 do
 * esquemático. Até confirmar (folha 05 / medição), o sistema trata
 * "nível de bateria" como NÃO DISPONÍVEL e exibe "—" na status bar.
 * Se um dia houver divisor, basta ler o canal ADC correspondente aqui.
 * --------------------------------------------------------------------- */
#define BOARD_BATT_ADC_CHANNEL   (-1)   /* -1 = sem medição disponível */

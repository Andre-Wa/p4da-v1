# Hardware — GUITION JC4880P443C_I_W (módulo JC-ESP32P4-M3)

Fontes primárias:
- BSP comunitário da placa exata: `ultramcu/guition-jc4880p4-bsp` (MIT), que
  publica o mapa de pinos verificado em hardware e linka o esquemático do fabricante.
- Notas de campo + folhas de esquemático: `ultramcu/guition-jc4880p443c-i-w`.
- Protótipo anterior (`Andre-Wa/esp32-p4da-slint-prototype`), validado em hardware.

Onde protótipo e placa real divergiam, **a placa real prevalece** (marcado [FIX]).

## Mapa de pinos (fonte da verdade: `main/board_config.h`)

| Bloco | Sinal | GPIO / canal | Estado |
|---|---|---|---|
| Display ST7701S (MIPI-DSI) | reset | 5 | OK |
| | backlight (PWM → boost MP3202) | 23 | OK |
| | DSI-PHY power | LDO on-chip ch3 @ 2500 mV | OK |
| | 2 lanes @ 500 Mbps, DPI 34 MHz, 480×800 nativo (retrato) | — | OK |
| Touch GT911 (I²C) | SDA / SCL (compartilhado c/ codec) | 7 / 8 | OK |
| | reset | 3 | [FIX] protótipo dizia 22; driver segue NC (POR funciona) |
| | INT | 21 | [HW?] não usado; candidato a wake do standby |
| | endereço | 0x5D (probe 0x14) | [FIX] primário |
| MicroSD (SDMMC) | CLK / CMD | 43 / 44 | OK |
| | D0 / D1 / D2 / D3 | 39 / 40 / **41 / 42** | [FIX] protótipo: 46 / 45 (**45 é NC**, R10 não populado) |
| | TF_VCC | LDO on-chip ch4 @ 3300 mV | OK |
| Wi-Fi/BT (C6, ESP-Hosted SDIO) | CLK / CMD | 18 / 19 | NEW |
| | D0–D3 | 14 / 15 / 16 / 17 | NEW |
| | reset do C6 | 54 (ativo-alto) | NEW |
| Áudio ES8311 (I²S) | MCLK / BCLK / LRCK | 13 / 12 / 10 | NEW |
| | DOUT (P4→codec) / DIN | 9 / 48 | NEW |
| | amp NS4150 PA_EN | 11 | NEW |
| Botão BOOT | — | 35 | NEW (wake do light sleep) |
| LED / RS-485 TX | — | 26 | NEW |
| UART0 console | TX / RX | 37 / 38 | OK |
| USB | HS = OTG (teclado) · FS = Serial-JTAG (flash/log) | — | OK |
| Bateria | IP5306 (sem telemetria!) | CN4 MX1.25 | [HW?] ver abaixo |

## Achados que mudam decisões de projeto

1. **SD a 100× mais velocidade disponível.** O protótipo montava o cartão em
   1-bit @ 400 kHz (`SDMMC_FREQ_PROBING`, `width=1`) — "funcionava" porque o
   modo 1-bit nunca toca D1..D3 (que estavam declarados errados). Com 4-bit @
   40 MHz e pinos corretos, o mesmo cartão salta de dezenas de KB/s para
   dezenas de MB/s. Pré-requisito p/ gerenciador de arquivos e player.
2. **Deep sleep não acorda por botão nem por toque nesta placa.** No ESP32-P4,
   só **GPIO0–15** são LP/RTC GPIOs (wake de deep sleep). O botão BOOT (35) e o
   INT do touch (21) estão fora → deep sleep = "desligar" (volta no power-on).
   Light sleep aceita wake por qualquer GPIO → é o degrau de standby usável.
   Detalhes e consequências: `docs/POWER.md`.
3. **IP5306 não tem telemetria.** Sem I²C, sem divisor de VOUT-BAT para ADC do
   P4 na folha 02 do esquemático. Nível de bateria = não medível por software
   hoje; status bar mostra `--`. Opções futuras: confirmar folha 05, ou mod de
   hardware (divisor p/ um ADC livre), ou aceitar só os LEDs do IP5306.
4. **Wi-Fi exige reflash do C6.** O slave ESP-Hosted de fábrica (2.3.0) não casa
   com host ~2.12 (`Version mismatch`, drops de SDIO/`ASSOC_LEAVE`). Procedimento
   em `tools/flash_c6_wifi.sh` (UART direto no header JP1, **com o P4 haltado**).
5. **Chip pode ser engineering sample (rev v1.3).** Toolchains novos recusam P4
   rev < 3.1. O protótipo compila e roda em **ESP-IDF v5.5.1** → mantemos IDF
   5.5.x pinado (`idf_component.yml` limita `idf: >=5.3,<6.0`). Não subir para
   IDF 6.x sem testar; se aparecer "Illegal instruction" no bootloader, é isso.
6. **Backlight**: BSP comunitário trata GPIO23 como GPIO ativo-alto simples;
   aqui usamos LEDC PWM (funciona e dá dimerização p/ standby). Se alguma
   unidade não dimerizar, o piso de 10% em `board_display_backlight_set()`
   vira on/off — aceitável.
7. **Escritas em flash/NVS com DPI vivo** podem causar underrun (flash branco).
   Mitigação do protótipo mantida: `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`. Nossas
   escritas persistentes vão quase todas para o **SD**, não para flash — o que
   reduz ainda mais a janela do problema.

## Bateria / conector CN4 (polaridade!)

MX1.25 2 pinos, trava para cima, furos para você: **esquerda = BAT− (preto)**,
**direita = BAT+ (vermelho)**. Inverter alimenta o IP5306 ao contrário.

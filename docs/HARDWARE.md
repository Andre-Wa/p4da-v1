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
| Bateria | IP5306 (carga, sem telemetria) + divisor p/ ADC | CN4 MX1.25 + GPIO53 | [NEW-BAT.0] ver abaixo |

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
3. **O IP5306 não tem telemetria — mas a placa TEM divisor de bateria.**
   O IP5306 padrão não implementa I²C (só a variante customizada
   "IP5306_I2C", nos pinos L1/L2/L3, aqui NC), e mesmo essa variante NÃO
   expõe tensão/corrente: só flags de carga/cheia/carga-leve/tecla
   (register file V1.21: reg 0x70/0x71/0x72/0x77). A folha 03 do
   esquemático ("Expand IO"), porém, traz um divisor do rail da bateria:
   BAT+ → R52 68K → nó → R57 100K → GND (C61 10nF no nó), nó → R55 0R →
   **GPIO53 = ADC2_CH4 do P4** (Wi-Fi/BT rodam no C6, então o ADC2 do P4
   está livre). Desde a v5.12.0 (BAT.0) `main/battery.c` lê esse nó e a
   status bar mostra % real. A análise antiga (v5.11.x) olhou só a folha
   02 e concluiu "não medível por software" — estava incompleta, não
   errada no IP5306: o que media é o divisor, não o charger.
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
8. **Saída de áudio EXISTE na placa** (pesquisa de 2026-09-30,
   `docs/BLUETOOTH.md`): codec ES8311 + amplificador NS4150 (PA_EN=GPIO11)
   alimentam um **conector de alto-falante** (a família JC4880P4xx tem
   conector de falante e de mic — conferir o passo na variante antes de
   comprar o falante). Fone Bluetooth é inviável (C6 é BLE-only; A2DP não
   existe e LE Audio não tem suporte no IDF p/ C6); alternativa sem solda =
   fone/DAC **USB-C** via `usb_host_uac` na porta OTG (divide com o teclado
   HID). BLE p/ sync com telefone, por outro lado, é plenamente viável
   (NimBLE + HCI sobre o SDIO do ESP-Hosted, sem reflash do C6).
9. **O "veneno INT_MAX" do system.lua era ABI do Lua, NÃO o cartão SD**
   (veredito de 2026-09-30, três fontes de evidência). O componente
   `espressif/lua` 5.5.0 compila a VM com `LUA_32BITS=1` (`lua_Number` =
   `float`, `lua_Integer` = `int32`), mas o define é **PRIVATE** no
   CMakeLists dele (`idf-extra-components/lua/CMakeLists.txt`; bug público:
   espressif/developer-portal discussion #188). O `main/`, sem o define,
   compilava com `double`/`int64` dos headers:
   - `lua_tonumber` devolve **float nos 32 bits baixos de `fa0`**; o caller
     lia `double` (64 bits) → lixo finito → `(int)` no RISC-V satura em
     **2147483647** (`fcvt.w.d` satura, não é UB na prática) → TODO campo
     numérico do config virava INT_MAX com `parse=ok`. O self-test do
     M4.11 pegou exatamente isso no boot seguinte ("FALHOU").
   - `lua_tointeger` "funcionava" por sorte: o psABI RISC-V garante retorno
     `int32` sign-extended em `a0`, e o caller lia `int64` de `a0`.
   - Strings/bools (`wifi.lua`, `save_persists`, censo, dump) nunca foram
     afetados — por isso o log parecia esquizofrênico.
   - `pda.settings.get()` de scripts Lua devolvia lixo numérico pelo mesmo
     motivo (`lua_pushnumber` com double lido como float pela VM).
   **Cura**: `target_compile_definitions(main PRIVATE LUA_32BITS=1)` no
   `main/CMakeLists.txt` + `_Static_assert(sizeof(lua_Number)==4)` no
   `pda_config.c` (o build falha com mensagem clara se o define sumir).
   **Cartão exonerado**: o cartão de 8 GB passou no teste de mídia instável
   (duas leituras, FNV-1a idêntico, vários boots) e os dumps sempre
   mostraram conteúdo íntegro. Os cartões de 4 GB/16 GB "corrompidos" são
   problema separado (testar com `fsck.fat`/`f3` no PC). Nota: a partir de
   2026-01-09 o master do idf-extra-components ganhou um `port/include/
   luaconf.h` público que define `LUA_32BITS` para todos os consumidores —
   quando o registro publicar versão com esse port, o nosso define vira
   redundância inofensiva (idempotente).
10. **USB-A host NÃO tem VBUS na bateria — é projeto, não defeito**
    (achado de 2026-10-12, folhas 02+03). O VBUS das duas portas USB-A
    ("Full Speed USB" USB1 e "High speed USB" USB3) é a rede
    **USB5V_IN**, que só existe vinda da entrada externa: VIN (MX1.25 4P)
    → Q4 (AO3401, chave P-MOS) → USB5V_IN. O boost do IP5306
    (**VOUT-BAT**, que alimenta o buck 3V3, o backlight e o NS4150) NÃO
    toca o USB5V_IN em lugar nenhum do esquemático. Consequências:
    - Na bateria, periféricos USB (teclado, dongle, headset com fio via
      hub) ficam SEM energia → "USB não funciona na bateria" relatado em
      campo é esperado; com carregador/PC ligado, funciona.
    - O header de expansão JP1 pin 2 = **VCC5V** (= VOUT-BAT com fusível
      de 1 A) VIVE na bateria: é a fonte certa p/ alimentar periférico
      externo sem mexer no firmware (ver docs/USB.md §"Teclado RP2040-Zero
      sem VBUS no USB-A").
    - O falante funciona na bateria porque o NS4150 é alimentado por
      VOUT-BAT (folha 02, bloco SPEAKER: pin 7 VCC ← VOUT-BAT) — provado
      em campo na v5.11.4/v5.12.0 sem USB plugado.
    - Cuidado p/ futuras ideias: alimentar algo pesado do VCC5V na
      bateria esbarra no auto-desligue do IP5306 por carga leve
      (docs/POWER.md fontes [9]-[11]) — o sistema inteiro já passa do
      limiar, mas um periférico que puxe picos pode oscilar o rail.

## Bateria / conector CN4 (polaridade!)

MX1.25 2 pinos, trava para cima, furos para você: **esquerda = BAT− (preto)**,
**direita = BAT+ (vermelho)**. Inverter alimenta o IP5306 ao contrário.

## Nível de bateria: divisor no GPIO53 (BAT.0, v5.12.0)

Folha 03 do esquemático ("Expand IO"):

```
BAT+ --R52 68K--+--R57 100K--GND     C61 10nF no nó (anti-ruído)
                |
                +--R55 0R-- GPIO53   (ADC2_CHANNEL_4, atten 12 dB)
```

`main/battery.c` (task "battery", poll 4 s, 8 amostras, média, EMA 1/4):
`V_bat = V_nó × (68+100)/100 = V_nó × 1.68`; % por curva LiPo típica sem
carga (piecewise-linear em `s_curve`, calibrar em BAT.1). Calibração eFuse
line-fitting quando o chip tiver; sem eFuse, fallback linear ~3.1 V FS @
12 dB (mesmo padrão do exemplo oficial `oneshot_read` do IDF).

Comportamentos esperados (não são bugs):
- **Em carga** a leitura reflete a tensão de carga (lê alto, ~4.2 V no
  topo): o divisor está do lado da bateria, e o IP5306 eleva a bateria
  durante CC/CV. Desde a v5.13.0 (BAT.2) uma máquina de estados detecta
  carga/saída por salto de tensão e CONGELA o % no pós-carga até a tensão
  assentar — estados "bateria|carregando|assentando" no log e no
  Config→Sistema; detalhes e limiares em docs/POWER.md §BAT.2.
- **Sob carga pesada** (tela + falante + Wi-Fi) a tensão cede pela
  resistência interna da célula: o % lê pessimista e recupera em repouso.
- O divisor consome ~25 µA permanentes (4.2 V / 168 K) — irrelevante.
- Precisão de 1ª rodada: resistores 1% + ADC sem calibração eFuse podem
  somar alguns %; o log periódico (`battery: NN% | mV | nó mV | raw`) dá
  os números para calibrar contra multímetro (docs/POWER.md, BAT.1).

O que o IP5306 ainda é o único que sabe: estado de carga/cheia/carga-leve
e a tecla BF2 — e só na variante custom com I²C (aqui NC). Para o PDA, o
divisor cobre o que importa (nível); o corte por carga leve continua
descrito em docs/POWER.md.

## DPI + DMA2D: por que `use_dma2d = false`

Com `use_dma2d = true` o `esp_lcd_panel_draw_bitmap()` do painel DPI é
assíncrono; o platform ESP do Slint re-chama antes do fim da transferência
anterior e o driver responde `ESP_ERR_INVALID_STATE`
("previous draw operation is not finished"), num loop de retries que
rasga a tela e mata o touch. Síncrono (CPU) é determinístico e sobra CPU
no P4. Medido no boot de 2026-09-22: sem DMA2D o bring-up completa e o
loop do Slint roda limpo.

## A tecla ao lado do conector de bateria = KEY do IP5306 (BF2)

Folha 02 do esquemático: o tact switch **BF2** liga o pino **KEY (5)** do
IP5306 ao GND através de **R48 10K**. **Não chega em nenhum GPIO do
ESP32-P4** — por isso não existe `#define` dela no `board_config.h` e o
firmware não a lê hoje. (Os pinos LED1-3 do IP5306 aparecem NC nesta
variante, então a consulta de nível pode não ter feedback visível.)

Comportamento (típico do IP5306, um SoC de power bank):

| Situação | Ação da tecla |
|---|---|
| Saída cortada, só bateria ("placa off") | toque curto liga VOUT → boot frio |
| Rodando na bateria | toque curto = consulta nível (LEDs, se populados) |
| Rodando na bateria | **segurar ~2-3 s = corta VOUT = power-off total** |
| USB conectado | carga automática; tecla só consulta/força saída |

Consequência de projeto: o IP5306 também tem **auto-desligue** de power
bank (carga abaixo do limiar sem USB). Nosso HIBERNATE (deep sleep,
consumo mínimo) na bateria pode ser cortado por ele — e isso é *feature*:
vira o "desligar" do PDA, e o boot frio seguinte restaura a sessão de
`<raiz>/.state/session.txt`. Em STANDBY (light sleep, dezenas de mA) a
carga segura o IP5306 acordado e o wake continua sendo BOOT/GT911.

**Mod opcional p/ torná-la botão de software:** 1 fio do nó do switch p/
um **LP GPIO livre (0-15; ex. GPIO4/GPIO6 no header de 26 pinos)** —
precisa ser LP para servir de wake de deep sleep (ver docs/POWER.md).
Pull-up interno + ISR; atrás de um `#define BOARD_PWR_KEY_GPIO` para não
afetar placas sem o mod. Não feito em M1 de propósito.

*Não confundir*: o botão **BOOT (GPIO35, strapping)** é outro componente,
em outro ponto da placa, e esse o firmware lê (wake do light sleep).

## SD (slot 0) × SDIO do C6 (slot 1) — NÃO trocar

No ESP32-P4 o periférico SDMMC tem dois slots: **slot 0 = IOMUX fixo do
microSD** (GPIO 43/44/39-42 desta placa) e **slot 1 = GPIO-matrix, usado
pelo ESP-Hosted/SDIO do C6** (18/19/14-17). `SDMMC_SLOT_CONFIG_DEFAULT()`
cai no slot 1: montar o cartão lá fazia hosted e cartão brigarem pelo
periférico (`sdmmc_card_init failed` no hosted, `0x107` no cartão e
leituras corrompidas de `system.lua` disparando a "cura" a cada boot).
`storage_init.c` fixa `host.slot = 0`. Se um dia o SD ou o Wi-Fi
"morrerem" juntos, suspeite disso primeiro.

## GT911: NACK no primeiro I2C do warm boot (M5.1.1, 2026-10-05)

Após RESET quente, o GT911 às vezes não responde ao primeiro
`esp_lcd_touch_new_i2c_gt911` (NACK; `TouchPad_ID 0x00,0x00,0x00`), e o
`ESP_ERROR_CHECK` do boot abortava (crash-report de 2026-10-05). Sem
pino de reset controlado (RST/INT em NC de propósito, ver achado #2),
a cura é retry: 5 tentativas, 80 ms entre, `i2c_master_bus_reset` da
3ª em diante (`board_touch_init`). Se um boot logar
`GT911 tentativa N/5`, o retry pagou seu salário.

# PDA ESP32-P4 — sistema (M1)

Sistema operacional de bolso para a **GUITION JC4880P443C_I_W**
(ESP32-P4 + ESP32-C6, tela 4.3" 480×800 MIPI-DSI, touch GT911, SD, ES8311,
bateria via IP5306), em C++/Slint + Lua 5.5.

Evolução "quase do zero" do protótipo
[`Andre-Wa/esp32-p4da-slint-prototype`](https://github.com/Andre-Wa/esp32-p4da-slint-prototype):
o que estava provado foi mantido (display, touch, USB-HID, notas), o que estava
errado foi corrigido (pinos/velocidade do SD), e o que não existia foi construído
(config em Lua, scripts Lua, arquivos navegáveis, energia em degraus, status bar).

## O que já funciona (M1)

- Tela/touch/backlight com rotação paisagem (Slint, software renderer)
- **Cartão SD como armazenamento principal** (SDMMC 4-bit @ 40 MHz) com fallback
  interno transparente; árvore `/pda/{config,scripts,notes,music,logs,.state}`
- **Configurações em Lua** (`config/system.lua`) editáveis na tela ou no cartão
- **Scripts Lua** (`scripts/*.lua`) com console na tela e API `pda.*` protegida
- Gerenciador de arquivos navegável + editor de notas (teclado USB)
- **Energia em degraus**: dimeriza → standby (light sleep, acorda por botão)
  → hibernate (deep sleep com sessão restaurada do SD)
- Teclado USB HID (boot protocol) com mapa US + shift

## Leia antes de flashear

| Doc | Conteúdo |
|---|---|
| `docs/HARDWARE.md` | pinagem reconciliada, achados críticos (SD, LP GPIOs, IP5306, C6) |
| `docs/POWER.md` | por que standby = light sleep e hibernate = deep sleep; plano de medição |
| `docs/ARCHITECTURE.md` | camadas, threading, modelo de armazenamento |
| `docs/LUA.md` | formato da config + referência da API `pda.*` |
| `docs/ROADMAP.md` | M1→M6 com critérios de aceite |

## Build

Requer **ESP-IDF v5.5.x** no PATH (pinado de propósito — ver `docs/HARDWARE.md` #5).

```bash
cd p4da
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # porta USB-Serial-JTAG (porta "Full Speed")
```

Primeiro build baixa componentes (Slint, Lua, etc.) via Component Manager.

## Cartão SD

Copie o template uma vez (depois o próprio firmware semeia o que faltar):

```bash
cp -r sdcard-template/pda /mnt/seu-cartao/
```

Sem cartão, tudo funciona igual em `/internal/pda` (8 MB) — e as configurações
são promovidas automaticamente ao cartão no primeiro boot com ele presente.

## Wi-Fi (fase M4 — preparar o terreno)

O C6 vem de fábrica com ESP-Hosted slave incompatível com host atual.
Procedimento e avisos em `tools/flash_c6_wifi.sh`. **Não é necessário p/ M1.**

## Estrutura

```
p4da/
├── CMakeLists.txt · partitions.csv · sdkconfig.defaults · .clangd
├── docs/                      # HARDWARE · POWER · ARCHITECTURE · LUA · ROADMAP
├── sdcard-template/pda/       # config/system.lua + scripts de exemplo
├── tools/flash_c6_wifi.sh     # reflash do coprocessador Wi-Fi (M4)
└── main/
    ├── board_config.h         # TODA a pinagem, reconciliada c/ a placa real
    ├── display_init.[ch]      # ST7701S MIPI-DSI + backlight PWM c/ dimerização
    ├── touch_init.[ch]        # GT911 I2C
    ├── usb_hid_keyboard.[ch]  # teclado USB (boot protocol)
    ├── storage_init.[ch]      # SD 4-bit + LittleFS + raiz /pda + espelho
    ├── pda_config.[ch]        # settings em Lua (fonte da verdade em C)
    ├── lua_runtime.[ch]       # VM Lua 5.5 protegida + API pda.*
    ├── power_mgmt.[ch]        # degraus DIM/STANDBY/HIBERNATE
    ├── main.cpp               # orquestração (threads + event loop)
    └── ui/app_ui.slint        # status bar + 6 telas
```

## Notas de build (IDF 5.5.1, testado)

- **Componente `esp_timer`** é obrigatório no `REQUIRES` (usamos
  `esp_timer_get_time()` p/ idle/uptime) — no IDF 5.5 ele não vem mais
  embutido em `esp_system`.
- **Slint resolve para 1.18.x** (prebuilt `Slint-cpp-1.18.1-riscv32imafc-esp-espidf`),
  não 1.12: a API de `slint-esp.h` que usamos é compatível e agora setamos
  `panel_type = MipiDsiDpi` explicitamente (o P4 tem RGB e DSI; `Auto` já
  escolheria DSI, mas explícito evita surpresa).
- `-Werror=all` do IDF pega pegadinhas que evitamos de propósito:
  `/*` dentro de comentário (`-Wcomment`), truncamento de `snprintf`
  (`-Wformat-truncation`) → usamos `strlcpy` nas cópias de string.
- Se o build reclamar de componente velho: `rm -rf build managed_components
  dependencies.lock sdkconfig` e reconstrua.

### Gotcha do Slint 1.18: glifos de fallback incham a flash
O slint-compiler embute os glifos dos caracteres que aparecem no `.slint`.
Caracteres fora da fonte default (emoji `📁`, símbolos `↻ ↑ ▏ —`) disparam
**font fallback** com glifos de centenas de KiB até MiB cada — vimos
`.rodata` de 33,7 MiB e `elf2image` estourar o limite de 16 MB.
**Regra: UI em ASCII + Latin-1 (acentos PT-BR ok).** Sem emoji no `.slint`.
No Slint **1.18.1** isso não foi suficiente (glifos inflados mesmo p/ ASCII,
31,8 MiB de `.rodata`) — por isso o pin em **1.12.1**, ver
`docs/DEPENDENCIAS.md`. Se um dia subirmos o Slint, meça `idf.py size` antes.
- **Pilhas**: task `main` = 32 KiB (o event loop + layout do Slint 1.12 com
  nossas telas aninhadas estourava os 8 KiB originais com
  `Stack protection fault` em `NotesListScreen::item_geometry`);
  threads de I/O = 8 KiB e a thread do Lua = 32 KiB via `spawn_thread()`
  (o default de ~3 KiB do IDF não segura a VM Lua).
- **`sdkconfig.defaults` NÃO aceita comentário no fim da linha de atribuição**
  (`FOO=1  # bla` invalida o valor e o símbolo cai no default, em silêncio).
  Comentários só em linha própria. Já causou stack de 3584 B no lugar de
  32768 B e `Stack protection fault` no boot.

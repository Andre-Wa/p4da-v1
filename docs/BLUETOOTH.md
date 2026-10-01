# Bluetooth no PDA (pesquisa M4.11 — 2026-09-30)

Pergunta de partida: a placa (GUITION JC4880P443C_I_W, ESP32-P4 + ESP32-C6)
consegue (a) falar com um telefone/computador como um relógio fala com o app
do celular (ping, hora, clima, sincronização) e (b) tocar música num fone de
ouvido sem fio?

## TL;DR

| Caso de uso | Viável? | Como |
|---|---|---|
| BLE com telefone/app (sync, hora, clima, ping, provisão Wi-Fi) | **SIM** | C6 via ESP-Hosted (HCI sobre SDIO) + NimBLE/Bluedroid no P4 |
| Bluetooth Classic (A2DP — fone de ouvido comum) | **NÃO** | C6 é BLE-only; não existe rádio Classic no hardware |
| LE Audio (fones modernos, LC3) | **NÃO (hoje)** | ESP-IDF não tem LE Audio p/ C6 (issue aberta desde 2023); só o novo ESP32-S31 tem ESP-BLE-AUDIO |
| Música com fio | **SIM** | (a) alto-falante no conector da placa (ES8311 + NS4150); (b) fone/DAC USB-C via `usb_host_uac` na porta OTG |

## 1. O hardware

- O **ESP32-P4 não tem rádio** (nem Wi-Fi nem BT). Toda conectividade vem do
  **ESP32-C6** coprocessador, ligado por SDIO (ESP-Hosted).
- O **ESP32-C6** é Wi-Fi 6 + **Bluetooth 5 (LE) + 802.15.4** (Zigbee/Thread).
  **Não tem Bluetooth Classic** — é BLE-only por hardware (confirmado no
  datasheet; fórum Espressif: "the C6 is a BLE-only chip meaning it doesn't
  support BT Classic").
- O boot do PDA já mostra o slave anunciando BT: `* WLAN`, `- HCI over SDIO`,
  `- BLE only`, e o host registra `hci_stub_drv: Host BT Support: Disabled`
  (desabilitado só porque `CONFIG_BT_ENABLED` está desligado no nosso
  sdkconfig — o caminho existe e está pronto).
- Antena: o módulo JC-ESP32P4-M3-C6 tem conector IPEX **e** a placa tem
  antena cerâmica (a família JC4880P4xx é documentada assim). O Wi-Fi já
  funciona bem no aparelho → o caminho de RF está ok. (Lição da issue
  esp-hosted-mcu#180: um "bug de scan BLE" num Waveshare P4 era só o módulo
  C6-MINI-**1U** sem antena conectada — resolvida a antena, o scan BLE via
  SDIO ficou "rock solid", validado por colaborador da Espressif no
  P4-Function-EV com o mesmo esquema host/slave 2.12.x.)

## 2. BLE P4 ↔ C6: como se liga

Receita validada na issue esp-hosted-mcu#180 (host 2.12.x, IDF 5.5.x,
Bluedroid; NimBLE idem) — sdkconfig do host P4:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y          # ou CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CONTROLLER_DISABLED=y     # P4 não tem controller local
CONFIG_BT_BLE_ENABLED=y
CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y   # (variante Bluedroid: ..._BT_BLUEDROID)
```

- O slave que já flasheamos (`network_adapter` 2.12.9, ver
  `tools/flash_c6_wifi.sh`) **já inclui HCI de BT** — não precisa reflashar
  o C6 para M4c (o log de capabilities confirma).
- Stack recomendada: **NimBLE** (menor RAM/flash que Bluedroid; PDA é
  peripheral/GATT server, não precisa dos perfis Classic do Bluedroid).
- Custo estimado no binário: ~200–400 KB (temos folga: factory 6 MB, app
  ~4,1 MB) + RAM do NimBLE (~30–50 KB). Wi-Fi e BLE **coexistem** no mesmo
  rádio C6 (coex já ativo no slave) — com algum custo de throughput dos dois
  lados, aceitável p/ sync esporádica.

## 3. O que dá p/ fazer com BLE (escopo M4c)

Modelo "smartwatch": o PDA anuncia um serviço GATT e o telefone (app
companheiro) conecta:

- **Sync de dados**: notas/arquivos pequenos em chunks via characteristic
  (MTU negociado ~247–512 B; ~10–50 kB/s reais em coex com Wi-Fi — ok p/
  notas/config, lento p/ binários grandes; p/ arquivos grandes, melhor
  Wi-Fi direto: o PDA já serve/consome rede).
- **Hora/clima sem NTP**: app do celular escreve epoch + previsão num
  characteristic (útil quando não há Wi-Fi).
- **Ping/localizar**: RSSI + beep de UI (M5) nos dois sentidos.
- **Provisão Wi-Fi**: digitar SSID/senha pelo app em vez do OSK.
- **Notificações** (estilo watch): ANCS-like não existe no Android; no
  Android o app parceiro lê `NotificationListenerService` e repassa — exige
  app companheiro próprio (fora do escopo do firmware, mas o GATT suporta).

Fora do escopo (sem Classic): mãos-livre HFP, áudio A2DP, SPP serial.

> **Design completo do app companheiro** (canal BLE de presença + canal
> Wi-Fi/REST de volume, algoritmo de sync, fases F0–F4, orçamento de
> RAM/flash): `docs/ANDROID_SYNC.md` (proposta de 2026-09-30).

## 4. Áudio sem Bluetooth (resposta ao "fone sem fio")

Fones BT comuns usam **A2DP (Classic)** → impossível no C6. Fones BT novos
com **LE Audio (LC3)** teoricamente caberiam no BLE 5.3 do C6, mas o ESP-IDF
**não implementa LE Audio p/ C6** (issue espressif/esp-idf#12277 aberta desde
2023; a API ESP-BLE-AUDIO existe só p/ ESP32-S31, chip que não está nesta
placa). Implementar um sink unicast LE Audio do zero seria um projeto
enorme — não vale.

Caminhos reais p/ áudio (M5):

1. **Alto-falante na própria placa (recomendado).** O esquemático da placa
   (repo `ultramcu/guition-jc4880p443c-i-w`) traz **codec ES8311 (I²S) +
   amplificador NS4150** já mapeados em `main/board_config.h`
   (MCLK/BCLK/LRCK = 13/12/10, DOUT=9, DIN=48, PA_EN=11), e a família
   JC4880P4xx tem **conector de alto-falante** (e de microfone) na placa —
   confirmado p/ o JC4880P433 (CNX-Software). Falta só **comprar um
   alto-falante pequeno** (tipicamente 4 Ω/3 W com plugue MX1.25/JST —
   conferir o passo do conector na sua variante antes de comprar). O M5 já
   planeja isso: beep de UI primeiro (prova do caminho I2S/PA_EN), depois
   player do SD. O ES8311 também tem entrada (DIN=48) p/ o conector de mic.
2. **Fone/DAC USB-C.** O P4 tem USB HS host (a mesma porta OTG do teclado
   HID). O componente `espressif/usb_host_uac` (esp-iot-solution) toca áudio
   em dispositivos UAC (headsets USB-C e DACs baratos). Custo: dividir a
   porta com o teclado HID (hub USB ou troca a quente). Fica como opção
   secundária/alternativa p/ M5.
3. **Mod P3 (não recomendado).** Derivar a saída do ES8311 p/ um jack 3,5 mm
   exige solda/mod de hardware; o caminho do amplificador já vai p/ o
   conector de alto-falante.

## 5. Decisões registradas

- **M4c (BLE) segue viável e útil** — escopo: NimBLE peripheral + serviço
  GATT "pda" (hora/clima/sync/ping/provisão), coex com Wi-Fi. Sem reflash do
  C6. Adiar até depois dos gestos (prioridade do usuário, 2026-09-30).
- **Áudio de fone sem fio: descartado** por hardware/stack (sem A2DP, sem LE
  Audio). M5: usuário não tem (ainda) o conector JST do alto-falante da
  placa → **M5a passa a ser USB-C UAC** (fone/DAC USB p/ testes); conector
  do falante quando chegar (dica: cabo extensor de bateria JST-PH como
  doador do plugue); adaptador P2 nos pinos de expansão só após conferir o
  esquemático (ver `docs/ROADMAP.md` M5).
- 802.15.4 (Zigbee/Thread) existe no silício do C6, mas o slave ESP-Hosted
  que usamos não o expõe — ignorado.

## Fontes

- Datasheet/fórum: ESP32-C6 BLE-only (sem Classic):
  https://esp32.com/viewtopic.php?t=39603
- esp-hosted-mcu (2.x/3.x): arquitetura RPC + HCI sobre SDIO; exemplos BT
  (NimBLE/Bluedroid): https://github.com/espressif/esp-hosted-mcu
- Issue #180 (P4+C6 BLE scan): sdkconfig completo + resolução "antena
  ausente", BLE estável após fix de hardware:
  https://github.com/espressif/esp-hosted-mcu/issues/180
- LE Audio no C6 não suportado (aberta desde 2023):
  https://github.com/espressif/esp-idf/issues/12277
- ESP-BLE-AUDIO só no ESP32-S31:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s31/api-reference/bluetooth/esp-ble-audio.html
- Placa (família JC4880P433): módulo JC-ESP32P4-M3-C6 (Wi-Fi 6 + BT5 +
  802.15.4), conectores de alto-falante e microfone, antena cerâmica + IPEX:
  https://www.cnx-software.com/2025-08-12/4-3-inch-touch-display-board-features-single-esp32-p4-esp32-c6-module-supports-camera-and-speakers/
- USB host áudio (UAC): https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_host_solutions.html

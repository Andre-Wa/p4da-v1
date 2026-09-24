# Wi-Fi / NTP (M4) — P4 host + C6 coprocessor

## Topologia
O P4 não tem rádio: o Wi-Fi/BT vive no **ESP32-C6** a bordo, falando
**ESP-Hosted 2.12.x sobre SDIO** (CLK18/CMD19/D0-3=14-17, reset GPIO54).
No host, `espressif/esp_wifi_remote` (+ `esp_hosted` 2.12.9 pinado) expõe a
API `esp_wifi` usual — `main/wifi_net.c` é código Wi-Fi "normal".

## Pré-requisito: firmware do C6
O slave que vem de fábrica (2.3.0) não casa com host 2.12.x
(`Version mismatch`, drops de SDIO). Flashear **network_adapter 2.12.9**:

    ./tools/flash_c6_wifi.sh        # UART no header JP1, P4 HALTADO antes

## Configuração
`<raiz>/config/wifi.lua` (template em `sdcard-template/`):
`ssid`, `password` (texto puro — trate o cartão como mídia sensível),
`auto_connect`. Sem o arquivo o Wi-Fi nem sobe (`wifi off` na status bar).

## Comportamento
- Boot: `wifi.lua` → task `wifi_net` (NVS → netif → event loop → wifi init →
  connect). `IP_EVENT_STA_GOT_IP` → status bar `wifi -NN dBm` + SNTP
  (`locale.ntp_server`), e o relógio da status bar troca uptime por **HH:MM**
  local (`locale.timezone` → TZ POSIX, tabela curta em `wifi_net.c`).
- Queda: reconexão automática em 2 s enquanto `auto_connect`.

## Limitações conhecidas
- **Standby × SDIO**: o link com o C6 não sobrevive ao light sleep
  (o exemplo oficial já avisa: "SDIO currently does not work with auto
  light sleep"). Ao acordar, o wifi reconecta sozinho em alguns segundos;
  o relógio mantém a última sincronização até novo NTP.
- Sem RTC de hardware: após hibernate/power-cycle sem Wi-Fi, o relógio
  volta a uptime até o próximo sync.
- Bluetooth (M4b): mesmo transporte, stack BT no host via hosted HCI —
  ainda não iniciado.

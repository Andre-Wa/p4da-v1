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

## Nota de API (IDF 5.5)
O `esp_sntp.h` da 5.5 é a API **legacy**: `esp_sntp_init(void)` +
`esp_sntp_setservername()` + `esp_sntp_set_time_sync_notification_cb()`.
A variante com `esp_sntp_config_t`/`ESP_SNTP_DEFAULT_CONFIG` só existe em
IDF mais novo — não usar enquanto estivermos pinados em 5.5.x.
- Callbacks implementados em `main.cpp` e chamados de código C
  (`wifi_net_on_event_ui`) PRECISAM de `extern "C"` na definição, senão
  o link falha com undefined reference (mangling C++ vs C).

## Multi-redes salvas (M5.2, 2026-10-05)

`config/wifi.lua` agora aceita (e o PDA gera) uma LISTA de redes; o formato
legado de um ssid só continua sendo lido (vira a entrada 1):

```lua
return {
  auto_connect = true,
  networks = {
    { ssid = "Casa",   password = "..." },
    { ssid = "Trabalho", password = "..." },
  },
}
```

Comportamento:
- Boot: tenta a última rede que conectou (`s_last_good`).
- Disconnect com `NO_AP_FOUND` (AP fora): marca a rede e rotaciona p/
  próxima salva presente; a escada de backoff (2/4/8/16/30 s) conta por
  rede (troca de rede zera p/ 2 s).
- Disconnect com AUTH_FAIL/handshake (senha errada): marca a rede como
  inválida neste ciclo e rotaciona; se TODAS falharem por auth,
  suspende com `todas as redes salvas falharam (auth)…` até ação em
  Redes (conectar por lá limpa a marca da rede escolhida).
- Tela Redes: rede travada JÁ SALVA conecta direto ao tocar (sem prompt
  de senha); rede nova pede senha e, ao conectar, entra na lista
  (máx. 8; o arquivo é reescrito com a lista completa).
- Standby pausa tudo como antes; o resume reconecta a rede corrente.

## Gerenciador de conexão scan-driven (M5.3, 2026-10-06)

Fluxo (inspirado no Android, pedido do usuário na validação da v5.2):
1. scan (bloqueante, na task `wifi_rcn`) → 2. lista de redes presentes →
3. melhor rede SALVA presente (RSSI) → 3.1 até TRY_BUDGET=3 tentativas
(backoff 2/4/8/16/30 s na mesma rede) → 4. conectou: fluxo normal
(NTP etc.) → 5. erro contínuo ou AUTH_FAIL×3: marca a rede e volta ao
passo 1 p/ a próxima salva presente → 6. nenhuma salva presente:
`nenhuma rede salva presente — Wi-Fi em espera até ação manual`
(sem martelo; acordam: toggle, connect manual, rescan da tela Redes,
resume do standby — que também limpa as marcas do ciclo).

AUTH_FAIL espúrio (rede BOA que vacila) não rota mais na 1ª falha: são
3 tentativas na mesma rede antes de marcar (`AUTH_FAIL (reason N) em
"X": tentativa n/3`); o caso do log de 2026-10-05 (F3 correta marcada
por um 202 isolado) não se repete.

## Chave de rádio (M5.3)

`net.wifi_enabled` no system.lua + SwitchRow "Wi-Fi ligado (radio)" em
Config → Redes: OFF = `esp_wifi_stop()` (C6 libera o rádio); ON =
`esp_wifi_start()` → STA_START → fluxo de scan. Persiste em NVS
(`wifion`). Sem wifi.lua E chave ON: comportamento antigo (offline,
scan disponível em Redes). Conectar por Redes com a chave OFF liga o
rádio automaticamente.

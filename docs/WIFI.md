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
(NTP etc.) → 5. erro contínuo ou falha de auth/handshake×3: marca a
rede e volta ao passo 1 p/ a próxima salva presente → 6. nenhuma salva
PRESENTE NO SCAN: `nenhuma das N rede(s) salva(s) presente no scan —
nova varredura em 120 s` (v5.15.0: a espera NÃO é mais permanente —
ver §Diagnóstico abaixo; a mensagem da v5.14, "nenhuma rede salva
presente", lia-se como "não há redes salvas" quando havia 2). Acordam
na hora: toggle, connect manual, rescan da tela Redes, resume do
standby (que também limpa as marcas do ciclo).

Falha de auth/handshake espúria (rede BOA que vacila) não rota mais na
1ª falha: são 3 tentativas na mesma rede antes de marcar (`falha de
auth/handshake em "X": n/3 na mesma rede`); o caso do log de
2026-10-05 (F3 correta marcada por um 202 isolado) não se repete.

## Diagnóstico de desconexões e recuperação de espera (v5.15.0)

**Incidente da 14ª rodada** (log do usuário, v5.14.0): o Wi-Fi não
conectou num boot em que sempre conectava, e "logo depois, no mesmo
boot, conectou" após re-salvar a rede. A cadeia no log:

| t (s) | evento | leitura correta |
|---|---|---|
| 9836 | conecta "Redstone" (escolhida por scan) | ok |
| 12265 | desconexão SEM log nosso | `NO_AP_FOUND` (201) — inferido pelo salto do contador (o ramo era silencioso) |
| 24997 / 62659 | "AUTH_FAIL (reason 15)" ×2 | reason 15 = `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT` (EAPOL não completou): senha errada **OU AP instável/ocupado** — o rótulo AUTH_FAIL mandava o diagnóstico p/ senha |
| 37946 | desconexão sem log | falha genérica (beacon/assoc) |
| 97609→100129 | desconexão + scan vazio p/ salvas | AP **sumiu do ar** (não estava nos resultados do scan) |
| 100129→224409 | "nenhuma rede salva presente" | espera PERMANENTE: o AP voltou minutos depois e o PDA seguiu offline até ação manual |
| 224409 | usuário re-salva (wifi.lua 237 B) | flags limpas + connect imediato → conectado |

Conclusão: AP instável por alguns minutos (handshakes perdidos +
beacons perdidos + sumiço do scan) e um manager que, ao ficar ocioso,
dormia para sempre. Nenhum dos dois é "senha errada".

Mudanças (v5.15.0):
1. **Toda** desconexão loga o reason pelo nome (`desconexão: NOME
   (reason N)`, tabela `wifi_reason_str` conferida contra
   `esp_wifi_types_generic.h` do IDF 5.5.1: 15=4WAY_HANDSHAKE_TIMEOUT,
   200=BEACON_TIMEOUT, 201=NO_AP_FOUND, 202=AUTH_FAIL, 203=ASSOC_FAIL,
   204=HANDSHAKE_TIMEOUT, 205=CONNECTION_FAIL, 210=NO_AP_W_COMPAT_…).
   O ramo NO_AP_FOUND agora loga (`AP "X" ausente no momento —
   rotacionando via scan`).
2. **Espera ociosa com re-scan**: sem rede salva no scan, nova
   varredura a cada `WIFI_IDLE_RESCAN_S`=120 s (loop de esperas de
   500 ms, então notify de ação manual acorda em ≤0,5 s). A cada
   `WIFI_IDLE_RELAX`=5 ciclos (~10 min) as marcas de falha
   (bad/noap/auth_hits) são limpas p/ nova rodada — comportamento tipo
   Android. Custo: ~0,3 s de scan a cada 2 min SÓ desconectado, com
   auto-connect e fora do standby (`s_paused` pausa tudo).
3. **Log de reconexão honesto**: acabou o `(tentativa 7/3)` que
   misturava contador global do ciclo com orçamento POR rede. Agora:
   `reconexão em N s — rede "X" (auth/handshake n/3 na rede |
   insistindo na mesma rede | escolhida por scan, RSSI -NN dBm)`.
4. **Config STA de fonte única** (`build_sta_cfg`): os três caminhos
   (boot, apply_saved, connect manual) montavam `wifi_config_t`
   separados; só o manual levava `pmf_cfg.capable=true`. No IDF 5.5 o
   campo é deprecado (PMF é negociado sempre que o AP anuncia), mas
   deriva entre caminhos é exatamente o tipo de bug que o incidente
   parecia — eliminada.

O mesmo incidente, na v5.15.0, logaria: `desconexão: NO_AP_FOUND
(reason 201)` → `desconexão: 4WAY_HANDSHAKE_TIMEOUT (reason 15)` ×2 →
`nenhuma das 2 rede(s) salva(s) presente no scan — nova varredura em
120 s` → (AP volta) `reconexão em 2 s — rede "Redstone" (escolhida por
scan, RSSI -NN dBm)` → conectado, sem toque do usuário.

## Chave de rádio (M5.3)

`net.wifi_enabled` no system.lua + SwitchRow "Wi-Fi ligado (radio)" em
Config → Redes: OFF = `esp_wifi_stop()` (C6 libera o rádio); ON =
`esp_wifi_start()` → STA_START → fluxo de scan. Persiste em NVS
(`wifion`). Sem wifi.lua E chave ON: comportamento antigo (offline,
scan disponível em Redes). Conectar por Redes com a chave OFF liga o
rádio automaticamente.

## Ruído de log aceito (não é bug)

`RPC_WRAP`/`H_API` durante ciclos de reconexão; `--- ERROR: device
reports readiness…` do USB-Serial-JTAG dormindo; `STA up (ssid=-)` antes
do primeiro apply. Reduzir verbosidade do hosted fica p/ quando o
modem-sleep do C6 entrar (POWER.md § estágios).

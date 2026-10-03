# Energia & standby — como e por quê

## As quatro opções clássicas, com números esperados

| Modo | O que fica ligado | Consumo típico estimado* | Resume |
|---|---|---|---|
| Só dimerizar/apagar backlight | CPU + UI + DSI + SD | 150–300 mA | instantâneo |
| **Light sleep** (nosso STANDBY) | RAM, PSRAM, DSI parado (DISPOFF), backlight 0 | 20–80 mA | **< 100 ms** |
| **Deep sleep** (nosso HIBERNATE) | nada (só domínio RTC) | 0,1–5 mA | reboot 1,5–3 s (sessão restaurada do SD) |
| Desligado (IP5306 corta rail) | nada | ~µA | boot frio |

\* Estimativas de projeto para validar com medição real (ver "Plano de medição").
O backlight e o painel dominam o consumo ativo; PSRAM em retenção domina o light sleep.

## Por que o deep sleep NÃO é o degrau automático padrão

No ESP32-P4, **wake de deep sleep só por LP GPIO (GPIO0–15), timer ou LP-UART**.
Nesta placa:

- botão BOOT = GPIO35 → **fora** do domínio LP;
- INT do GT911 = GPIO21 → **fora** do domínio LP;
- nenhum botão/wake de usuário em GPIO0–15 (esses pinos estão ocupados por
  I²S/SDIO/I²C/reset de painel).

Logo, deep sleep automático = aparelho "some" até tirar/recarregar a bateria.
Inaceitável como comportamento silencioso. Light sleep, por outro lado, aceita
wake por **qualquer** GPIO (gpio_wakeup) → botão e toque funcionam.

## STANDBY: dois modos (`power.light_sleep`, default **false**)

Medido em hardware (2026-09-24): no P4, **light sleep mata SDMMC, o SDIO do
ESP-Hosted e o USB DWC2**. Pior: com o Wi-Fi ativo, o host SDMMC é
*compartilhado* entre o cartão (slot 0) e o C6 (slot 1), então o
unmount→mount do wake não re-inicializa o periférico
("SDMMC host already initialized, skipping init flow") e o cartão volta
morto (0x107). Re-inicializar hosted+SD+USB a cada wake é uma coreografia
frágil demais para ser o default.

- **`light_sleep = false` (default, robusto):** STANDBY = backlight 0 +
  DISPOFF + CPU idle esperando semáforo; wake por ISR do botão BOOT, por
  INT do touch (se pulsar) ou por qualquer atividade de UI/USB. Todos os
  periféricos continuam vivos; economia vem da tela (o maior consumidor).
- **`light_sleep = true` (experimental):** o fluxo antigo (light sleep +
  remount SD + reinstall USB no wake), para medição de consumo. Checkbox
  em Config, chave `power.light_sleep` no system.lua.
- HIBERNATE continua sendo o modo de máxima economia (e o "desligar" do
  PDA), alcançável também por `deep_sleep_after_s` a partir dos dois modos.

## Arquitetura em degraus (implementada em `power_mgmt.c`)

```
ACTIVE ──idle>=dim_after_s──▶ DIM (backlight 12%)
  ▲                            │
  │ qualquer atividade         │ idle>=screen_off_after_s
  └────────────────────────────▼
                          STANDBY: backlight 0 + DISPOFF + light sleep
                          wake: GPIO35 (botão) [+ GPIO21 se wake_on_touch]
                          │ timer (se deep_sleep_after_s>0) e segue ocioso
                          ▼
                          HIBERNATE: salva sessão → unmount SD → deep sleep
                          volta apenas no power-on/reset, sessão restaurada
```

- `power_mgmt_activity()` é chamado por: teclas USB, callbacks de UI e (opcional)
  toque. Em STANDBY a task fica bloqueada em `esp_light_sleep_start()` (modo
  light sleep) ou no semáforo de wake (modo idle robusto).
- **Pedido manual de standby é STICKY (M4.13).** `power_mgmt_request_standby()`
  (botão "Suspender" do painel) seta `s_manual_standby` + zera o idle; o
  `power_task` escurece no próximo tick e entra em standby no seguinte
  (≤0.5 s, sem esperar `dim_after_s`/`screen_off_after_s`). O flag NÃO é
  cancelado por toque: antes ele só zerava o relógio de idle e a CAUDA de
  eventos de toque do próprio tap no botão (GT911 reporta por dezenas de
  ms após o release) religava o relógio — o pedido evaporava em silêncio
  (log mostrava só "standby solicitado manualmente"). Flag consumido ao
  acordar (`enter_standby`). Para desistir: acordar de novo.
- Hibernar também é **manual** (tela Config → "Hibernar"), que é o "desligar"
  do PDA: sessão (tela + nota aberta) gravada em `<raiz>/.state/session.txt`.
- Antes do deep sleep: `storage_shutdown_sd()` (unmount FatFS + LDO ch4 off)
  para não corromper o cartão nem gastar o rail dele dormindo.

## Decisões configuráveis (Lua: `config/system.lua`)

| Chave | Default | Significado |
|---|---|---|
| `power.dim_after_s` | 30 | ocioso → dimeriza |
| `power.screen_off_after_s` | 120 | ocioso → STANDBY |
| `power.deep_sleep_after_s` | 0 | em STANDBY, hiberna após N s (0 = nunca) |
| `power.wake_on_touch` | false | [HW?] GPIO21 como wake; ligue após validar o pino |

## M4.14 — rework de energia (aberto, fila pós-M4.13; feedback de 2026-10-02)

Observações do hardware que viram tarefa aqui:

1. **`power.wake_on_touch` não gateia o wake por toque de verdade.** O ISR
   do INT (GPIO21) é instalado sempre em `power_mgmt_init()` e dá o
   semáforo de wake em STANDBY; a chave hoje só entra no polling de idle
   do `power_task`. Tarefa: com a chave OFF, desarmar o ISR/ignorá-lo no
   wake (acidentes no bolso); com ON, comportamento atual. A SwitchRow já
   existe em settings.slint ("Toque segura o idle / acorda").
2. **STANDBY ainda "faz coisas".** No modo idle robusto a CPU segue viva:
   Wi-Fi reconecta (log mostra `desconectado — reconectando em 2 s` DENTRO
   do standby) e NTP atualiza. Tarefa: pausar reconexão automática e o
   timer de NTP ao entrar, retomar no wake (`wifi_net_pause/resume`);
   avaliar `host-power-save` do esp_hosted para modem-sleep do C6.
3. **Botão BOOT como wake explícito.** O ISR de GPIO35 já é instalado no
   init e dá o semáforo — validar na placa (e conferir se alguma
   reconfiguração de GPIO posterior não mata o `intr_type`).

Não são regressões do M4.13: são limites conhecidos do modo robusto
(documentados desde M-power), agora com dono e fila.

## Plano de medição (próximo passo de hardware)

1. Amperímetro em série no rail da bateria (ou no cabo do CN4).
2. Registrar: ACTIVE (brilho 100/50/10), DIM, STANDBY, HIBERNATE.
3. Se STANDBY ficar > ~40 mA, o próximo alvo é o painel: tear-down completo do
   DSI (release LDO ch3) no standby e re-init no wake (~0,3–0,5 s aceitáveis).
   Já está mapeado como otimização; não fizemos agora para manter o wake instantâneo.
4. Se HIBERNATE ficar > ~1 mA, investigar rails sempre-vivos (TLV62569, IP5306
   quiescente) — aí é limite de placa, não de firmware.

## SD × light sleep: por que remontamos no wake

Medido em hardware (2026-09-22): após `STANDBY`, qualquer I/O no cartão
falha com `sdmmc_host_wait_for_event returned 0x107` — o host SDMMC sai
do estado funcional no sleep e o driver do IDF 5.5 não o re-inicializa no
wake. `storage_remount_sd()` (unmount + mount, ~0,3–0,5 s) roda no hook de
wake do `power_mgmt`, antes de a UI voltar a tocar no VFS; se o cartão não
voltar, a raiz ativa degrada para `/internal/pda` e o PDA segue usável.
Os `--- ERROR: device reports readiness to read...` do monitor são só o
USB-Serial-JTAG dormindo (cosmético).

## Bootloop de 2026-09-23: NaN em `ui.scale`

Cadeia: `system.lua` com `scale` não-finito → `apply_ui_scale(NaN)` →
tokens do Theme NaN → core Rust do Slint panica no layout → Guru
Meditation → reset em loop. Defesas adicionadas (todas em `pda_config.c`
+ `main.cpp`): `tbl_int`/`tbl_flt` recusam não-finitos, `clamp_all` tem
cheque explícito de `isfinite` (NaN escapa de `<`/`>`), `apply_ui_scale`
re-clampa na entrada. **Regra geral: qualquer número que venha de arquivo
ou de binding Slint e vire geometria deve ser validado com `isfinite`.**

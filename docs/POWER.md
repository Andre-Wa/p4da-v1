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
  A volta é SÓ por RESET/power-on (deep sleep sem wake source, decisão
  desta placa) — e desde o **M4.13b** o boot detecta esse retorno por um
  flag NVS (`pdapwr/hib`, gravado antes do deep sleep e consumido ao
  restaurar) e reabre a tela/nota da sessão. Sem o flag,
  `ESP_RST_DEEPSLEEP` nunca ocorria (não há wake source) e a restauração
  era código morto — bug achado na validação do M4.13 em 2026-10-04.
- Antes do deep sleep: `storage_shutdown_sd()` (unmount FatFS + LDO ch4 off)
  para não corromper o cartão nem gastar o rail dele dormindo.

## Decisões configuráveis (Lua: `config/system.lua`)

| Chave | Default | Significado |
|---|---|---|
| `power.dim_after_s` | 30 | ocioso → dimeriza |
| `power.screen_off_after_s` | 120 | ocioso → STANDBY |
| `power.deep_sleep_after_s` | 0 | em STANDBY, hiberna após N s (0 = nunca) |
| `power.wake_on_touch` | false | gateia SÓ o wake por toque do STANDBY (ISR do GT911); toque como atividade (segurar idle, acordar de DIM) é SEMPRE ativo — semântica M4.14.2, pedida na validação. Com a chave OFF, em STANDBY o toque nem é entregue à UI (proxy `get_xy`, M4.14.3): sem wake por callback nem action fantasma de elemento |
| `power.boot_btn_standby` | false | M4.14.4: pressão nova do BOOT em ACTIVE/DIM pede standby (toggle com o wake por BOOT, que é sempre armado) |

## M4.14 — rework de energia (IMPLEMENTADO 2026-10-04, aguardando hardware; checklist `validacoes/VALIDACAO_M4.14.md`)

Observações do hardware que viram tarefa aqui:

1. **`power.wake_on_touch` não gateia o wake por toque de verdade.** O ISR
   do INT (GPIO21) é instalado sempre em `power_mgmt_init()` e dá o
   semáforo de wake em STANDBY; a chave hoje só entra no polling de idle
   do `power_task`. Tarefa: com a chave OFF, desarmar o ISR/ignorá-lo no
   wake (acidentes no bolso); com ON, comportamento atual. A SwitchRow já
   existe em settings.slint ("Toque segura o idle / acorda").
   **FEITO M4.14**: snapshot da chave em `enter_standby()`
   (`s_touch_wake_ok`); a ISR ignora toque com OFF. Proveniência do wake
   no log (`acordou do STANDBY (wake: ...)`).
2. **STANDBY ainda "faz coisas".** No modo idle robusto a CPU segue viva:
   Wi-Fi reconecta (log mostra `desconectado — reconectando em 2 s` DENTRO
   do standby) e NTP atualiza. Tarefa: pausar reconexão automática e o
   timer de NTP ao entrar, retomar no wake (`wifi_net_pause/resume`);
   avaliar `host-power-save` do esp_hosted para modem-sleep do C6.
   **FEITO M4.14** (pause/resume via standby_cb; `esp_sntp_stop`/
   `sntp_restart`); modem-sleep do C6 fica p/ o próximo estágio
   (§ Pesquisa e plano em estágios, abaixo).
3. **Botão BOOT como wake explícito.** O ISR de GPIO35 já é instalado no
   init e dá o semáforo — validar na placa (e conferir se alguma
   reconfiguração de GPIO posterior não mata o `intr_type`).
   **FEITO M4.14 (código)**: conferido por inspeção que nada reconfigura
   GPIO35 após `power_mgmt_init()`; aceitação HW no checklist.
4. **DIM com idle acumulado logo após o NTP** (log da validação de
   2026-10-04): `DIM (idle 21s)` em t≈24,4 s, com `dim_after_s=5` e um
   `DIM (idle 5s)` prévio em t≈7,6 s — uma transição ACTIVE→DIM com 21 s
   de idle implica ou `power_task` ~16 s sem avaliar o estado (starvation
   durante o burst hosted/NTP?) ou um wake DIM→ACTIVE que não resetou
   `s_last_activity_us`. Cosmético no pior caso (blink de ~0,1 s na
   sincronização do relógio); fechar com evidência adicionando log de
   transição de estado (ACTIVE/DIM/STANDBY + causa) antes de mexer.
   **INSTRUMENTADO M4.14**: `set_state(st, why)` loga toda transição com
   motivo + idle; recorrência agora tem culpado nomeado. Medição segue
   em aberto (sem correção de comportamento ainda, de propósito).

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

## Pesquisa e plano em estágios do rework (fundido de POWER_REWORK.md em 2026-10-06)

Status desde a redação original: M4.14/M4.15/M5.0b consumiram parte do
Estágio 1 (pause Wi-Fi/NTP, GT911 sleep, retry fix); C6-off no hibernate
REVERTIDO (force-hold unsafe no P4, ver M4.15/M5 no ROADMAP); DFS/`esp_pm`
e teardown do painel seguem ADIADOS até o Estágio 0 (medição).

## 2. Para onde vai a energia (hipóteses NÃO medidas)

### 2.1 STANDBY robusto (suspeitas, em ordem provável)
backlight 0 já é o maior alívio; restam: CPU 360 MHz sem DFS (§3.5),
painel ST7701S + DSI PHY alimentados (blank ≠ off), GT911 scanning
(~3,5 mA típicos), C6 com modem ativo + loop de reconexão, rail do SD
(LDO ch4) ligado, VBUS do USB host, PSRAM XIP.

### 2.2 HIBERNATE (vizinhança manda)
O P4 some (µA), mas a placa não: **C6 com CHIP_PU preso em pull-up**
(receita do issue 18443: GPIO54 baixo + `gpio_force_hold_all` antes do
sleep), GT911 acordado (3,5 mA → <50 µA com comando de sleep), rail do
painel/DSI se não desligado, ES8311, quiescente do IP5306. Lição do 18443:
número de datasheet é do chip nu; em devboard, os vizinhos definem o piso
—"the dev board is simply not designed with deep sleep in mind".

## 3. Pesquisa (achados + fontes)

**3.1 Deep sleep no P4.** Wake por GPIO só no domínio VDD_LP (LP =
GPIO0–15 aqui); por default o sleep ISOLA os GPIOs — vizinhos com pull-up
externo podem vazar; reter com `gpio_hold_en`/`gpio_force_hold_all`.
C6: GPIO54 (Slave_Reset) baixo antes do sleep segura-o off. [1][5]

**3.2 GT911 tem sleep de verdade.** Comando I2C `0x05` no registro
`0x8040` (requer INT baixo antes do comando); 3,5 mA → <50 µA; existe
também doze/gesture mode p/ wake-on-touch em produtos de bateria.
No STANDBY com `wake_on_touch=false`, dormir o GT911 é ganho líquido
imediato; com `=true`, avaliar doze mode. [6][7][8]

**3.3 IP5306 corta saída em carga baixa** (~45–60 mA, ~30 s, sem USB) —
auto-desligue de power bank; a variante I2C permitiria configurar, mas a
nossa NÃO tem I2C (HARDWARE.md achado #3) → não dá p/ desligar o
auto-off por software; projeta-se em volta: hibernate="off" é feature;
medir o STANDBY otimizado p/ garantir que fica ACIMA do limiar na bateria
(ou aceitar o corte como power-off acidental? decisão §5). [9][10][11]

**3.4 Wi-Fi/NTP pausáveis no standby** (M4.14#2): `wifi_net_pause/resume`
+ parar timer SNTP; reconexão retoma no wake. Ganho direto no C6.

**3.5 `esp_pm_configure()` liga DFS + auto-light-sleep.** Sem ele,
`CONFIG_PM_ENABLE` não escala frequência nem dorme sozinho. Chamá-lo
(após o bring-up) + auditoria de locks (DSI video-mode e o tick do Slint
podem pedir lock de freq) = ganho "grátis" em idle/DIM/standby robusto.
Risco baixo/médio; medir antes/depois. [1]

**3.6 ESP-Hosted power save existe**: o componente 2.12.9 registra o CLI
`host-power-save` no nosso boot; o exemplo `host_network_split__power_save`
(esp-hosted-mcu) documenta o lifecycle (slave só dorme com o bus SDIO
ocioso, hooks no lifecycle do host). API exata do host IDF a confirmar no
`managed_components` da sua máquina (grep `power_save` nos headers do
componente). Efeito esperado: modem-sleep do C6 durante standby. [12]

**3.7 Painel: blank ≠ off.** Opções em custo/crescente: (a) comando
de sleep do ST7701S via DSI (chechar BSP/datasheet do painel); (b) tear-down
completo (LDO ch3 off + deinit DSI) com re-init de 0,3–0,5 s no wake — já
mapeado no POWER.md como otimização pós-medição. [2][4]

## 4. Plano em estágios (proposta p/ M4.14 → M-power)

**Estágio 0 — MEDIR** (amperímetro em série no cabo da bateria/CN4):
ACTIVE 100/50/10, DIM, STANDBY, HIBERNATE; 3 amostras cada. Sem isso,
otimizamos no escuro (plano já existe no POWER.md; agora é porta de
entrada de todo o resto).

**Estágio 1 — software de baixo risco (M4.14):** pausar Wi-Fi/NTP no
standby; dormir o GT911 no standby quando `!wake_on_touch`; no HIBERNATE:
GPIO54 baixo + force-hold (C6 off) e GT911 sleep; `esp_pm_configure()` +
auditoria de locks; log de transição de estado (fecha a anomalia #4);
`wake_on_touch` gateando o ISR (#1); validar wake do BOOT no standby (#3).
Aceite: logs + ΔmA do Estágio 0 repetido.

**Estágio 2 — painel** (se a medição apontar painel/DSI como dominante no
standby): sleep do ST7701S ou tear-down LDO ch3 + DSI, wake c/ re-init
(0,3–0,5 s aceitáveis).

**Estágio 3 — M-power (light sleep)** SOMENTE se 1+2 não atingirem a meta:
rework de re-init coordenado hosted+SDMMC+USB no wake, ou tear-down
completo do hosted antes do sleep.

**Estágio 4 — hibernate v2 (a "refatoração"):** base = M4.13b (flag NVS +
session.txt); acrescentar desligamento da vizinhança antes do deep sleep
(C6, GT911, painel); UX documentada: USB → RESET restaura; bateria →
IP5306 corta, BF2/USB religam e restauram; opcional mod BF2→LP GPIO p/
wake por botão sem power-cycle. Critério: boot pós-hibernate restaura
tela+nota nos dois cenários de alimentação.

## 5. Decisões em aberto (com você)

1. Medir primeiro (Estágio 0) com amperímetro, ou Estágio 1 direto e
   medir depois?
2. Corte do IP5306 em standby otimizado na bateria: aceitar como
   "power-off" ou manter margem acima do limiar?
3. Mod BF2→LP GPIO: entra em alguma rodada de hardware sua?
4. M4.13b: flashear o patch agora (valida hibernate no binário atual) ou
   esperar o Estágio 4 junto?

## Fontes

[1] IDF Sleep Modes (ESP32-P4): wake só em GPIOs do VDD_LP; isolamento de
GPIOs no sleep — https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/sleep_modes.html
[2] ESP-IDF issue #18443 (P4 deep sleep não reduz consumo): C6 off via
GPIO54 + `gpio_force_hold_all`; vizinhos (ETH PHY) dominavam; "dev board
not designed with deep sleep in mind" — https://github.com/espressif/esp-idf/issues/18443
[3] Datasheet ESP32-P4 (Elecrow mirror): 55 GPIOs, 16 LP — https://www.elecrow.com/download/product/DHE04310D/esp32-p4_datasheet_en.pdf
[4] POWER.md/HARDWARE.md internos: LDO ch3/ch4, kill-switch, IP5306/BF2.
[5] Datasheet GT911 (FORTEC): sleep por comando I2C, INT baixo antes — https://www.fortec-integrated.de/fileadmin/pdf/produkte/Touchcontroller/DDGroup/GT911_Datasheet.pdf
[6] GT911 Programming Guide (Orient Display): `0x05` em `0x8040` — https://www.orientdisplay.com/pdf/GT911.pdf
[7] Correntes GT911: 3,5 mA ativo, <50 µA sleep, <10 µA hibernation — https://focuslcds.com/application-notes/programming-a-capacitive-touch-panel-utilizing-the-gt911-touch-controller/
[8] Goodix forum: wake-on-touch em sleep/doze — https://developers.goodix.com/en/bbs/detail/ddf4973acf244c89a5f2f80505c49767
[9] M5Stack community: IP5306 corta VOUT <45 mA após ~32 s — https://community.m5stack.com/topic/62/ip5306-automatic-standby
[10] lewisxhe/esp32-camera-series#26: IP5306 desliga em deep sleep (~5 mA) — https://github.com/lewisxhe/esp32-camera-series/issues/26
[11] Arduino forum: IP5306 I2C e auto-shutdown configurável (~30 s) — https://forum.arduino.cc/t/using-esp32-with-ip5306-i2c-power-management-and-voltage-protection/1435971
[12] esp-hosted-mcu exemplo host_network_split__power_save (lifecycle do
host power save) — https://github.com/espressif/esp-hosted-mcu/blob/main/examples/host_network_split__power_save/README_light_sleep.md

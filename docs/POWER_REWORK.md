# Energia — estado atual, pesquisa e plano de rework (M4.14 / M-power)

Estudo de 2026-10-04, pedido após a validação do M4.13 (hibernate ainda
aberto — ver §1.3). Fontes externas no fim; fatos de placa vêm de
`docs/HARDWARE.md` e `docs/POWER.md` (não repetidos aqui na íntegra).

## 1. Como funciona HOJE (mapa do código)

### 1.1 Os degraus

```
ACTIVE ──idle dim_after_s──▶ DIM ──idle screen_off_after_s──▶ STANDBY ──deep_sleep_after_s ou manual──▶ HIBERNATE
   ▲        (backlight 12)      ▲        (backlight 0,           │            (sessão+NVS, SD unmount,
   │                           │         painel blank)          │             deep sleep SEM wake source)
   └────── atividade ──────────┴────────── wake ────────────────┘
```

| Estado | Gatilho de entrada | O que DESLIGA | O que segue VIVO | Como sai |
|---|---|---|---|---|
| ACTIVE | boot / atividade | — | tudo | idle → DIM |
| DIM | `idle >= power.dim_after_s` (default 30; seu config: 5) | backlight → 12 | tudo (CPU 360 MHz, Wi-Fi, SD, USB, GT911 scanning, DSI/phym) | atividade volta p/ ACTIVE; `screen_off_after_s` → STANDBY |
| STANDBY (robusto, default) | `idle >= screen_off_after_s` (120) ou "Suspender" (sticky, M4.13) | backlight 0 + painel blank | **CPU idle a 360 MHz sem DFS/auto-sleep**, Wi-Fi reconectando, NTP, SD rail (LDO ch4), USB host, GT911, DSI PHY | ISR GPIO (BOOT/toque) no semáforo, ou qualquer `power_mgmt_activity()`; wake imediato |
| STANDBY (light sleep) | kill-switch SEMPRE off (M-power) | CPU dorme | — | crash no wake (SDMMC×SDIO hosted) + wake não disparava → desativado 2026-09-24 |
| HIBERNATE | manual (Config) ou `deep_sleep_after_s` no STANDBY | sessão→`session.txt`, flag NVS (M4.13b), SD unmount + LDO ch4 off, deep sleep | só o que a placa deixa (ver §2.2) | **sem wake source**: EN/RESET (USB) ou corte do IP5306 + tecla BF2 (bateria) |

Fontes de atividade (`power_mgmt_activity()`): teclas USB HID, callbacks de
UI e — só se `power.wake_on_touch` — o poll do INT do GT911 no `power_task`.
O ISR de wake (BOOT + GT911) é armado SEMPRE no init, independente da chave
(bug de design anotado como M4.14#1: a SwitchRow só segura o idle, não
gateia o wake).

`esp_pm_configure()` NUNCA é chamado: com `CONFIG_PM_ENABLE=y` mas sem
configure, não há DFS nem auto-light-sleep — a CPU idle roda a 360 MHz o
tempo todo, inclusive no STANDBY robusto. (§3.5)

### 1.2 Hibernate hoje: três verdades que se somam

1. **v4.13 (binário que você tem)**: `woke_from_hibernate()` exigia
   `ESP_RST_DEEPSLEEP`, impossível sem wake source → restauração era
   código morto. Corrigido no **M4.13b** (flag NVS `pdapwr/hib`) — patch
   em `patches/M4.13b-hibernate.patch` (workspace), **ainda não buildado**.
2. **Na bateria**: o auto-desligue do IP5306 (carga abaixo do limiar)
   corta VOUT no hibernate — por design vira o "desligar" do PDA; a volta
   é tecla **BF2** (ao lado do conector de bateria) ou plugar USB → boot
   frio → restaura sessão. RESET com VOUT cortado NÃO boota.
3. **No USB** (seus testes c/ monitor): VOUT preso → deep sleep só sai por
   EN/RESET → boot frio → (M4.13b) restaura. Sem o fix: launcher limpo,
   exatamente o relato da validação.

BOOT=GPIO35 e INT do GT911=GPIO21 são GPIOs **HP**: nunca acordam deep
sleep (wake de deep sleep só VDD_LP = GPIO0–15 nesta placa). O mod
opcional BF2→LP GPIO (ex. GPIO4/GPIO6 no header de 26 pinos) segue
documentado em HARDWARE.md como o caminho p/ wake de hibernate por botão.

### 1.3 Abertos na fila (donos deste doc)

- M4.14#1 wake_on_touch gatear o ISR; #2 pausar Wi-Fi/NTP no standby;
  #3 validar BOOT como wake do standby; #4 anomalia `DIM (idle 21s/10s)`
  pós-NTP (transição sem causa nos logs → instrumentar).
- M-power: rework do light sleep (só se os estágios 1-2 não bastarem).
- Hibernate "refatoração" sentida na validação: vira o Estágio 4 (§4).

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

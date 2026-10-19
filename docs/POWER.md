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

## Bateria: nível em % (BAT.0), estabilidade em carga (BAT.2), calibração (BAT.1) e % sob carga (BAT.1c)

Desde a v5.12.0 a status bar mostra % real: divisor da folha 03
(BAT+ → R52 68K → nó → R57 100K → GND; nó → R55 0R → GPIO53 = ADC2_CH4),
lido por `main/battery.c` (topologia e caveats em docs/HARDWARE.md §
"Nível de bateria: divisor no GPIO53"). O log periódico
`battery: NN% | mV | nó mV | raw | estado` é a matéria-prima da
calibração. **BAT.0 validado em hardware 2026-10-12** (log do usuário,
v5.12.0): % na status bar, leitura periódica estável em repouso
(4077-4080 mV / 89% por ~10 min), comportamento em carga coerente
(4170-4174 mV / 97%) e assentamento pós-carga visível (4135 mV / 94%).

### BAT.2 — por que o % "pula" ao mexer no carregador (v5.13.0)

Sem pino de status do IP5306 roteado ao P4, o firmware NÃO sabe por
hardware se há carregador; ele INFERE por tensão, e isso explica tudo o
que se vê em campo:

| evento físico | tensão no divisor | o que a UI mostra |
|---|---|---|
| repouso na bateria | OCV real (ex.: 4079 mV) | % pela curva (89%) |
| carregador ligado (CC/CV) | tensão de TERMINAL, até ~4.2 V | % alto (97%) — é a física, não é bug |
| carregador removido | queda rápida p/ OCV + sag | % **congelado** ("assentando") |
| tensão assentada (sag ≥ 90 mV do pico, ou 10 min) | OCV de novo | % reassume pela curva (94%) |

Máquina de estados em `main/battery.c` (`BSTAT_BATTERY/CHARGING/
SETTLING`, v5.13.0): salto de +40 mV num poll de 4 s = carregador ligou
(impossível só com química); queda de −25 mV estando em carga =
carregador saiu → congela o % até assentar (mata o efeito sanfona
97→94→97 de tirar/recolocar); em "assentando", só volta a "carregando"
se a tensão retornar ao pico−30 mV (rebound pós-unplug não conta).
Teste de host: `tools/hosttest/host_batt_test.c` (perfis do log real).
O estado aparece no log periódico, no Config→Sistema
(`battery_describe`) e, desde a v5.14.0, no SUFIXO da status bar
(`90%+` carregando, `90%~` assentando) p/ leitura sem serial:
"bateria" | "carregando" | "assentando".

Limites honestos: nos primeiros minutos APÓS conectar o carregador o %
sobe rápido (terminal em CV) e é otimista; "100%" confiável só em
repouso pós-carga. Trocar de bateria da MESMA química (Li-ion/LiPo
4.2 V — 14500, 18650, pouch) NÃO exige ajuste: a curva é %-vs-TENSÃO, e
capacidade (mAh) só muda autonomia, não a curva. Atenção: células 14500
reais têm ~0.6-0.9 Ah apesar de rótulos "3000 mAh" — a autonomia virá
menor que o rótulo. Químicas OUTRAS (LiFePO4 3.6 V máx, Li-ion 4.35 V)
exigem editar `s_curve` em main/battery.c (e o teto BATT_MV_MAX).

### BAT.1a — âncoras aprendidas em campo (v5.14.0, IMPLEMENTADO)

Medição de campo da 13ª rodada (v5.13.0, célula 14500 "3000 mAh"):
**4156 mV em carga** no divisor e **3.9 V no multímetro** logo após
despluguar. Não é erro de leitura: é a tensão de TERMINAL do CC-CV
(relaxo de ~250 mV quando a corrente de carga some, agravado pela
resistência interna alta de 14500 genérica). Consequência: o % NA
bateria caía para a faixa de 68% com a célula ainda cheia — a curva
genérica 3300-4200 mV não conhece os EXTREMOS reais desta célula.

Resposta (sem multímetro, sem serial, sem botão de calibração manual):
o módulo aprende os extremos sozinho e remapeia a curva entre eles.

| âncora | quando aprende | guardas |
|---|---|---|
| `vmin` (0%) | estado **bateria** E pct ≤ 30 (perto do vazio: é onde o corte real acontece) | [2800,4400] mV abs.; vão ≥ 600 mV; histerese 15 mV; simétrica (célula que corta em 3.45 V SOBE a âncora — senão o PDA "morre aos 15%") |
| `vmax` (100%) | estado **carregando**, acima da âncora atual + 15 mV (topo CC-CV; meia-carga não move) | idem; só sobe |

Persistência: NVS namespace `batt`, chaves i32 `vmin`/`vmax` (escrita só
quando uma âncora move > 15 mV — dezenas de writes na vida do aparelho).
`battery_calib_load()` valida o par no boot (faixas + vão); par inválido
= padrão de fábrica. Remapeamento: `std_mv()` = 3300 + (mv−vmin)·900/
(vmax−vmin); a FORMA da curva continua a LiPo genérica (refino de forma
segue em BAT.1b). Botão **"Zerar calibração da bateria"** no
Config→Sistema restaura 3300-4200 mV e persiste.

**Teste de campo SEM serial** (o USB-C que loga é o que carrega — na
bateria não há log):
1. Status bar mostra o estado por sufixo: `90%+` = carregando, `90%~` =
   assentando, sem sufixo = bateria (só ASCII, sem risco de glifo).
2. Config→Sistema, linha "Bateria", atualiza a cada 4 s AO VIVO:
   `NN% (mmmm mV, ânc VMIN-VMAX mV, ADC2_CH4/GPIO53, cal. eFuse[, estado])`
   — é o "serial visual": dá p/ fotografar a tela e coletar os pares
   mV/estado de uma descarga inteira.
3. Procedimento de aprendizado: use a placa NA BATERIA até ela desligar
   sozinha (o último `vmin` fica gravado no NVS ANTES do corte — a
   gravação ocorre a cada novo mínimo com pct ≤ 30, não no desligamento);
   depois carregue até o fim com a placa ligada (o topo CC-CV grava
   `vmax`). Reconecte o USB e leia as âncoras na linha Bateria (ou no
   log `battery: âncoras da bateria (NVS): VMIN-VMAX mV`).
4. Autonomia por degrau: com a linha ao vivo, anote %/mV e hora em cada
   troca de degrau (ACTIVE/DIM/STANDBY) — fecha o item 3 do BAT.1b sem
   amperímetro.

Teste de host: `tools/hosttest/host_batt_test.c` seções G–K (identidade
com âncoras de fábrica, aprendizado simétrico de vmin, vmax em carga,
guardas de pct/absoluto/histerese/vão, round-trip NVS + reset).

### BAT.1c — estabilidade do % sob carga (v5.15.0, IMPLEMENTADO; validação HW pendente)

Medição de campo da 14ª rodada (v5.14.0, célula 14500, âncoras NVS
2815-4200 mV): descarga na bateria variou de **3950 mV (máx, ±50 mV)**
até **2815 mV (mín, tudo ligado)**; no carregador, **4176 mV carregando
e 4104 mV cheio**. Queixa: *"utilizar o brilho mais alto ou baixo, wifi,
etc. influencia na voltagem lida, deixando a porcentagem variando muito
durante o uso"*. É física, não bug de leitura: a tensão no divisor é a
de TERMINAL (OCV − I·R_interno; R alto nesta 14500 genérica + corrente
de entrada do boost do IP5306), então cada evento de carga elétrica
move a leitura:

| evento | ordem do sag/recuperação no divisor |
|---|---|
| brilho mínimo→máximo (backlight ST7701) | −50 a −200 mV (e volta ao dimerizar) |
| rajada TX do Wi-Fi (C6 via SDIO) | pulsos de dezenas de mV entre polls |
| beep/falante NS4150 (VOUT-BAT) | dezenas de mV durante o tom |
| descarga profunda com tudo ligado | até 2815 mV (o mínimo aprendido) |

Com a curva antiga (EMA α=1/4 e % = curva(EMA) direto), ±50 mV viravam
±3-5 pts de % na tela, nos dois sentidos, a cada evento. Três respostas
em `main/battery.c`:

1. **EMA lenta** (α=1/16 por poll de 4 s, τ≈64 s) vira a fonte do % NA
   BATERIA. A EMA rápida (α=1/4) segue dona da lógica de carga/
   assentamento da BAT.2 — intocada. O log serial imprime as duas
   (`bateria: NN% | mV | lenta mV | nó | raw | estado`), e a linha do
   Config→Sistema mostra o filtro do qual o % deriva (par %-mV
   coerente; a física do sag continua visível no serial).
2. **Passo no % exibido** (`batt_pct_paced`): cai no máx. **1 pt a cada
   3 polls (5%/min)** e só sobe se a curva passar do exibido em **>2
   pts (banda morta)**, no máx. **1 pt a cada 6 polls**. Uma descarga
   real (~0.5-1%/min) é muito mais lenta que o teto de queda — o passo
   nunca esconde o fim da bateria; o que ele elimina é a dança.
   Resultado de campo esperado: sob troca de brilho/Wi-Fi o % oscila
   ≤±2 pts e devagar, em vez de ±5 pts instantâneos.
3. **Salto de carregador com confirmação de 2 polls**: +40 mV num poll
   só não basta mais — *alívio* de carga (dimerizar o backlight, fim de
   beep) recupera 50-150 mV num poll e fantasiava de carregador
   (sufixo "+" e % seguindo tensão alta). Agora: candidato no salto,
   confirma no poll seguinte se o nível se SUSTENTOU (≥ candidato−15)
   E está ACIMA da EMA lenta +40 mV (recuperação de sag volta AO nível
   do filtro; carregador CC/CV levanta ACIMA dele). Custo: o "+"
   aparece 4 s depois — irrelevante p/ evento de minutos. O "Zerar
   calibração" também religa o % na hora (flag `s_recal` consumida pela
   task no poll seguinte) em vez de esperar o passo caminhar.

Trade-offs aceitos (documentados p/ não virarem "bug" depois): durante
uma descarga REAL rápida o % exibido atrasa alguns minutos (o passo é
o limitante só nesse caso extremo); após alívio de carga longo o % não
sobe de volta além da banda morta (fica ≤2 pts abaixo da curva até a
próxima carga); a âncora vmin=2815 aprendida é o sag SOB CARGA — para
esta placa é justamente o limite real de operação (o corte acontece
com a tensão já caída), então fica.

Teste de host: `tools/hosttest/host_batt_test.c` seções L–O (sag de 1
poll + recuperação NÃO vira carregador; queda ritmada 1 pt/3 polls;
subida ritmada com banda morta; dança ±15 mV não move o %; reset
religa o % na curva nova no 1º poll).

### BAT.1b — o que resta calibrar (com multímetro)

1. Pares multímetro × `battery:` em repouso a cada ~10% p/ refinar a
   FORMA da `s_curve` (as âncoras já corrigem os extremos; o joelho do
   meio segue genérico).
2. Conferir o fator do divisor: `V_bat/V_nó` medido deve dar ~1.68
   (tolerância dos resistores); se divergir >2%, ajustar
   `BOARD_BATT_DIV_R_TOP_K/BOT_K` no board_config.h.
3. Anotar o comportamento em carga: durante CC/CV a leitura lê alto de
   propósito; "100%" só é confiável em repouso pós-carga.

## Plano de conclusão da bateria (definido 2026-10-17, v5.16.0)

Pedido do usuário na 15ª rodada: "um plano para terminar a implementação
da bateria". Ordem de implementação = ordem das fases; cada fase valida em
HW antes da seguinte. O que já está FEITO/validado: BAT.0 (leitura real),
BAT.2 (máquina de estados), BAT.1a (âncoras aprendidas), BAT.1c (% estável
sob carga — ✅ 15ª rodada).

### Observações de campo da 15ª rodada (log do usuário, v5.15.0 em carga)
- BAT.1c funcionando como projetado: `salto candidato +262 mV → confirmando
  no próximo poll → carregador confirmado: +269 mV sustentado` (o alívio de
  carga não fantasiou de "+"); % travado em 98% durante todo o ciclo
  plug→unplug→assentando; EMA lenta convergindo visivelmente (4014→4173).
- "Assentando" longo NÃO é bug: a saída exige sag ≥ 90 mV do pico
  (`BATT_SETTLE_SAG_MV`) OU 150 polls/10 min (`BATT_SETTLE_POLLS`) OU voltar
  ao nível pré-carga +15 mV. No log, o sag era 83 mV aos 8 min → a saída
  viria pelo teto de 10 min com snap honesto p/ a curva (~92% em 4096 mV).
  Bateria cheia de verdade segura ~4,1 V em repouso: o snap pós-carga é a
  leitura mais confiável do ciclo.
- Flap observado: `carregador saiu (4175 mV)` → 4 s depois `carregador de
  volta` (t=714612/718613). A queda CC/CV ou um degrau de carga de ≥25 mV
  num poll único (`BATT_DROP_MV`) já derruba o estado. Cosmético (o % seguiu
  segurado), mas vira BAT.2b abaixo.

### Fases
1. **BAT.2b — debounce das transições** (pequena, entra junto da próxima
   rodada de fixes): saída de carga confirmada em 2 polls (espelho da subida,
   que já é confirmada); saída de "assentando" adiantada quando a EMA rápida
   e a lenta convergem (|rápida−lenta| ≤ 5 mV por 3 polls) — o teto de 10 min
   deixa de ser o caminho comum. Teste de host: novos casos no
   `host_batt_test.c` com a série EXATA do log da 15ª rodada (o flap não
   pode mais trocar de estado; a convergência encerra o assentamento cedo).
2. **BAT.1b — FORMA da curva** (já especificada acima neste arquivo):
   pares multímetro × linha ao vivo do Config→Sistema em repouso a cada
   ~10% (procedimento sem serial no §BAT.1a item 4); ajustar `s_curve`
   piecewise-linear p/ a célula 14500 do usuário; conferir o fator do
   divisor (~1,68). Sem os pares do usuário esta fase NÃO anda — é a única
   fase do plano que depende de coleta dele.
3. **BAT.3 — autonomia estimada**: perfis de corrente por estado de energia
   (ACTIVE brilho X, DIM, OFF/standby, hibernate; deltas de Wi-Fi, beep e
   PA tocando — medir em série com o multímetro, procedimento na §Plano de
   medição); integrador de mAh consumidos no NVS (append a cada min);
   "≈Xh restantes" na linha Bateria do Config→Sistema. Lógica pura do
   integrador nasce com teste de host.
4. **BAT.4 — proteção de bateria crítica**: avisos em degraus (15% toast;
   8% força DIM + aviso; 4% salva sessão + hibernate de emergência antes do
   brownout corromper SD/NVS). Máquina de limiares com histerese (não pode
   oscilar aviso/normal em sag) + teste de host. Interage com POWER.md
   §arquitetura em degraus: o hibernate de emergência usa o MESMO caminho
   do hibernate manual (sessão + flag NVS).
5. **BAT.5 — saúde/histórico (opcional, só se 1–4 validados)**: mín/máx
   diários em anel NVS (30 dias), contagem estimada de ciclos (Ah
   descarregado acumulado / capacidade nominal), linha "Saúde" no
   Config→Sistema. Serve p/ detectar a degradação da 14500 de campo
   (resistência interna alta já observada na 14ª rodada).

Aceite geral do plano: ao fim de BAT.4, o PDA nunca desliga "seco" (sempre
hiberna com sessão salva) e o usuário sabe, sem serial, o % real (curva
calibrada), a autonomia estimada e o histórico de saúde.

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
[5] Esquemático GUITION JC4880P443C_I_W, folha 03 "Expand IO" (divisor de
BAT+ R52 68K/R57 100K → GPIO53): repo de notas de campo da comunidade
ultramcu/guition-jc4880p443c-i-w (schematic/03-*.webp) —
https://github.com/ultramcu/guition-jc4880p443c-i-w
[6] Register file IP5306 (I2C, V1.21, Injoinic): reg 0x70/0x71/0x72/0x77
(carga/cheia/carga-leve/tecla) e o aviso de que o chip NÃO tem tensão/
corrente internos — https://done.land/assets/files/ip5306_i2c_registers.pdf
[7] done.land "IP5306 I2C": versão padrão do IP5306 NÃO implementa I2C
(só variante custom, pinos L1/L2/L3; aqui NC) —
https://done.land/components/power/powersupplies/battery/chargers/charge-discharge/ip5306/ip5306i2c/
[8] Datasheet ESP32-P4 (Espressif): dois SAR ADC de 12 bits, 14 canais —
https://documentation.espressif.com/esp32-p4_datasheet_en.pdf
[9] IDF v5.5.1: soc/adc_channel.h do esp32p4 (GPIO53 = ADC2_CHANNEL_4) e
exemplo examples/peripherals/adc/oneshot_read (padrão de calibração
usado em main/battery.c).

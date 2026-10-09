# Roadmap — do M1 ao PDA completo

Cada milestone fecha com critério de aceite testável no hardware.
Nada de M(n+1) começa com pendência de M(n).

## M1 — Fundação (ESTE commit)
- [x] Pinagem reconciliada com a JC4880P443C_I_W (`board_config.h`, `docs/HARDWARE.md`)
- [x] SD como armazenamento principal: 4-bit @ 40 MHz, raiz lógica `/pda`, espelho de settings
- [x] Configurações em Lua (`config/system.lua`) + tela Config funcional
- [x] Runtime Lua protegido + tela Scripts (lista + console) + API `pda.*`
- [x] Gerenciador de arquivos navegável (entrar/sair de diretórios, abrir texto)
- [x] Notas (criar/editar/salvar/apagar) na raiz ativa
- [x] Energia em degraus: DIM → STANDBY (light sleep, wake botão/toque) → HIBERNATE
      (deep sleep c/ sessão restaurada); botões "Dormir agora"/"Hibernar"
- [x] Status bar (uptime, armazenamento, bateria "--", estado de energia)
- **Aceite**: boot sem cartão não trava; com cartão, tudo grava nele; standby
  acorda por botão; hibernate restaura a nota aberta; `exemplo.lua` roda sem derrubar.

## M2 — Edição de texto de verdade (FEITO, exceto destaque)
- [x] Editor com cursor visível ("|" na linha de exibição) e móvel:
      setas/Home/End/Delete no teclado USB, toque posiciona o cursor no
      FIM da linha tocada, OSK tem setas/Home? (setas + del; Home/End via
      teclado físico), scroll vertical e horizontal (Flickable)
- [x] **Teclado virtual** em 4 fileiras + shift + espaço/enter/backspace/
      setas/del, automático quando `onscreen_keyboard_auto && !USB`,
      ou manual pelo botão "Tecl" (tri-estado: auto/força-on/força-off)
- [x] Gerenciador abre qualquer arquivo texto (.txt .lua .md .csv .log
      .ini .json) no editor; Salvar grava de volta no mesmo caminho
- [x] "Apagar" só aparece para arquivos dentro de `notes/` (segurança)
- [ ] Destaque leve para `.lua` — DEFERIDO (opcional no escopo original)
- **Aceite**: editar um `.lua` pelo teclado virtual e executá-lo na tela
  Scripts.
- Limitações conhecidas (documentadas, não bugs): o toque posiciona a
  LINHA (coluna via setas/OSK); largura de scroll horizontal é estimada
  (10 px/char p/ fonte 16 px); o editor instancia um elemento por linha
  (Flickable), então arquivos com milhares de linhas ficam lentos —
  virtualização fica p/ M3.

## M2.5 — Design system + modularização da UI (FEITO)
- [x] `app_ui.slint` monolítico -> módulos: `theme.slint` (tokens M3-dark
      adaptados), `elements/` (botões/superfícies/OSK), `screens/` (1 tela/arquivo)
- [x] Padronização visual: state layers, elevação tonal, type scale,
      botões por papel (filled/tonal/danger/ghost) — ver `docs/UI.md`
- [x] API pública da `AppWindow` (contrato com o C++) intacta

## M3a — Ícones (FEITO)
- [x] Subset Material Icons (3,4 KB, Apache-2.0) + `elements/icons.slint`
      (`IconForKind` com literais estáticos; C++ envia só categoria)
- [x] Ícones por tipo nas listas + ícone de armazenamento na status bar

## M3b — Operações de arquivo (FEITO, aguardando validação em hardware)
- [x] Sheet de ações por entrada (⋮): abrir / renomear / copiar / mover / apagar
- [x] Prompt com OSK (renomear, novo diretório) e confirmação p/ apagar
      (apaga recursivo p/ diretórios)
- [x] Copiar/mover com seletor de destino (modo picker na própria lista);
      não sobrescreve destino existente (falha com aviso no console)
- [x] Voltar/Sobe fecham overlays em camadas (prompt > sheet > picker)

## M4.6 — Cursor + Notas ler/editar (FEITO, validado em HW 2026-09-29)
- [x] Cursor piscante (500 ms, esp_timer) com glifo configurável
      `ui.cursor = "bar" | "under" | "block"` (seletor em Config > Entrada)
- [x] Modo **leitura** com markdown de bloco: `# ## ###`, bullets `-/*`,
      citações `>`, cercas de código, `---`; estilos por linha (modelo int)
- [x] Notas abrem em leitura; editor/arquivos abrem em edição; botão
      Ler/Editar alterna; em leitura, teclado/toque não editam (só rolam)
- Validação em HW achou 2 bugs → corrigidos no M4.7 abaixo.

## M4.7 — Markdown visível + cursor de slot (FEITO, validado em HW 2026-09-29)
- [x] **Bug do título em branco**: `Text` com `height:` menor que a
      line-box da fonte renderiza ZERO linhas (h1 23px em linha de 25px =
      invisível). `Theme.line-h` 25→28 px (+ espelho `ED_LINE_H` no
      main.cpp) e hierarquia real: h1=type-title 23, h2=type-subtitle 20
      (token novo), h3=type-body bold colorido.
- [x] **Bold de verdade**: faces `Roboto-Regular/Bold-Subset.ttf` embutidas
      (default-font-family: "Roboto"); antes o bold caía no Regular do
      fontconfig do host e `##` ficava idêntico ao corpo.
- [x] **Cursor ocupa o slot do caractere** (terminal-style): `exemplo` →
      `exe_plo` (antes inseria célula extra: `exe_mplo`). Block = espaço +
      overlay `Rectangle` no .slint (`█` não existe em fonte embutida);
      botão do seletor agora usa `■` (U+25A0, existe no Roboto).
- [x] md_render tolerante: `#título` sem espaço vira h1; BOM UTF-8 e CRLF
      saneados no `ed_load` (BOM na 1ª linha cegava o `# `).
- [x] Harness de render offscreen (slint-compiler 1.12.1 + Slint-cpp Linux
      + SoftwareRenderer em buffer): bugs acima provados/reproduzidos e
      fixes validados SEM hardware — ver docs/UI.md §"Render offscreen".
- [x] Nota de teste canônica: `sdcard-template/pda/notas/_indice.md`.
- [x] **Partição `factory` 4M→6M** (`partitions.csv`): o binário chegou a
      0x412d00 (~4,07 MiB) — fontes Roboto R/B embutidas + -O2 (PERF) —
      e o `check_sizes` falhou com overflow de 75 KiB. Novo mapa:
      app 6 MB @ 0x10000 + `storage` 8 MB (auto, vai para 0x610000) em
      flash de 16 MB, ~1,9 MB livres. Efeito colateral do primeiro boot
      pós-reflash: LittleFS interno em offset novo = lixo → monta falha →
      `format_if_mount_failed` formata (espelho interno zerado; NVS em
      0x9000 intacto e SD intocado — cadeia ativo→espelho→NVS→defaults
      recupera as settings).

## M4.13 — Opt-out de gestos por tela, standby manual sticky e grade de pixel do cursor (FEITO; ✅ VALIDADO EM HARDWARE 2026-10-04 — hibernate abriu o M4.13b)

Rodada de feedback de 2026-10-02: M4.12 VALIDADO em hardware (gestos e
pulldown ok), com três problemas/sugestões — todos diagnosticados com
evidência no host ANTES de ir para a placa:

- [x] **Opt-out de gestos por tela** (sugestão do usuário): propriedades
      `back-swipe-allowed` / `qs-pull-allowed` no `app_ui.slint`. Telas que
      gerenciam gestos próprios declaram `false` e a camada de gestos do app
      SAI do hit-test — o mesmo efeito que o Launcher já tinha por ordem de
      empilhamento. `AppState.Editor` (edição E leitura) opta por não ter o
      back-swipe: a margem esquerda rola a caixa de texto e o voltar fica no
      botão da toolbar. O pulldown segue global (a tira só cobre a status
      bar, que não tem controles).
- [x] **Botão "Suspender" não suspendia**: o pedido manual só zerava o
      relógio de idle; a CAUDA de eventos de toque do MESMO tap no botão
      (GT911 segue reportando por dezenas de ms após o release) religava o
      relógio via `power_mgmt_activity()` e o pedido evaporava em silêncio
      (log do usuário: pedido em t=568 s, `DIM (idle 5s)` em t=573 s — se o
      zero tivesse sobrevivido, o idle logado seria ~568 s). Correção: flag
      sticky `s_manual_standby` em `power_mgmt.c`, consumida só ao entrar em
      standby; pedido → DIM em ≤200 ms → standby no tick seguinte; toque não
      cancela mais o pedido (desistir = acordar de novo).
- [x] **Cursor de bloco derivando no fim da linha**: o software renderer do
      Slint 1.12 GRAVA o avanço do glifo na grade de pixels — pitch real
      medido = `round(0.600em × fsize)` (sonda `fontpitch` do harness:
      18px→11, 20px→12, 23px→14, min==max==média em 24 glifos idênticos;
      sonda `coltest`: overlay do bloco na col 43 a −9 px do `|` in-flow na
      mesma coluna). A fórmula antiga usava 0.6em fracionário (10.8 px) e
      acumulava 0.2 px/char. Correção: `cell = Math.round(fsize/1px*6/10)*1px`
      no `editor.slint` (MdRow + EditorScreen) e `ED_CHAR_W = 11.0f` no C++
      (espelhos). Bar/under nunca derivaram porque são glifos embutidos no
      texto. Efeito colateral bom: runs de palavra do modo leitura (MdRow)
      agora casam EXATOS com o fluxo de glifos (antes havia drift invisível
      de 0.2 px/char também lá).
- [x] Evidência: `render_coltest.png` (bloco e `|` na mesma célula),
      `render_fontpitch.png`, set completo re-renderizado
      (reading/reading2/edit/prompt/panel/paneldrag); `md_test` 24/24 e
      `hosttest` T1–T8 verdes; `slint-compiler` limpo.
- [x] Ferramentas: `tools/render_offscreen.sh` renderiza 8 modos (inclui as
      sondas `coltest`/`fontpitch`); `tools/ppm2png.py` novo (converte os
      PPM do harness em PNG na raiz do repo).
- [x] Sobre = "M4.13".
- **Aceite HW**: no painel, "Suspender" escurece em ≤0.5 s e entra em
  standby (`SUSPENDER: DIM imediato (pedido manual)` + `entrando em STANDBY
  (idle robusto)` no log, sem DIM por idle antes); no editor, arrastar a
  margem esquerda rola o texto (não volta) e o botão Voltar da toolbar
  funciona; nas demais telas o back-swipe segue igual; cursor de bloco no
  fim de uma linha longa (40+ colunas) cobre exatamente a célula do
  caractere; com prompt aberto nada muda.
- **Adiado p/ M4.14 (energia, a pedido do usuário)**: `power.wake_on_touch`
  gatear de fato o wake por toque (hoje o ISR do INT acorda sempre — a
  SwitchRow existe mas só segura o idle); standby mais profundo (pausar
  reconexão Wi-Fi e NTP durante o standby); botão BOOT como wake (ISR já
  instalado no init — validar na placa e conferir conflito de GPIO config).
- **Validação 2026-10-04**: checklist completo verde (gestos, opt-out do
  editor, cursor de bloco, Suspender sticky c/ assinatura de log exata,
  OSK/prompt/brilho do painel) — exceto o item de hibernate, que virou o
  M4.13b abaixo. Checklist preenchido fora do repo:
  `validacoes/VALIDACAO_M4.13.md` (lista descartável por versão, não entra
  no git por decisão do usuário).

## M4.13b — Sessão do hibernate restaurada no RESET (FEITO; ✅ VALIDADO EM HARDWARE 2026-10-05)
Bug achado NA validação do M4.13 (2026-10-04): "hibernar não restaura a
tela ao apertar RESET e não existe outra forma de sair do hibernate".

- [x] **Causa raiz (lida no código, não conjectura)**: `power_mgmt_hibernate()`
      entra em deep sleep SEM wake source (volta = RESET/power-on, por
      design — docs/POWER.md), mas `power_mgmt_woke_from_hibernate()`
      exigia `esp_reset_reason() == ESP_RST_DEEPSLEEP`, causa que SÓ
      ocorre em wake por wake source (inexistente). RESET gera
      `ESP_RST_PIN`/`ESP_RST_POWERON` → `session_restore_if_needed()`
      retornava na 1ª linha: restauração era código morto desde que o
      wake source saiu (kill-switch M-power, 2026-09-24). A 2ª queixa do
      relato é o design: deep sleep sem wake source só sai por
      RESET/power-on (botão/toque NÃO acordam — documentado).
- [x] **Cura**: flag `hib` em NVS (namespace novo `pdapwr`, flash interna:
      sobrevive a RESET, power-cycle e ao `storage_shutdown_sd()` que
      desmonta o SD logo depois) gravada antes do `esp_deep_sleep_start()`
      e CONSUMIDA no primeiro boot que a achar (`hibernate_flag_set/take`,
      apaga ao ler p/ um reboot comum não restaurar de novo).
      `woke_from_hibernate()` = flag consumido OU `ESP_RST_DEEPSLEEP`
      (porta aberta p/ wake sources futuros).
- [x] Evidência por log: hibernate loga `HIBERNATE: flag NVS gravado —
      volta só por RESET/power-on (sessão será restaurada no boot)`; o
      boot pós-hibernação loga `boot pós-hibernação detectado (flag
      NVS/reason)` + `restaurando sessão: app=.. note=..`; sem
      session.txt legível loga `pós-hibernação sem session.txt legível`.
- [x] Sobre = "M4.13b".
- **Aceite HW**: abrir uma nota (ou Arquivos/Config), Config → Hibernar,
      apertar RESET: boot mostra os logs acima e volta à MESMA tela/nota;
      reboot comum SEM hibernate prévio NÃO restaura (flag consumido);
      hibernate → power-cycle de bateria (se acessível) idem.
- **Nota de 2026-10-04**: o log recebido do usuário ainda era do binário
      v4.13 (compile time idêntico ao da validação) — o patch NUNCA foi
      buildado; patch aplicável em `patches/M4.13b-hibernate.patch`
      (workspace, fora do git). Usuário decidiu não bloquear nele: o
      hibernate pode ser absorvido pelo Estágio 4 do rework de energia
      (`docs/POWER_REWORK.md`), que também explica por que RESET não era
      a saída esperada na bateria (corte do IP5306 → tecla BF2).

## M4.14 — Standby profundo, wake honesto e transições assinadas (FEITO; ✅ VALIDADO EM HARDWARE 2026-10-04, rodadas 1–3; ajustes M4.14.1–M4.14.4)
Consome a fila de energia adiada no M4.13 (+ item 4, a anomalia dos logs
de validação). Patch delta sobre v4.13+M4.13b:
`patches/M4.14-energy.delta.patch` (workspace, fora do git).

- [x] **#1 `power.wake_on_touch` gateia o wake por toque**: snapshot da
      chave AO ENTRAR em standby (`s_touch_wake_ok`); com OFF a ISR do
      GT911 não dá o semáforo de wake (acidentes de bolso morrem; BOOT e
      USB seguem acordando). Log de entrada: `wake armado: boot=sim
      touch=nao|sim`.
- [x] **#2 Standby não reconecta Wi-Fi nem polla NTP**: `standby_cb`
      chama `wifi_net_pause/resume` (wifi_net.c): handler de DISCONNECTED
      com `s_paused` loga `desconectado em standby — reconexão pausada
      (M4.14)` e não retenta; relógio via `esp_sntp_stop()`/
      `sntp_restart()`. A associação VIVA é mantida de propósito
      (modem-sleep do C6 = próximo estágio, `docs/POWER.md` § estágios).
      Wake: `Wi-Fi retomado do standby` (+ `— reconectando` se caído) e
      `NTP retomado`.
- [x] **#3 BOOT como wake**: ISR de GPIO35 já existia; conferido que nada
      reconfigura o pino após o init (grep: só power_mgmt + board_config).
      Ganhou proveniência no log; validação HW no checklist.
- [x] **#4 Transições assinadas** (anomalia de 2026-10-04): toda escrita
      de `s_state` passa por `set_state(st, why)` → `pwr: X -> Y (motivo,
      idle Ns)`; wake loga a fonte (`acordou do STANDBY (wake: botao
      BOOT|toque|atividade UI/USB)`). Se o "DIM (idle 21s) pós-NTP"
      recorrer, o log nomeia o culpado — ou prova que não houve virada.
- [x] Sobre = "M4.14". Host: slint-compiler limpo, hosttest T1–T8,
      md_test 24/24.
- [x] **Ajustes da validação (v4.14.2, 2026-10-04)**: (a) semântica da
      chave separada — toque SEMPRE segura idle/acorda de DIM;
      `wake_on_touch` gateia apenas o wake do STANDBY na ISR (rótulo novo:
      "Toque acorda do STANDBY (INT GT911)"); trade-off aceito: toque no
      bolso segura o idle durante ACTIVE/DIM; (b) resume do NTP:
      `sntp_restart()` é no-op com o serviço parado (lwIP 5.5.1
      sntp.c:123) → resume usa `esp_sntp_init()`; pause/resume não
      consultam mais `esp_sntp_enabled()` (assíncrono, race); (c) BOOT não
      acorda do HIBERNATE por design (deep sleep sem wake source; GPIO35 é
      HP, wake de deep sleep só LP 0–15) — fica p/ o Estágio 4 do
      POWER.md § estágios (mod BF2→LP ou aceitar RESET/BF2 como power-on).
- [x] **M4.14.3 (2ª rodada da validação)**: toque fantasma em elemento
      interativo durante STANDBY com chave OFF — o toque chegava ao Slint
      (a integração slint-esp polla o driver sozinha), o callback do
      elemento RODAVA com a tela off (action fantasma: tocar onde estava
      o Voltar navegava) e acordava por "atividade UI/USB". Cura: proxy em
      `get_xy` do driver GT911 (`touch_init.c`): enquanto
      `power_mgmt_touch_blind()` (STANDBY && chave OFF), o Slint recebe 0
      pontos; `read_data` segue drenando o chip (sem flood de pontos
      velhos no wake). Chave ON: comportamento inalterado (toque acorda e
      o ponto que acordou atua, como antes).
- [x] **M4.14.4 (pedido da 3ª rodada, 2026-10-04)**: botão BOOT como
      toggle de standby — opt-in `power.boot_btn_standby` (SwitchRow
      "Botão BOOT entra em STANDBY (toggle)" em Config → Entrada): pressão
      NOVA em ACTIVE/DIM pede standby pelo mesmo caminho sticky do
      "Suspender"; detecção de borda (pressão longa já em curso no boot/
      strapping não dispara); em STANDBY o poll não roda (task bloqueada
      no enter_standby), então o BOOT segue sendo só wake lá. No mesmo
      commit: churn `DIM->ACTIVE->DIM` no mesmo tick (log de 2026-10-04,
      t≈348,6 s) curado — `idle_s` é recalculado após os polls, pois
      `power_mgmt_activity()` no meio do tick resetava o relógio e o
      valor do topo ficava velho.
- [x] Sobre = "M4.14.4". ✅ VALIDADO EM HARDWARE 2026-10-04 (4ª rodada,
      incl. teste com senha errada p/ exercitar o loop de reconexão).
- [x] **Build-fix v4.14.1** (achado no build do usuário, 2026-10-04):
      `sntp_start()` tinha sido inserida ANTES da definição de
      `sntp_synced()` (undeclared no IDF gcc); ordem corrigida. Lição:
      `wifi_net.c`/`power_mgmt.c` só compilam no build IDF — erro de
      declaração/order em arquivos IDF-only escapa da suíte de host.
- **Aceite HW**: `validacoes/VALIDACAO_M4.14.md` (fora do repo), que
      inclui o reteste do hibernate M4.13b (presente neste binário).

## M4.15 — Estágio 1b: vizinhança dorme junto (FEITO; ✅ VALIDADO EM HARDWARE 2026-10-05 — C6-off fica p/ hibernate v2)
Consome parte do Estágio 1b de `docs/POWER.md` (§ estágios) com evidência na mão
(issue esp-idf#18443; correntes do datasheet GT911). DFS/`esp_pm_configure`
e teardown do painel seguem ADIADOS até o Estágio 0 (medição com
amperímetro): sem número, não se otimiza.

- [x] **Retry espúrio no standby** (log da senha errada, t≈17,5 s): o
      handler de DISCONNECTED dorme 2 s dentro do event loop e chamava
      `esp_wifi_connect()` sem re-checar `s_paused` ao acordar do delay;
      agora re-checa (`standby no meio do retry — reconexão pausada
      (M4.15)`).
- [x] **GT911 dorme no STANDBY** quando o wake por toque não está armado:
      `esp_lcd_touch_enter_sleep/exit_sleep` no standby_cb (~3,5 mA →
      <50 µA); o proxy (M4.14.3/15) também finge `read_data` enquanto
      cego (sem NACK por poll no I2C). Chave ON: chip acordado (é ele que
      dá o wake).
- [-] **C6 em reset no HIBERNATE — REVERTIDO no v4.15.2**: no P4 (XIP em
      PSRAM) o `gpio_force_hold_all()` latcha os pads de MSPI/SPI e a
      entrada do deep sleep stallou: `HP_SYS_HP_WDT_RESET` (rst 0x7) no
      log do usuário (2026-10-05) + boot seguinte travado com latch
      residual. A receita da esp-idf#18443 NÃO é portável p/ esta placa;
      "C6 off no hibernate" fica ABERTO p/ o hibernate v2 (Estágio 4).
      Hibernate volta ao fluxo M4.13b (sessão + NVS + shutdown do SD).
- [x] **Debounce do BOOT (v4.15.2)**: o negedge da MESMA pressão que
      pediu o standby chegava com a placa já dormindo (wake 42 ms após o
      entry no log da 5ª rodada → ciclo off/on e "tela pisca" com
      pressões longas); wake por BOOT em até 400 ms da entrada é igno-
      rado e re-bloqueado (`bounce do BOOT ignorado (<400 ms no standby)`).
      Aceito pelo usuário (2026-10-05): pressões MUITO longas (>2 s) ainda
      podem acordar por micro-soltadas do botão (re-contato = pressão
      nova legítima) — documentado, não é bug.
- [x] **M4.15.3: restore de sessão adiado p/ o event loop**: aplicar no
      bring-up crashava (`assert xTaskToNotify==NULL` em
      `vTaskGenericNotifyGiveFromISR`: `invoke_from_event_loop` antes da
      task ui_loop existir — log do usuário hibernando pela tela de
      notas); agora o bring-up só parseia (`sessão lida: app=.. note=..`)
      e o apply roda via `invoke_from_event_loop` após a criação da
      ui_loop (`restaurando sessão: …`). Rama nova: `app=settings`
      restaura a tela Config (antes caía no launcher).
- [x] Sobre = "M4.15.3". Host: slint-compiler limpo, hosttest T1–T8,
      md_test 24/24.
- **Aceite HW**: `validacoes/VALIDACAO_M4.15.md` (lista também no chat).
      ✅ Rodada 6 (2026-10-05): restore de Config/Notas/Arquivos/Scripts/
      Redes sem crash (`sessão lida…` no bring-up → `restaurando sessão…`
      no event loop); deep sleep limpo (sem banner WDT); BOOT longo não
      cicla mais; loop de reconexão fora de casa = by design (backoff
      vira candidato da revisão pré-M5).

## M4.16 — OSK: alvos ≥48 px + fileira de números fixa (FEITO; aguardando hardware)
Wishlist prioridade 2 do usuário (2026-09-30).

- [x] Fileira de números FIXA (1–0) no topo, 40 px (fileira auxiliar); as
      4 fileiras de baixo sobem p/ ≥48 px (`Theme.osk-h` 193→272 px).
- [x] Mais espaçamento (5→6 px; padding 6→8 px) e teclas largas maiores:
      setas/ok 44→56 px, SHIFT/BACKSPACE/MODE 64→76 px, enter 70→84 px.
- [x] Prompt overlay sem colisão: campo em y 40–160 px; OSK começa em
      y≈204 px (480−272−4).
- [x] Sem mudança de charset (dígitos já embutidos desde o M4.8) → fontes
      e partição intocadas.
- [x] **M4.16.1 (feedback HW 2026-10-05)**: fileira de números fixa
      REMOVIDA (cobria tela do editor; dígitos seguem no modo 123) —
      osk-h 272→226 px, mantidos teclas ≥48 px e spacing 6 px (aprovados);
      diálogo do prompt com texto 120→180 px p/ OK/Cancelar não sumirem
      acima do OSK (y≈250); **repetição ao segurar** (sugestão do usuário):
      BACKSPACE/setas/SPACE repetem após 400 ms e depois a cada 90 ms
      (fire-on-down nas repetíveis; release não acrescenta pulso);
      TouchArea 1.12 não tem pressed/released → property-espelho +
      `changed`, e Timer se controla por `running` (`triggered` é o cb).
- [x] Sobre = "M4.16.1"; slint-compiler limpo.
- [x] **M4.16.2 (feedback HW + render offscreen, 2026-10-05)**: botões
      OK/Cancelar do prompt REALMENTE visíveis. Causa raiz (provada por
      bisseção com `render_prompt.png`): no Slint 1.12, uma fileira
      colocada DEPOIS de um irmão de altura fixa dentro do
      VerticalLayout do diálogo era mispositioned para fora do diálogo
      (sob o OSK, z-order maior); com a row no TOPO do layout, renderiza.
      Cura: diálogo compacto (156 px c/ texto, 110 px sem) e row de
      botões em geometria ABSOLUTA (x/y/w/h explícitos) dentro do
      diálogo — imune ao bug de layout. Estreante: harness de render
      usado como juiz antes do hardware (pedido do usuário).
- [x] Sobre = "M4.16.2". ✅ VALIDADO EM HARDWARE 2026-10-05 (7ª rodada):
      prompts de renomear/novo arq/novo dir/senha Wi-Fi/apagar com
      OK/Cancelar visíveis e funcionais; repetição e taps ok. Wishlist
      prio 2 FECHADA; itálico (prio 3) adiado por decisão do usuário.
- **Aceite HW**: prompt de renomear/senha Wi-Fi mostra OK (filled) e
      Cancelar (ghost) acima do OSK; confirmação sem texto (apagar)
      idem centrada; OK confirma, Cancelar aborta; repetição e tap
      simples seguem ok. Cobre também o aceite da M4.16.1 (teclas
      maiores, repetição, espaço do editor), cujo item 3 (prompt) era
      este bug.
- **Fila da revisão pré-M5**: auditar outros layouts com irmãos de
      altura fixa + condicionais (mesma família de bug); eco do clamp
      no slider; backoff Wi-Fi fora de casa; mtime/RTC sem NTP.
- **Itálico (wishlist prio 3)**: ADIADO por decisão do usuário
      (2026-10-05: "não é prioridade agora; se fácil, no fim da
      versão") — e NÃO é fácil (pipeline de fonte + 3ª família + kinds
      no md_render + runs no editor + orçamento de flash); fica p/ a
      versão seguinte, não entra por debaixo do tapete.

## M4.12 — Gestos + pulldown de ajustes rápidos (FEITO; ✅ VALIDADO EM HARDWARE 2026-10-02 — "Suspender" corrigido no M4.13)
PRIORIDADES 1 e 5 do usuário (2026-09-30) entregues juntas, porque "pulldown
É um gesto". Implementação 100% em Slint + 2 callbacks novos no C++
(`qs-brightness`, `qs-state`); navegação do painel reusa callbacks existentes
(`app-back`/`fm-back`, `cfg-open-networks`, `cfg-sleep-now`,
`open-app-dispatch("Config")`).

- [x] Pulldown da status bar com drag-following (abre com >1/3 da altura,
      recolhe abaixo disso; fecha por grip/scrim/atalho).
- [x] Painel: brilho AO VIVO (backlight por tick, `system.lua` gravado 1x no
      fechamento), linha de rede viva (SSID + rssi), atalhos
      Redes/Config/Suspender, todos os controles ≥48 px.
- [x] Gesto de voltar: arrasto da borda esquerda (>36 px, dominante
      horizontal) em toda tela menos o launcher; no gerenciador usa
      `fm-back()` (folha/picker/subdir antes de sair do app).
- [x] Sem mudança de charset/glifos (painel só texto) → sem tocar em fonte
      nem partição; Sobre = "M4.12".
- [x] Evidência offscreen: `render_panel.png` (aberto) e
      `render_paneldrag.png` (meio arrastado, 110 px) — modos novos do
      harness via `qs-dragging`+`qs-drag` (duração de animação 0 ms no drag).
- [x] Regressões de host verdes: `md_test` 24/24, `hosttest` T1–T8,
      `slint-compiler` limpo (2 erros pegos no host antes do hardware:
      percent-to-length e assign em property `in` → `in-out`).
- **Aceite HW**: arrastar a status bar p/ baixo abre o painel seguindo o
      dedo; soltar no meio recolhe; brilho do painel muda na hora e sobrevive
      ao reboot (`[quick] brilho persistido: N` no log ao fechar); borda
      esquerda volta do editor/notes/config; com prompt aberto (senha Wi-Fi)
      nenhum gesto dispara.

## M4.11b — Causa raiz do INT_MAX: ABI do espressif/lua (FEITO; ✅ VALIDADO EM HARDWARE 2026-09-30)
**Veredito do boot de aceite:** `parser self-test: OK` · `parse ... fnv=1aeaac5f
-> br=1 dim=5 off=120 deep=0` (valores reais, não INT_MAX) · primeira aparição
de `system.lua intacto — nenhuma regravação no boot` · cursor sobreviveu ao
reboot · `pda.settings.get("display.brightness")` → `30.0` (numérico correto
via fronteira Lua). Standby robusto dormiu/acordou limpo; NTP ressincronizou
após ~1h de uptime. **O caso INT_MAX está encerrado.** (`br=1` no boot = a
sombra NVS da sessão anterior funcionando como projetada; ajustado p/ 30 na UI
durante a sessão.)
O boot de campo do M4.11 (2026-09-30, 2 boots completos) entregou a
evidência que faltava: **`parser self-test: FALHOU`** — o parser não lia
nem um chunk constante gerado na RAM. Combinado com o que o MESMO log
mostrou funcionando (`save_persists` via `lua_tointeger`, `wifi.lua` via
`lua_tostring`/`lua_toboolean`, dump/censo íntegros, FNV estável entre 2
leituras = mídia OK), sobrou uma única fronteira quebrada: **`lua_Number`**.

- [x] **Diagnóstico (3 fontes)**: `espressif/lua` 5.5.0 compila a VM com
      `LUA_32BITS=1` **PRIVATE** (`idf-extra-components/lua/CMakeLists.txt`;
      bug público: espressif/developer-portal discussion #188, comentário
      de 2026-07-24) → VM `float`/`int32` vs. `main/` compilado com
      `double`/`int64` dos headers. `lua_tonumber` devolve float nos 32
      bits baixos de `fa0`; lido como double vira lixo finito → `(int)`
      satura em INT_MAX no RISC-V. `lua_tointeger` sobrevivia por sorte
      (psABI RISC-V sign-extends int32 em `a0`). Lua 5.5.0 oficial confere:
      hook `#if defined(LUA_32BITS)` presente no `luaconf.h`. Detalhes em
      `docs/HARDWARE.md` (achado #9). **Cartão SD exonerado.**
- [x] **Cura**: `target_compile_definitions(main PRIVATE LUA_32BITS=1)` no
      `main/CMakeLists.txt` + `_Static_assert(sizeof(lua_Number)==4 &&
      sizeof(lua_Integer)==4)` no `pda_config.c` (guarda o build p/ sempre).
      `lua_runtime.c` (`pda.settings.get/set` de scripts) cura junto.
- [x] **Defesa em profundidade**: `tbl_int` integer-first
      (`lua_tointegerx`; só frações reais caem no `lua_tonumber`).
- [x] **Self-test com autópsia**: em caso de falha loga os 2 lados
      (veio br=.. cursor=.. | esperado br=.. cursor=..) — nunca mais um
      "FALHOU" mudo.
- [x] **Bug do cursor que não persistia (relato do usuário)**: 2 causas,
      ambas corrigidas — (a) `on_cfg_set_cursor` só marcava dirty em RAM,
      agora chama `pda_config_save()` na hora; (b) a sombra NVS não tinha a
      chave `cursor`, então toda cura de boot apagava a preferência →
      `nvs_set_str/get_str "cursor"` adicionados (opcional no load: NVS
      antigo sem a chave não invalida a sombra).
- [x] **Testes de host estendidos** (`tools/hosttest/run.sh`): T6 floats
      (42.7→42, 42.0→42, "55"→55), T7 round-trip do cursor pelo arquivo,
      T8 sombra NVS devolve brightness E cursor — **T1–T8 passando** +
      `md_test` 24/24.
- **Aceite HW**: boot com `parser self-test: OK`, `parse ... -> br=25
  dim=5 off=120 deep=0` (valores do arquivo, não INT_MAX), primeira
  aparição de `system.lua intacto — nenhuma regravação no boot`; mudar o
  cursor na tela Config → reboot → cursor mantido; Configurações → Sobre =
  "M4.11b".

## M4.11 — Listas aninhadas + cerco ao parse do system.lua (FEITO; veredito do parse-buffer superado pelo M4.11b)
Feedback do usuário pós-M4.10 (markdown ok; warnings do pda_config no boot;
itens de lista recuados não renderizavam) + log de boot já com censo/dump:

- [x] **Mistério do system.lua — veredito do log de 2026-09-30**: o dump
      provou conteúdo VÁLIDO no cartão (`brightness = 30`, 790 B, entrada
      única, mtime recente), mas o parse devolveu INT_MAX em tudo com
      `parse=ok`. A hipótese da época (divergência entre o `fopen` do
      `luaL_loadfile` e a leitura auditada) motivou a troca p/ parse de
      buffer — mas o boot de campo com o parse-buffer AINDA deu INT_MAX e
      o self-test FALHOU → a causa real era outra: **ABI do espressif/lua
      (`LUA_32BITS` PRIVATE), resolvida no M4.11b**. O parse-buffer fica
      de pé como melhoria correta (parse e auditoria veem os mesmos bytes).
- [x] **Detecção de mídia instável**: segunda leitura imediata + FNV-1a;
      diferiu → `E MÍDIA INSTÁVEL: ... mudou entre 2 leituras — cartão
      suspeito` (evidência definitiva p/ cartão falso/cansado; teste T5).
- [x] **Self-test do parser no boot**: serializa as configs em RAM e
      re-parseia (`parser self-test: OK/FALHOU`) — separa "parse quebrado/
      antigo no build" de "conteúdo insano na mídia".
- [x] **Diagnóstico por boot**: `parse <path>: N B fnv=xxxxxxxx -> br=..
      dim=.. off=.. deep=..` (INFO) + WARN nomeando seção ausente
      (display/power/locale/ui) quando o chunk não bate com o formato.
- [x] **Bateria de testes de host** `tools/hosttest/run.sh`: compila o
      `pda_config.c` REAL no x86 (stubs ESP + Lua 5.4 do sistema); 5
      cenários (cartão vazio, round-trip de usuário, INT_MAX literal,
      sintaxe inválida, mídia instável) — todos passando.
- [x] **Listas aninhadas** (feedback de baixa prioridade): `md_render.h`
      reconhece `  - item` (2 espaços = 1 nível, teto 3 níveis = 6 células);
      marcador `•` na coluna do nível; dentro de cerca ``` o recuo continua
      código. Teste `tools/md_test.cpp`: 24 asserções (aninhamento +
      regressões da M4.10) passando. `_indice.md` com exemplos novos.
- [x] **Pesquisa Bluetooth** (`docs/BLUETOOTH.md`): BLE com telefone =
      viável (C6 + ESP-Hosted HCI/SDIO + NimBLE, sem reflash do C6);
      A2DP/fone BT = impossível (C6 é BLE-only; LE Audio não existe no IDF
      p/ C6); áudio = alto-falante no conector da placa (ES8311+NS4150,
      falta só o falante) ou fone/DAC USB-C (`usb_host_uac`).
- Próximo (wishlist do usuário, ordem dele): gestos + pulldown de ajustes
      rápidos (M4.12), alvo de toque do OSK, itálico, M3 Expressive.

## M4.10 — Markdown de leitura: h1, inline, réguas e crase (FEITO; validação HW pendente)
Feedback do usuário pós-M4.9 (4 sintomas, todos reproduzidos no harness
offscreen ANTES de corrigir — `render_reading.png`/`render_reading2.png`):
- [x] **h1 invisível de novo**: line-box do PDA Mono = 1.3188em (hhea
      2146/-555 @ upem 2048) → h1 23px precisa de 30.3px e a fileira tem
      28px; `Text` com height < line-box renderiza ZERO linhas no 1.12.
      Corrigido no `MdRow` (editor.slint): p/ fsize >= 22px o Text ganha
      height 34px centrado (y -3px) — a fileira continua 28px p/ o scroll.
- [x] **linha em branco virava régua**: estilo 6 era "hr OU vazio"; agora
      vazio = estilo 9 (nada desenha) e 6 é SÓ `---`/`***`.
- [x] **crase/backtick em branco**: o set ASCII embutido no slint-compiler
      1.12 (`embed_glyphs.rs`) cobre 0x20-0x7E MENOS o 0x60 — sem ele em um
      literal estático, o glifo não é pré-rasterizado. `` ` `` entrou na
      âncora de `app_ui.slint`; a tecla do OSK (123, fileira 3) que já
      existia mas renderizava o rótulo em branco agora aparece, e o backtick
      também foi para a camada 123 sem shift.
- [x] **negrito inline não renderizava** (markdown era só de bloco): parser
      novo em `main/md_render.h` (header sem dependências, incluído pelo
      firmware E pelo harness — o PNG prova o parser real) com runs por
      linha: `**x**`/`__x__` = negrito, `` `x` `` = código verde; marcador
      órfão vira literal. Modelo de modelos `[[MdRun]]` (text/col/kind);
      cada run é um `Text` posicionado por coluna mono (avanço 0.600em),
      sem HorizontalLayout. Itálico segue pendente (precisa de face
      oblíqua — UI_WISHLIST).
- [x] **quirk do renderer descoberto no caminho**: dentro de um `Text`
      longo, o glifo de espaço pré-rasterizado às vezes avança 2 células
      (medido em pixel: linha deslizava +1 e comia o vão do run seguinte).
      Contrato novo: nenhum run desenhado contém espaço — o parser quebra
      em palavras e os vãos vêm das colunas (células em branco).
- [x] Evidência: `render_reading.png` (topo), `render_reading2.png` (lista/
      citação/cerca/régua), `render_reading_tail.png` (negrito inline,
      `código inline`, crase sozinha e h1 tolerante no fim do doc).
- [x] Censo do system.lua: `census_fat()` chamava `f_opendir()` com path de
      VFS ("/sdcard/...") e falhava SEMPRE (log "f_opendir falhou" no boot
      do usuário). Virou `census_dir()` com `opendir/readdir` (funciona nas
      duas raízes, FAT e LittleFS) + `dump_head()` que loga os primeiros
      bytes do arquivo quando os valores vêm insanos — é a evidência que
      separa "arquivo realmente corrompido no meio físico" de "parse
      enlouqueceu". Próximo boot do usuário fecha o caso.

## M4.9 — Peso 400 + pipeline de fontes genérico (FEITO; validado HW em 2026-09-29)
Feedback do usuário no M4.8: "legível mas seria bom aumentar um pouco mais o
peso" + "como funciona o script de fontes / posso trocar por Fira Code?".
- [x] Base **wght 300 → 400** (`--weight 400`); bold segue 700. Render
      offscreen confirma corpo maior sem perder o ar mono
      (`render_reading/edit/prompt.png`).
- [x] **Família interna fixa `PDA Mono`** (name-table 1/2/4/6/16/17): trocar a
      fonte de origem não toca em nenhum `.slint`/C++. Assets renomeados p/
      `Mono-Base.ttf`/`Mono-Bold.ttf`; faces legacy M4.7/M4.8 removidas.
- [x] **Pipeline genérico** (`tools/font_pipeline.py`): `--mono/--bold-src/
      --weight/--bold-weight/--family/--license/--license-url`, colheita de
      charset dos `.slint` (com `strip_comments`), **corte de ligaturas**
      (`calt/liga/dlig/clig` — proteger o cursor de slot), validação em dois
      baldes (crítico=UI → ERRO; opcional=pisos → aviso) + licença com
      proveniência (`assets/LICENSE-mono.txt`).
- [x] **Fira Code 400/700 testado como drop-in**: avanço 0.600em idêntico,
      cobertura 99,2%, `calt` removido; comparativo em
      `render_comparo_fontes.png`. Não embarcado (decisão: seguir Roboto Mono,
      Apache-2.0, já licenciada no repo).
- [x] `docs/FONTES.md` novo: funcionamento do pipeline, receitas de peso/troca,
      contratos (0.600em, ligaturas, âncora, licença), quirk de path.
- [x] `tools/render_offscreen.sh`: harness reproduzível no host (3 modos →
      PNG na raiz).
- [x] Diagnóstico do "system.lua volta insano": `census_fat()` em
      `pda_config.c` (f_readdir mostra TODAS as entradas do dir, com size/data
      de cada uma) + `purge_path()` agora loga deletes/falhas. Próximo boot do
      HW dirá se é duplicata FAT ou escrita externa.

## M4.8 — Mono leve na UI inteira + prompt com cursor (FEITO, validado em HW 2026-09-29/30)
Pedido do usuário após validar o M4.7: "fonte monoespaçada mais leve em
toda a interface".
- [x] **Roboto Mono Light (wght 300) como default da app inteira**
      (`default-font-family: "Roboto Mono"`): a face Light é instanciada do
      `RobotoMono[wght].ttf` e RENOMEADA p/ família "Roboto Mono" — o match
      de runtime do Slint (`min_by_key(weight.abs_diff(400))`) escolhe Light
      (300) p/ texto normal e Bold (700) p/ `font-weight: 700`. Faces
      proporcionais Roboto-Regular/Bold saíram dos imports (binário cai
      ~0,6 MB; gen do slint-compiler 10,8→7,3 MB). Arquivos ficam em
      assets/ sem import p/ rollback.
- [x] Pipeline reproduzível: `tools/font_pipeline.py` (instancer wght 300/
      700 + rename name-table 1/2/16/17 + pyftsubset + validação de família/
      peso/avanço 0.600em/cobertura). Charset idêntico ao anterior + `≠ ≈ ∞`
      (página 123 do OSK envia via C++ e NÃO estavam na âncora — glifos
      ausentes latentes).
- [x] **Prompt com cursor de verdade** (rename/novo arq/novo dir/senha
      Wi-Fi): antes `SPACE` caía no else e era anexado LITERAL (e setas
      mortas). Agora `g_fm.prompt_pos` + edição UTF-8 (BACKSPACE/DEL no
      codepoint, ←/→/HOME/END movem o caret, SPACE insere espaço) e o
      overlay desenha `pre | caret(2px) | suf` (props `fm-prompt-pre/suf`).
- [x] **Notas abre qualquer texto** (bug: `.md` listava mas o open_note
      concatenava ".txt"): mapa display→nome real (`s_notes_real`), `.md`
      abre em modo leitura (paridade com o FM), não-texto logado e
      ignorado; lista agora ordenada alfabeticamente.
- [x] `■` (U+25A0) não existe na Roboto Mono → botões do cursor no
      settings viraram rótulos "barra/traço/bloco" (charset fechado).
- [x] Âncora de rasterização revisada: charset GLOBAL (vale p/ todas as
      faces/sizes), `█` removido (não existe em face alguma).
- [x] Harness offscreen ganhou modo `prompt` (render_prompt.png na raiz).

## M3c — Pendentes do M3
- [ ] Hot-plug do SD (remontagem a quente + aviso na status bar)
- [ ] Virtualização do editor (arquivos com milhares de linhas)

## M3 — Arquivos avançados
- Hot-plug do SD (remontar sem reboot) + aviso na status bar
- Copiar/mover/renomear/apagar com confirmação; novo diretório
- Visualizador hex/imagem básica; ordenação e tamanho visível
- **Aceite**: trocar o cartão com o aparelho ligado e continuar navegando.

## M4a — Wi-Fi STA + NTP (FEITO, aguardando flash do C6 + validação)
- [x] `espressif/esp_hosted ==2.12.9` + `esp_wifi_remote` (par do slave 2.12.9)
- [x] `main/wifi_net.c`: STA + reconexão automática + SNTP + relógio HH:MM
      local na status bar (uptime até sincronizar) + RSSI na status bar
- [x] `config/wifi.lua` (ssid/password/auto_connect); sem arquivo = offline
- [x] docs/WIFI.md: procedimento do C6, limitação standby×SDIO, segurança
- [ ] M4b: tela de redes/scan + Bluetooth

## M4.5 — Revamp de UI (FEITO, validação pendente)
- [x] Config em **abas com sidebar de ícones** (Tela/Sistema/Entrada/Redes/Sobre)
      — resolve o overflow que escondia o 3o checkbox
- [x] **Switches** no lugar de CheckBoxes; readouts dos sliders com
      `Math.round` (sem casas decimais)
- [x] Launcher em **grade 3x3 de tiles** com ícone + legenda
- [x] Editor com **fonte monoespaçada** (Roboto Mono subset, Apache-2.0)
- [x] Aba Redes com status + atalho p/ tela de redes; aba Sobre com versões
- [ ] Wishlist remanescente: cursor piscante, leitor markdown, notas
      ler-vs-editar (=> M6)

## M4 — Conectividade (Wi-Fi/BT via C6)
- Reflash do C6 (ESP-Hosted slave 2.12.x) — `tools/flash_c6_wifi.sh`
- `espressif/esp_hosted` no P4; STA + DHCP; NTP → relógio real na status bar
- Config de redes em Lua (`config/wifi.lua`) + tela de redes
- BT: BLE via C6 (NimBLE, HCI sobre SDIO do ESP-Hosted) — pesquisa completa
  em `docs/BLUETOOTH.md`; A2DP impossível (C6 é BLE-only, sem LE Audio no IDF)
- **App companheiro Android** (ideia do usuário, 2026-09-30): sync de notas
  e dados como um smartwatch. Design faseado em `docs/ANDROID_SYNC.md`
  (Wi-Fi REST p/ volume + BLE GATT p/ presença; alternativa sem app:
  WebDAV no PDA + cliente pronto no telefone).
- **Aceite**: hora certa após boot com Wi-Fi salvo; `pda.net.*` mínimo p/ scripts.

## M5 — Áudio & player de música

## Roadmap do áudio (definido 2026-10-07, base validada com QCY H3S UAC1)

### M5a.2 — Controles e robustez do player (ABERTO — próximo)
- [x] **Stop/next imediatos** (v5.5, bug da 14ª rodada): stop por flag
      direta (fila só entre faixas); play com faixa tocando para a
      atual antes; scan de alt settings pula a alt 0 zero-bandwidth
      (mata os `Invalid alt setting` do driver).
- [x] Pause real: `uac_host_device_suspend/resume` (posição =
      offset do arquivo; loop de write espera pausado) + botão
      Pausar/Seguir na tela Música (v5.6).
- [x] Volume/mute (v5.8): teclas vol±/mute do H3S chamam
      `uac_host_device_set_volume/set_mute` em task descartável
      (timeout de ~5 s em device sem feature unit não congela UI nem
      stream); feature-detect: 1º set com erro marca `s_vol_bad` e
      silencia; auto-aplica no start só após 1 set ok. Slider na tela +
      persistência por VID:PID seguem ADIADOS (M5a.2.x).
- [x] Media keys HID consumer control (v5.6) + mapa vendor Jieli/QCY
      (v5.8: codes 08/01/02 de 2 B = play-pause/vol+/vol−, evidência do
      log da 16ª rodada; long-press next/prev é interno do fone): reports curtos
      (2–3 B) com usages HUT 0x0C (B5/B6/B7/CD/E9/EA/E2) viram comandos
      do player via `usb_hid_keyboard_set_media_cb`; boot keyboard (8 B)
      intocado; vale p/ H3S E p/ teclados com interface de mídia.
      Volume/mute: ação loga "sem suporte" até o feature-detect entrar.
- [ ] Repetir/aleatório mínimo (off/todas/uma) na tela Música.
- Aceite: Parar/Próxima/pause imediatos; tecla do fone controla o PDA;
  sem E-lines do driver no play.

### M5a.3 — Hub USB (teclado + DAC juntos)
- [ ] `CONFIG_USB_HOST_HUBS_SUPPORTED=y` (+ multi-level off); testes de
  hot-plug em cadeia (hub→teclado+DAC), enumeração lenta, HWM das tasks
  USB; regressão do quirk de primeira enumeração.
- Aceite: teclado digita E música toca pelo hub, plug/unplug em
  qualquer ordem sem crash.

### M5b — Acervo de verdade: MP3 + playlists
- [ ] Decoder MP3 fixed-point (libhelix via componente; sem FPU-heavy)
  em task própria alimentando o mesmo sink UAC (abstração `audio_sink`
  p/ não duplicar player por formato).
- [ ] Playlists `.m3u` em `/pda/music/` + fila da tela Música; shuffle
  entra aqui se não coube na M5a.2.
- [ ] Metadata mínima (nome de arquivo basta p/ round 1; ID3 v2 depois).
- Aceite: álbum MP3 do cartão toca com tela em standby (standby robusto
  não mata áudio — aceite herdado do M5 original).

### M5c — Saída local ES8311/I2S (quando o alto-falante JST chegar)
- [ ] Bring-up I2S + ES8311 + NS4150 (PA_EN): beep de UI primeiro (prova
  do caminho), depois o mesmo `audio_sink` do M5b.
- [ ] Seletor de saída em Config → Tela/Sistema: UAC | interno | auto
  (UAC se presente); mix de decisão documentado em USB.md/HARDWARE.md.
- [ ] Conector de bateria: validação de carga/descarga com áudio tocando
  (corrente do PA vs. IP5306 — limiar de auto-desligue!).
- Aceite: mesma música alternando saídas sem restart; alto-falante com
  bateria e USB desplugado.

### M5d — Integrações (cola com M6)
- [ ] Alarme/agenda (M6) tocando via sink local; wake por alarme com
  áudio; ducking simples de volume.
- [ ] Lua API `pda.audio.*` (play/stop/volume) p/ scripts do cartão.


### M5a.1 — Áudio USB: UAC + player WAV + tela Música (FEITO; aguardando hardware)
- [x] `espressif/usb_host_uac ==1.5.0` como 2º client da Host Library
      (HID segue 1º; install único na usb_host_lib_task).
- [x] `main/audio_uac.[ch]`: install com retry; open/alt-param/start/
      write/stop/close; disconnect → parada limpa; nome do produto p/ UI.
- [x] WAV PCM16 1–2 ch de `/pda/music/`; fmt não suportado = erro na
      tela, sem crash.
- [x] Tela Música (lista + Saída/Faixa/Estado + Parar/Próxima/Att/
      Voltar); tile do launcher habilitado; AppState::Music; sessão
      "music"; evidência `render_music.png`.
- [x] Sobre = "M5a.1"; slint limpo, hosttest T1–T8, md_test 24/24.
- [x] **M5a.1.1**: `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`
      256→1024 (QCY H3s composite morria em CHECK_SHORT_CONFIG_DESC);
      warning deprecated do slint-esp aceito e documentado
      (DEPENDENCIAS.md).
- [x] **M5a.1.2**: H3S é UAC1 (hipótese UAC2 inicial corrigida);
      crash no plug era reentrância nossa (open/close no callback do
      driver) → v5.4.4 difere p/ task `au_evt`; volume-query timeout do
      H3S aceito como não-fatal; tons de teste 44k1 E 48k no template.
- **Aceite HW**: DAC/fone USB-C **UAC1** (sem hub): `speaker UAC encontrado` +
      `UAC device: "…"` no log; tocar/parar/trocar faixa com som real;
      unplugar tocando → "sem dispositivo" sem crash; replugar → toca;
      WAV 24 bits → estado de erro; standby durante música → áudio
      continua; teclado sozinho segue ok.
- **Limitações round 2 (M5a.2)**: hub (teclado+DAC juntos), pause,
      teclas de mídia, volume; M5b: MP3/playlists.

### M5.0 — Passe M3 Expressive (FEITO; aguardando hardware; abre a M5 por pedido do usuário)
Wishlist prioridade 4 (2026-09-30), escopo adaptado ao painel/fontes mono.

- [x] **Cor dinâmica**: `ui.accent` (cyan|violet|green|amber|pink) em
      system.lua + sombra NVS + seletor em Config → Tela (5 botões,
      padrão do seletor de cursor). Paletas computadas em `theme.slint`
      (ternárias sobre `Theme.accent`); propagação
      `AppWindow.cfg-accent` → `init`/`changed` → `Theme.accent`.
      Limitação documentada: Slint 1.12 NÃO expõe globals ao C++
      (`global<Theme>()` não existe no código gerado) e o harness
      offscreen não roda event loop → cor dinâmica é validável SÓ em
      hardware; sanitize de valor inválido → cyan (hosttest cobre parse).
- [x] **Formas maiores**: shape-xs/sm/md/lg/xl = 6/12/18/24/40 px
      (botões viram pill, diálogos mais redondos).
- [x] **Movimento**: OSK do editor sempre montado com altura animada
      (170 ms ease-out, reveal clipado — sem trocar o layout que
      encolhe o texto); quick-panel já tinha drag-following (M4.12).
- [x] **Deferido**: tipografia expressiva (tamanhos novos de glifo
      custam flash; a escala segue trocável offline via
      `tools/scale_type.py`).
- [x] Evidência offscreen: `render_edit.png`/`render_reading.png`
      (formas pill, hierarquia md intacta, OSK height-0 sem phantom);
      slint-compiler limpo, hosttest T1–T8, md_test 24/24.
- [x] Sobre = "M5.0".
- **Aceite HW**: Config → Tela → Acento: tocar violeta/verde/âmbar/rosa
      recolore NA HORA (botões, status bar, cursor, containers); reboot
      mantém (system.lua + NVS); `accent = "foo"` no cartão → cyan sem
      crash; OSK abre/fecha com slide ~170 ms (sem pop); shapes novos
      sem overflow em toolbar/prompt/painel/launcher.
      ✅ VALIDADO EM HARDWARE 2026-10-05 (8ª rodada): 6/6 itens, incl.
      recolor imediato, persistência no reboot e animação do OSK.

### M5.0b — Quick wins da revisão pré-M5 (FEITO; aguardando hardware)
Itens A1–A5 da revisão pré-M5 (fundida em USB.md §M5a, ARCHITECTURE.md §princípios e ROADMAP M5.0b):

- [x] **A1 backoff Wi-Fi não-bloqueante**: retry sai do event handler
      (que bloqueava o event loop 2 s por retry) p/ task `wifi_rcn` com
      notificação; escada 2/4/8/16/30 s; zera no GOT_IP e no resume;
      re-checa pause/auth no fim do backoff. Reason de auth
      (AUTH_FAIL/HANDSHAKE/4WAY) **suspende** o loop automático com
      `AUTH_FAIL (reason N): senha errada?…` até ação em Redes
      (`wifi_net_connect` religa).
- [x] **A2 eco do clamp**: após save de Config, os valores REAIS
      (clamp_all) voltam aos sliders (caso real: deep 40→66 invisível).
- [x] **A3 seed de RTC**: último UTC bom persistido no NVS
      (`pdawifi/last_utc`) a cada sync; `wifi_net_seed_clock()` no boot
      (após nvs_flash_init) semeia settimeofday sem rede — mata mtime
      FAT 1980 e relógio epocal; NTP refina depois.
- [x] **A4 HWM de pilhas**: `hwm[boot+60s]`/`hwm[standby] <task>: N B
      livres no mínimo` p/ main/ui_loop/pda_power/wifi_net/wifi_rcn/
      io_*/lua_script (stack-creep sem debugger).
- [x] **A5 tela Redes auditada**: modo `networks` novo no harness
      offscreen (`render_networks.png`): lista/ícones/rssi/SSID longo ok,
      botões pill; suspeita de layout Slint 1.12 em `networks.slint:56`
      descartada no render. Validação HW = abrir a tela com scan real.
- [x] Sobre = "M5.0b". Host: slint limpo, hosttest T1–T8, md_test 24/24.
- **Validação 9ª rodada (2026-10-05), parcial**: A1 backoff observado
  (com o dente 32→30, corrigido no v5.2 p/ min(2x,30)); AUTH_FAIL
  suspende certeiro (reason 202); A3 `relógio semeado do NVS` no boot
  sem rede; A4 linhas `hwm[boot+60s]` presentes (ui_loop 25040 B livres,
  pda_power 4308, wifi_rcn 3784 — baseline); A2/A5 pendentes desta
  rodada. Crash reportado era o GT911 mudo no boot → M5.1.1 abaixo.

### M5.1.1 — GT911 retry no boot (FEITO; aguardando hardware)
- [x] Crash de 2026-10-05: warm reset → GT911 NACK no 1º I2C →
      `ESP_ERROR_CHECK(board_touch_init)` abortava o boot. Agora
      `board_touch_init` tenta 5× (80 ms entre; `i2c_master_bus_reset`
      a partir da 3ª) antes de falhar fatal.

### M5.3 — Gerenciador scan-driven + chave de rádio (FEITO; aguardando hardware)
Redesenho pedido pelo usuário na validação da v5.2 (rotação ansiosa
marcava rede BOA por AUTH_FAIL espúrio — log de 2026-10-05/06):
- [x] Fluxo Android-like: scan → melhor salva presente (RSSI) → até 3
      tentativas na mesma rede → rota → espera sem martelo se nenhuma
      presente (`nenhuma rede salva presente — Wi-Fi em espera…`).
- [x] AUTH_FAIL só marca a rede após 3 hits (`tentativa n/3` no log);
      NO_AP_FOUND roda o scan já; resume/toggle/connect/rescan limpam o
      ciclo.
- [x] Chave `net.wifi_enabled` (system.lua + NVS `wifion`) + SwitchRow
      "Wi-Fi ligado (radio)" em Config → Redes; OFF = esp_wifi_stop;
      ON = start + fluxo de scan; conectar por Redes liga o rádio.
- [x] hosttest T1–T8 (pegou buffer 1024→1536 do serialize e ordem de
      argumentos da seção net — self-test FALHOU no host ANTES do HW,
      como projetado); slint limpo; md_test 24/24; cheque uso-vs-definição
      sem alertas.
- [x] Sobre = "M5.3".
- [x] **M5.3.2 (11ª rodada)**: SwitchRow do rádio com efeito/persistência
      IMEDIATOS (`changed wifi-on =>` → handler que salva e liga/desliga;
      antes a propriedade só valeria no botão Salvar e "sumia" ao sair da
      tela); scan com rádio OFF falha limpo (`ESP_ERR_INVALID_STATE`) e a
      tela Redes avisa `[wifi] rádio desligado — ligue em Config > Redes`
      em vez de RPC 12290 do hosted; conectar por Redes com rádio OFF
      sincroniza o switch (`set_cfg_wifi_on(true)`); pilha da `wifi_rcn`
      4096→6144 (HWM de 1960 B livres durante scan).
- [x] Sobre = "M5.3.2".
- [x] **M5a.1.3**: pilha do `au_play` 6144 int → 10240 PSRAM (stack
      protection fault no 1º play, log 2026-10-07); `au_evt` 6144 PSRAM;
      ambas no HWM.
- **Aceite HW**: AP atual desligado → 3 tentativas? NÃO: NO_AP_FOUND roda
      direto p/ scan e troca (log `escolhida por scan`); senha errada
      proposital → `tentativa 1/3…2/3…3/3` e só então marca/rota; AUTH_FAIL
      isolado em rede boa → retenta a MESMA e conecta; todas ausentes →
      linha de espera única, sem loop; chave OFF some com o rádio
      (status offline, sem eventos), ON reconecta via scan; reboot mantém
      a chave.

### M5.2 — Multi-redes salvas (FEITO; ✅ VALIDADO EM HARDWARE 2026-10-06, 10ª rodada)
- [x] `config/wifi.lua` com lista `networks = { {ssid,password}, … }`
      (máx. 8; legado de ssid único ainda lido); serialização da lista
      no save; upsert ao conectar pela tela.
- [x] Rotação por reason: NO_AP_FOUND marca `noap` e gira; AUTH_FAIL
      marca `bad` e gira; todas bad → suspensão com log até ação em
      Redes; GOT_IP limpa as flags e fixa `s_last_good` (boot começa
      por ela).
- [x] Tela Redes: rede travada salva conecta direto (sem prompt);
      `wifi_net_saved_pass()` novo.
- [x] Backoff: min(2x, 30) — escada 2/4/8/16/30 sem o dente 32.
- [x] Sobre = "M5.2"; docs/WIFI.md com formato e comportamento.
- **Aceite HW**: duas redes salvas → desligar o AP atual rota para a
      outra sem toque; senha errada numa delas rota (não suspende com
      outras salvas); wifi.lua reescrito como lista; saved-connect sem
      prompt; regressões M5.0b.
- **Aceite HW**: fora de casa, log mostra `reconexão em 2 s (backoff)` →
      `4 s` → `8 s`… (não martelo fixo de 2 s) e NENHUM delay visível na
      UI durante retries; senha errada propositais → linha AUTH_FAIL
      única e silêncio até tocar em Redes; boot sem rede mostra hora
      plausível (não 1970/uptime imediato) se já teve sync antes;
      `hwm[...]` aparece ~60 s após boot e em cada standby; tela Redes
      abre e lista como no render.
- **Estado do hardware (relato do usuário, 2026-09-30)**: ele NÃO tem o
  conector de alto-falante da placa (mesmo tipo do conector de bateria) e
  falhou ao tentar fabricar um. Caminhos, em ordem de viabilidade:
  1. **M5a — USB-C OTG (`usb_host_uac`)**: fone/DAC/caixinha USB como
     saída imediata, zero solda. Divide a porta OTG com o teclado HID
     (testes de bancada; depois, seletor de modo em runtime). Conector
     ortogonal à tela = menos conveniente, aceito para testes.
  2. **Conector JST do falante (objetivo final)**: dica de compra —
     **cabo extensor de bateria LiPo JST-PH 2 vias** (ou uma bateria
     velha com o conector) já vem com o plugue certo; corta-se e
     solda-se no falante (~3 W, conferir o passo na variante da placa).
     Sem necessidade de crimpagem do zero.
  3. **P2 nos pinos de expansão (a validar)**: o usuário cogita adaptar um
     cabo P2 (estéreo, sem mic — o mic já é soldado na placa) nos pinos de
     expansão, sem o NS4150. ANTES de cortar qualquer fio: conferir no
     esquemático se algum header expõe a saída analógica do ES8311
     (HPOL/HPOR/LINEOUT) — se só a saída PÓS-PA (conector do falante)
     existir, ligar fone nela funciona mas com nível/impedância errados.
  4. Fone Bluetooth: impossível (`docs/BLUETOOTH.md` — C6 BLE-only, sem
     A2DP/LE Audio no IDF p/ C6).
- ES8311 + NS4150: beep de UI primeiro (prova do caminho I2S/PA_EN) — só
  quando houver saída física; enquanto isso, M5a valida decode/playback.
- Player local do SD: WAV → MP3 (decoder) → filas/playlists (`.m3u`/Lua)
- Controles na UI + teclas de mídia do teclado USB
- **Aceite**: tocar um álbum do cartão com tela apagada (standby não mata o áudio).

## M6 — Apps de PDA
- Agenda/Contatos/Relógio (alarmes) com dados em Lua/CSV no cartão
- Integração alarme ↔ áudio (M5) e wake agendado
- **Aceite**: alarme dispara com o aparelho em standby.

## Dívidas técnicas conhecidas
- Medição real de corrente por degrau (`docs/POWER.md` → plano de medição)
- Teardown completo do DSI no standby se light sleep passar de ~40 mA
- Validação do INT do GT911 (GPIO21) para `wake_on_touch`
- Confirmar possibilidade de medir bateria (folha 05 do esquemático) ou mod de divisor

### M3b.1 — Correções da validação em hardware (2026-09-23)
- [x] Picker: `Sobe` navega (antes cancelava o seletor, prendendo o usuário
      em subdiretórios); overlays fecham só com prompt/sheet
- [x] Histórico de diretórios: `Voltar` em Arquivos desfaz navegação
      (incluindo Sobe) antes de sair da tela
- [x] Pilha de navegação de apps (`nav_goto`/`nav_back`): Launcher →
      Arquivos → Editor → Voltar retorna p/ Arquivos (não p/ Launcher)
- [x] "Novo arq" no cabeçalho: cria arquivo vazio e já abre no editor
- [x] `pda_config_init`: se `system.lua` estiver ausente na raiz ativa,
      promove o espelho da outra raiz ANTES de cair em defaults (nunca
      "esquece" config por boot com uma raiz ausente); log explícito de
      promoção/criação no boot

## Backlog / issues conhecidas
- **USB primeira enumeração**: `USBH: Dev 1 EP 0 Error` +
  `ENUM: CHECK_FULL_DEV_DESC FAILED` no hot-plug/boot com teclado (ramp de
  energia); retry interno do IDF recupera em alguns segundos. No unplug,
  `EP command error: ESP_ERR_INVALID_STATE` (corrida de teardown do host
  lib). Workaround: reconectar. Futuro: retry de enumeração próprio
  (liberar dispositivo e re-enumerar) — candidato a M4.
- **system.lua envenenado com INT_MAX** (herança do bug de NaN): curado em
  2026-09-23 com saneamento+regravação única no boot (pda_config.c).

### M4b — Tela de redes (FEITO, aguardando validação com C6 2.12.9)
- [x] `screens/networks.slint`: scan listando SSID + RSSI + cadeado
      (ícones `lock`/`wifi` adicionados ao subset), ordenado por sinal
- [x] Toque em rede aberta conecta; rede fechada abre prompt de senha
      (OSK) e conecta; conexão persiste em `config/wifi.lua` (serializado
      pelo firmware, com escape de aspas/barras)
- [x] Tile "Redes" no launcher; status da conexão (`Conectada: ...`)
- [ ] M4c: Bluetooth LE (HCI hosted) — pesquisa feita em 2026-09-30
      (`docs/BLUETOOTH.md`): viável com NimBLE no P4 + slave 2.12.9 atual
      (sem reflash do C6); escopo = peripheral GATT "pda" (hora/clima/sync/
      ping/provisão Wi-Fi). BT Classic/A2DP descartado (C6 é BLE-only).
      Aguardando a vez na fila (gestos primeiro, prioridade do usuário).

## M-power (rework do light sleep) — ABERTO
- Pesquisa completa + plano em estágios (0 medir → 1 software M4.14 →
  2 painel → 3 light sleep → 4 hibernate v2) em `docs/POWER.md` § estágios
  (2026-10-04). Decisões em aberto listadas lá (§5).
- O caminho `power.light_sleep` está atrás do kill-switch
  `power_mgmt_light_sleep_active()` (sempre false): no wake, o remount do
  SDMMC (periférico compartilhado com o SDIO do hosted) causou Instruction
  access fault no core 1, e o wake por BOOT/toque não disparava
  (`sleep: Incorrect wakeup source (7)/(4)`).
- Para reabilitar: rework de re-init coordenado hosted+SDMMC+USB no wake,
  ou aceitar tear-down completo do hosted antes do sleep e re-init depois.

## Backlog M4d / M-usb (pedidos de 2026-09-27)
- **Wi-Fi multi-redes**: perfil de redes em `config/wifi.lua` (lista com
  prioridade) + segredo não em texto puro (NVS com criptografia ou chave
  derivada); UI de gerenciamento (esquecer/reordenar).
- **Descoberta de redes fora de casa**: investigar scan com auto-reconnect
  pausado (feito o primeiro passo em M4.5), canais 2.4/5, APs escondidos,
  e comportamento do C6 em hotspot de telefone.
- **Armazenamento visível no PC (estilo MTP)**: USB MSC/ou MTP próprio no
  porto OTG, ativado EXPLICITAMENTE em Config (conflita com o modo host do
  teclado — exige troca de papel USB dinamicamente). Não trivial; entra
  como milestone próprio depois do M5.

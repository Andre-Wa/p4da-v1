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

## M4.13 — Opt-out de gestos por tela, standby manual sticky e grade de pixel do cursor (FEITO; aguardando hardware)

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

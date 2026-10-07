# UI — estrutura e design system

## Árvore

```
main/ui/
├── app_ui.slint          # ENTRADA (CMake): status bar + roteador + API pública
├── theme.slint           # tokens (global Theme): cores M3-dark, shape, type, espaço
├── elements/
│   ├── buttons.slint     # FilledButton · TonalButton · DangerButton · GhostButton · KeyCap
│   ├── surfaces.slint    # Surface (elevação tonal) · Divider · ScreenTitle
│   └── osk.slint         # OnScreenKeyboard (teclado virtual)
└── screens/
    ├── launcher.slint    # tiles do menu
    ├── files.slint       # gerenciador de arquivos
    ├── notes.slint       # lista de notas
    ├── editor.slint      # editor com cursor + OSK ancorado
    ├── scripts.slint     # scripts Lua + console
    └── settings.slint    # configurações (gravadas em system.lua)
```

Regras:
- **`app_ui.slint` é a única superfície que o C++ vê.** Propriedades/callbacks
  `ed-*`, `cfg-*`, `file-*` etc. são contrato com `main.cpp`: não renomeie
  sem atualizar o C++.
- Telas novas = arquivo novo em `screens/` + enum `AppState` + bloco `if`
  no roteador + binding 1:1 das propriedades.
- **Nada de cor/raio/tamanho literal em telas**: tudo via `Theme.*`.
  Exceções aceitáveis: larguras de botões específicos de layout e alturas
  de fileira do OSK.
- Slint não aceita `component` dentro de `component` — elementos novos
  vivem em `elements/`.
- **Só é importável o que tem `export`**: todo component em `elements/` e
  `screens/` deve ser `export component X inherits ...` (sem isso o import
  falha com "No exported type called 'X'").
- Elemento dentro de `Rectangle` (posicionamento absoluto) **não herda
  tamanho**: dê `width/height: 100%` explícito (telas, Flickable, ListView).

## Design system (M3 Expressive adaptado, 4.3" 800×480)

- Esquema **dark** de alto contraste, primário cian (`#6fd3ff`) p/ legibilidade
  sob luz ambiente; superfícies tonais `surface-1..3` como "elevação".
- **State layers** do M3: overlay em botões e linhas de lista, em vez de
  trocar a cor de fundo. Padrão canônico (definido por andre-dev em
  `elements/buttons.slint` e propagado p/ listas):
  `Rectangle { width/height:100%; background: Theme.state-pressed;
  visible: ta.pressed; border-radius: Theme.shape-sm; opacity: 10%; }`
  Os tokens `state-*` no theme são **opacos**; a intensidade vem só do
  `opacity` no uso (10% botões/linhas). Não queime alpha na cor.
- **Shape**: sm 8 / md 12 / lg 16 (botões) / xl 28 (reservado p/ destaques).
- **Type scale** comprimida: display 30 · headline 24 · title 20 · body 16
  (piso de conforto no painel) · label 13 · tiny 11.
- Botões por papel: `Filled` = ação primária (Salvar), `Tonal` = secundária
  (Att/Sobe/Tecl/tiles), `Danger` = destrutiva (Apagar/Hibernar),
  `Ghost` = neutra (Voltar/Limpar).
- Semânticas: `success` p/ diretórios/scripts/SD, `tertiary` p/ dirty/bateria,
  `error*` p/ destrutivo.

## Escala de UI: estática, via tools/scale_type.py

Todos os tokens de tamanho (fontes, formas, espaços, alturas) são
`in property` **estáticos** no `theme.slint`, com base ~15% maior que o
design original. Não existe escala em runtime (removida em 2026-09-23):
o renderer pré-rasteriza fontes por tamanho estático, e tokens mutáveis
em runtime causaram bootloop por NaN (docs/POWER.md).

Para testar outra escala:
    python3 tools/scale_type.py 1.15 && idf.py build
(o fator multiplica os valores ATUAIS; p/ escala absoluta, `git checkout`
do theme antes). `main.cpp` espelha 5 métricas do theme para o cálculo de
scroll-do-cursor (ED_LINE_H, ED_OSK_H, ED_STATUS_H, ED_BTN_SM, ED_CHAR_W) —
o script avisa para conferi-las após escalar.

## Teclado virtual (OSK)

Layout "telefone": 3 fileiras de letras + fileira de funções, com a tecla
de **modo** (`123`/`abc`) abaixo do SHIFT. Camadas (espelham
`keyboard/key_mapping.md` do teclado USB):

| modo | shift | fileiras |
|---|---|---|
| abc | off | qwerty / asdf+ç / zxcv+pont |
| abc | on | MAIÚSCULAS |
| 123 | off | dígitos / `!@#$%^&*()` / `-=[]\_+{}\|` (Lower) |
| 123 | on | `` `~€£¥°¶•ªº `` / `<>?/:;"'´¨` / `+-×÷=≠≈∞§¤` (Raise adaptada) |

Fileira de funções: `[modo] [<] [>] [^] [v] [espaco] [enter] [ok]`.
Altura total 168 px; `main.cpp:ED_OSK_H` espelha esse valor p/ cálculo de
scroll-do-cursor — se mudar um, mude o outro.

## Gotchas conhecidos

- Plugin Slint do VSCode roda Slint mais novo que o build (1.12.1): avisos de
  deprecação dele (`viewport-*` → `content-*`) são ruído; no 1.12.1 o válido
  é `viewport-*`. **Erros** do plugin valem para qualquer versão.
- `int` não converte para `length` implicitamente: multiplique por `1px`
  ou declare a propriedade como `length`.
- property e callback não podem ter o mesmo nome num component.

## Ícones (Material Icons, subset)

- `main/ui/assets/MaterialIcons-Subset.ttf` (~3,4 KB, Apache-2.0, licença
  junto) + `main/ui/elements/icons.slint` com `IconGlyph` (codepoint vindo
  do C++) e wrappers nomeados (`IconFolder`, `IconCode`, `IconSd`, ...).
- Glifos pré-rasterizados no compile como qualquer fonte estática; cor via
  `tint`, tamanho via `size` (tokens do Theme).
- Adicionar ícone: nome em `tools/icons.json` → `tools/make_icons.sh` →
  wrapper em `icons.slint` → rebuild. Codepoints usados pelo C++ estão em
  `main.cpp` (`IC_*`).
- No C++, codepoints de ícone são literais narrow **sem prefixo `u8`**
  (`static const char *IC_X = "\uE2C7";` ou o caractere cru): em C++20
  (IDF 5.5) `u8""` é `const char8_t*` e não converte p/ `const char*`.
- **Codepoints de ícone NUNCA viajam do C++ em runtime**: o renderer
  pré-rasteriza apenas literais estáticos do `.slint`. O C++ envia uma
  *categoria* ("dir","lua","audio",...) e `IconForKind` (icons.slint) faz o
  mapeamento com literais crus no ternário. Sintoma do erro: espaço
  reservado mas glifo em branco.

## Editor: markdown + cursor (M4.7)

### Markdown de bloco + inline — o que renderiza e como (M4.10)
- Parser único em `main/md_render.h`, compartilhado com o harness
  offscreen (`tools/render_offscreen.sh`): o PNG de evidência renderiza o
  MESMO código que roda na placa.
- `# `→ h1 (23 px, bold, primary); `## `→ h2 (20 px bold); `### `→ h3
  (18 px bold primary); parser tolerante: `#sem espaço` também conta.
- `- `/`* `→ bullet `•` (marcador em primary); `> `→ citação com barra
  `|` muda à esquerda; cercas ``` → bloco mono verde (a cerca vira linha
  de respiro; o info-string `lua` não aparece); `---`/`***`→ régua;
  linha EM BRANCO = estilo 9 (não desenha régua); resto = parágrafo.
- LISTAS ANINHADAS (M4.11): `  - item` recuado = bullet deslocado
  (2 espaços = 1 nível, teto 3 níveis); dentro de cercas ``` o recuo
  continua código. Teste: `tools/md_test.cpp`.
- INLINE (M4.10): `**x**` e `__x__` → negrito (face 700); `` `x` `` →
  código verde. Marcador não fechado vira texto literal. Itálico NÃO tem
  (sem face oblíqua embutida — UI_WISHLIST).
- Runs por linha (`[[MdRun]]` = text/col/kind): cada trecho é um `Text`
  posicionado em `6px + col * cell` (cell = fsize*0.6, avanço mono), então
  negrito/código não precisam de HorizontalLayout e o scroll horizontal
  virtualizado continua dono do x.
- Regra de OURO do software renderer (válida p/ qualquer Text): um `Text`
  cujo `height` é MENOR que a line-box da fonte (1.3188em no PDA Mono:
  hhea 2146/-555 @ upem 2048) renderiza **zero linhas** — h1 23px precisa
  de 30.3px em fileira de 28px; o `MdRow` dá folga (height 34px, y -3px)
  para fsize >= 22px. Linha do editor = `Theme.line-h` (28 px) espelhada
  em `main.cpp:ED_LINE_H`; avanço mono 0.6em = 10.8px espelhado em
  `ED_CHAR_W` e no `cell` do editor.slint.
- Regra de OURO 2 (M4.10): nenhum `Text` de leitura contém ESPAÇO — o
  glifo de espaço pré-rasterizado às vezes avança 2 células em Texts
  longos (medido em pixel). O parser quebra em palavras; os vãos são
  células em branco entre runs. Não "otimize" juntando runs.
- BOM UTF-8 e CRLF são saneados no `ed_load` (BOM na 1ª linha cegava o
  detector de `#`).
- Regra de OURO do software renderer (válida p/ qualquer Text): um `Text`
  cujo `height` é MENOR que a line-box da fonte (~1.17× font-size no
  Roboto, ~1.32× no Roboto Mono) renderiza **zero linhas** — foi assim que
  h1 ficou invisível com linha de 25 px. Linha do editor = `Theme.line-h`
  (28 px) espelhada em `main.cpp:ED_LINE_H`; avanço mono 0.6 em =
  10.8 px espelhado em `ED_CHAR_W` e no `cell` do editor.slint.
- BOM UTF-8 e CRLF são saneados no `ed_load` (BOM na 1ª linha cegava o
  detector de `#`).

### Cursor do editor
- O cursor OCUPA O SLOT do caractere (terminal-style): com cursor na col 3
  de `exemplo` a linha exibida é `exe_plo` (o char some enquanto aceso).
  No fim da linha o glifo acresce após o último char.
- bar/under = glifo `|`/`_` embutido na string (mono, avanço = célula).
- block = caractere virou espaço + overlay `Rectangle` no delegate
  (`cursor-row/col/on/kind`); `█` não existe em nenhuma fonte embutida.
- Piscar: esp_timer 530 ms → `s_cursor_on` → `ed_push_ui(false)`; qualquer
  atividade de cursor rearma a fase aceso (`cursor_rearm`).
- **Grade de pixel (M4.13)**: o software renderer 1.12 grava o avanço do
  glifo na grade de pixels — pitch REAL = `round(0.600em × fsize)` (medido:
  18px→11, 20px→12, 23px→14; sonda `fontpitch`). Todo posicionamento por
  coluna usa essa célula inteira: `cell = Math.round(fsize/1px*6/10)*1px`
  no .slint e `ED_CHAR_W = 11.0f` no C++ (ESPELHOS: se um mudar, o outro
  muda). Com 10.8 fracionário o overlay do block derivava 0.2 px/col
  (~9 px na col 43 — "espaço de outra letra"); bar/under nunca derivaram
  por serem glifos in-flow. Sonda de regressão: modo `coltest` do harness
  (bloco na col 43 vs `|` in-flow na mesma coluna → mesma célula).

### Fontes embutidas (M4.9 — família interna "PDA Mono", peso 400/700)
- Faces: `Mono-Base.ttf` (wght 400 desde M4.9 — o 300 do M4.8 foi achado
  fino no painel) + `Mono-Bold.ttf` (700), família interna fixa
  **"PDA Mono"** (`default-font-family: "PDA Mono"`), hoje originadas da
  Roboto Mono; + `MaterialIcons-Subset.ttf` (ícones).
- Trocar de fonte/peso NÃO toca em `.slint` nem em C++: é só
  `tools/font_pipeline.py --mono <ttf> --weight N` (família é renomeada
  na name-table). Receita completa, contratos (avanço 0.600em, ligaturas
  OFF, charset/âncora, licença) e tabela de fontes candidatas em
  **`docs/FONTES.md`**. Comparativo Roboto×Fira Code:
  `render_comparo_fontes.png`.
- Charset: colhido automaticamente dos literais dos `.slint` (comentários
  removidos) — inclui a âncora Latin-1 de `app_ui.slint`, que garante
  acentos em texto de runtime; o pipeline FALHA se faltar glifo de char
  usado pela UI.
- Avanço mono = 0.6em exato NO ARQUIVO da fonte; o renderer 1.12 grava o
  avanço na grade de pixels, então a célula efetiva é
  `round(0.6em × fsize_px)` (18px→11). Consumidores de coluna (MdRow,
  cursor overlay, ED_CHAR_W, tap→col) usam a célula inteira — ver "grade
  de pixel" em "Cursor do editor" e docs/FONTES.md (M4.13).
- `■`/`█` não existem na Roboto Mono — botões de cursor do settings usam
  palavras (barra/traço/bloco) e o block-cursor é um Rectangle. (Fira Code
  tem ambos, mas o contrato de família fixa mantém a UI independente.)
- Por que explicitar a família default: sem isso o compilador embute o
  sans do FONTCONFIG do host (máquina de build!) e o `font-weight: 700`
  não tinha face bold → títulos idênticos ao corpo. Com a família
  explicitada, a resolução usa os imports do .slint (independe do host).

## Render offscreen no host (harness de validação)

Sem hardware dá para ver EXATAMENTE o que o device renderiza:
1. `slint-compiler` 1.12.1 (release GitHub, Linux-x86_64) com
   `--embed-resources embed-for-software-renderer --cpp-file a.cpp --cpp-file b.cpp`
   (split p/ caber em RAM pequena) sobre `main/ui/app_ui.slint`;
2. linkar com `Slint-cpp-1.12.1-Linux-x86_64` (Qt6/xkb/gbm via apt) num
   `WindowAdapter` próprio cujo `renderer()` é um `SoftwareRenderer`
   (`slint-platform.h`) e `render(span<Rgb8Pixel>, stride)` p/ um buffer;
3. alimentar propriedades `ed-*` como o `ed_push_ui` faz e dumpar PPM/PNG.
- Fonte de referência: `tools/offscreen_render_harness.cpp` (compila só no
  host; não entra no firmware).
- Modos: `reading`, `reading2`, `edit`, `prompt` e (M4.12) `panel` /
  `paneldrag` — estes últimos fixam `qs-dragging=true` + `qs-drag` em 192/110
  px, porque durante o drag a animação tem duração 0 ms e o frame único do
  harness mostra o estado exato do drag-following. Evidências:
  `render_panel.png`, `render_paneldrag.png` na raiz. (M4.13) `coltest`
  (overlay do cursor-bloco na col 43 vs `|` in-flow na mesma coluna —
  regressão de alinhamento) e `fontpitch` (24 'M' idênticos por estilo
  h1/h2/h3/parágrafo → mede o pitch real do renderer por face/tamanho).
  `tools/ppm2png.py` converte os PPM em PNG na raiz.

## Gestos + pulldown de ajustes rápidos (M4.12)

Mapa de gestos (tudo implementado em `app_ui.slint`, camada entre o conteúdo
e o `PromptOverlay`):

| Gesto | Zona | Efeito |
|---|---|---|
| Arrastar p/ baixo | status bar (34 px do topo) | abre o painel seguindo o dedo; solta com >1/3 da altura = abre, senão recolhe |
| Arrastar p/ cima no grip / toque no grip / toque no scrim | painel | fecha |
| Arrastar p/ direita da borda esquerda (>36 px, dominante horizontal) | tira de 18 px, qualquer tela menos o launcher | `app-back()` — no gerenciador, `fm-back()` (fecha folha/picker/sobe dir antes de sair) |

Empilhamento (Slint: último declarado = topo do hit-test):
`conteúdo → tira-esquerda → scrim → QuickPanel → tira-topo → PromptOverlay`.
- `PromptOverlay` tem TouchArea modal de tela cheia: com prompt aberto
  (senha Wi-Fi, confirmação de save) nenhum gesto dispara.
- Tira do topo declarada POR CIMA do painel com `enabled: !qs-open`: painel
  aberto, o hit-test desce para o grip do QuickPanel.

### Opt-out de gestos por tela (M4.13)

Nem toda tela quer os gestos do app: o editor rola a caixa de texto com
arrasto (inclusive na margem esquerda) e tem botão Voltar próprio. O
mecanismo é declarativo, em `app_ui.slint`:

| Propriedade | Default | Efeito em `false` |
|---|---|---|
| `back-swipe-allowed` | `active-app != Launcher && != Editor` | tira da borda esquerda sai do hit-test (a tela vira dona do arrasto) |
| `qs-pull-allowed` | `true` | tira do topo deixa de capturar o pulldown |

- `in-out`: o harness/C++ podem forçar p/ teste; o binding reage a
  `active-app` sozinho (trocou de tela, a política troca junto).
- O Launcher "opta" por ordem de empilhamento (a camada dele vence o
  hit-test) — mesmo efeito, mecanismo histórico.
- Para uma tela nova optar por não ter back-swipe: acrescente a condição
  no binding de `back-swipe-allowed` (ponto único, documentado aqui).
- O pulldown continua GLOBAL de propósito: a tira cobre só a status bar
  (34 px), que não tem controles — nenhuma toolbar de tela é roubada.
- Scrim e painel ficam `visible: false` com altura ≤1 px (não roubam toque).
- `qs-cur-h` anima (180 ms ease-out) só quando NÃO está arrastando
  (`animate { duration: qs-dragging ? 0ms : 180ms }`) — drag-following sem
  lag e sem relayout por frame (conteúdo do painel tem altura fixa 192 px,
  o clip revela).

Painel (`elements/quick_panel.slint`): grip, slider de brilho (alvo 48 px),
linha de rede (`cfg-wifi-info` + rssi da status bar, ambos vivos) e três
atalhos de 48 px — **Redes** (`cfg-open-networks`), **Config**
(`open-app-dispatch("Config")`), **Suspender** (`cfg-sleep-now`): nenhum
callback novo de navegação. Sem ícones de propósito (o subset MaterialIcons
embarcado não tem glifo de sol/wifi/lua; texto não muda o charset).

Persistência do brilho: `on_qs_brightness` aplica o backlight e atualiza a
RAM a cada tick do slider (zero I/O); `on_qs_state(false)` (painel fechou)
grava o `system.lua` uma vez. `cfg-save` e o save do cursor limpam a flag
dirty (eles já persistem tudo).

Alvos de toque: todos os controles do painel têm ≥48 px de altura
(direção da PRIORIDADE 2 — OSK maior — que será tratada no M4.13).
- No host sem fontconfig o fontdb do slint-compiler ainda pede um sans
  default; com `default-font-family: "Roboto Mono"` a resolução usa os
  IMPORTS do .slint (as faces Light/Bold são custom fonts carregadas antes
  da query) — não depende mais de fontes instaladas no host. O truque de
  renomear DejaVu p/ "Arial" em `~/.local/share/fonts` era só p/ hosts sem
  família default alguma.

## M5.0 Expressive — acento dinâmico, formas, movimento (2026-10-05)

- **Cor dinâmica**: `Theme.accent` (string) dirige 7 cores de marca por
  ternárias em `theme.slint`; C++ seta `AppWindow.cfg-accent`
  (propagação `init`/`changed`). Limitação Slint 1.12: globals NÃO são
  expostas ao C++ — nunca tente `ui->global<Theme>()`.
- **Formas**: shapes 6/12/18/24/40 px (botões pill, diálogos redondos).
- **Movimento**: OSK do editor sempre montado com `animate height`
  (170 ms ease-out, reveal clipado); quick-panel mantém drag-following.
  Tipografia expressiva DEFERIDA (glifos por tamanho custam flash;
  escala offline via `tools/scale_type.py`).
- **Prompt (M4.16.2)**: botões OK/Cancelar em geometria ABSOLUTA no
  diálogo — ver gotcha de layout em `docs/DEPENDENCIAS.md`.

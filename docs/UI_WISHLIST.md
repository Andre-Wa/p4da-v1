# Wishlist de UI (anotada pelo andre-dev em 2026-09-24)

Backlog de refinamento de interface — sem pressa, entra entre milestones.

## Editor
- [x] Cursor piscante, glifo configurável (`|`, `_`, bloco) — M4.6
- [x] Fonte monoespaçada no editor (ou na UI toda, se o custo de rasterização
      por tamanho for aceitável) — alinharia colunas e facilitaria código.
      — FEITO M4.9: UI toda usa PDA Mono (avanço 0.600em).

## Interface
- [x] Configurações: barra lateral com ícones + abas — FEITO M4.5
      (Redes/Tela/Entrada/Sistema/Sobre; Bluetooth aguarda M4c).
- [x] Launcher: grade de ícones quadrados com legenda — FEITO M4.5
      (grade 3x3 de tiles).
- [x] Sliders da Config: readouts arredondados a inteiro — FEITO M4.5
      (Math.round).
- [x] Trocar CheckBox por Switch na tela de Config — FEITO M4.5.
- [x] Notas: modos ler (markdown render) vs editar — M4.6
- [x] Markdown inline: **negrito** e `código` — M4.10
- [x] Markdown: listas aninhadas (`  - item`, 2 espaços = 1 nível, teto 3)
      — M4.11 (teste `tools/md_test.cpp`)
- [ ] Markdown inline: itálico/emphasis (precisa embutir uma face oblíqua;
      o renderer 1.12 não sintetiza slant). Caminho: adicionar
      `--italic-src` ao `tools/font_pipeline.py` (Roboto Mono Italic,
      Apache-2.0, ~40 KB subset) + terceira família no runtime.

## Acrescentado em 2026-10-02
- [x] Opt-out de gestos do app por tela: editor (edição+leitura) fica sem o
      back-swipe da borda esquerda (margem rola o texto; voltar = botão da
      toolbar) — M4.13 (`back-swipe-allowed`/`qs-pull-allowed` em app_ui.slint).
- [x] Cursor de bloco alinhado à célula do caractere no fim da linha —
      M4.13 (renderer grava avanço na grade de pixels; `cell` inteira).
- [x] "Suspender" do painel entra em standby de verdade — M4.13 (pedido
      sticky; antes a cauda de toque do tap cancelava em silêncio).
- [ ] Energia: toggle de wake por toque gateando o ISR, standby mais
      profundo (pausar Wi-Fi/NTP), botão BOOT como wake — fila M4.14.

## Acrescentado em 2026-09-28
- [x] Gestos de navegação (swipe entre telas/voltar, swipe na status bar).
      **PRIORIDADE 1 do usuário (2026-09-30)** — feito no M4.12: arrasto da
      borda esquerda = voltar (mesma semântica dos botões Voltar, incl.
      fm-back no gerenciador); arrasto da status bar = pulldown.
- [x] Menu de configurações rápidas pulldown na barra de status
      (brilho, Wi-Fi, standby, brilho-noturno...).
      Feito no M4.12: painel com brilho AO VIVO (persiste no fechamento),
      linha de rede (SSID + rssi) e atalhos Redes/Config/Suspender.
      Usuário (2026-09-30): pensa numa barra com atalho de brilho + redes/
      conexões; combinar com o milestone de gestos (pulldown É um gesto).
- [x] OSK: teclas maiores/mais espaçadas — FEITO M4.16/M4.16.1:
      4 fileiras ≥48px, spacing 6px, setas/ok 56px, SHIFT/BACK/MODE 76px,
      enter 84px, osk-h 226px. **PRIORIDADE 2 do usuário (2026-09-30).**
      Fileira de números fixa: testada na M4.16 e REMOVIDA no feedback
      (cobria tela); dígitos seguem no modo 123.
- [x] OSK: repetição ao segurar (BACKSPACE/setas/espaço, 400ms+90ms) —
      FEITO M4.16.1 (sugestão do usuário na validação da M4.16).
- [x] M3 Expressive mais fiel — FEITO M5.0 (2026-10-05): cor dinâmica
      (ui.accent, 5 paletas), formas maiores (shapes 6-40px, botões
      pill), movimento (OSK animado 170ms; quick-panel já tinha drag).
      Tipografia expressiva DEFERIDA (glifos por tamanho custam flash).
      **PRIORIDADE 4 do usuário (2026-09-30)** (itálico = 3, opcional).

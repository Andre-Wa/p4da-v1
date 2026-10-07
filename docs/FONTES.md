# FONTES — como o p4da embute e troca fontes (M4.9)

Este documento explica **como funciona `tools/font_pipeline.py`** e como
trocar a fonte da app (Roboto Mono → Fira Code → qualquer outra) sem tocar
em nenhum `.slint` e sem quebrar o editor.

Estado atual: **família interna `PDA Mono`, base wght 400 + negrito wght 700**
(origem: Roboto Mono, Apache-2.0). Validado offscreen em
`render_reading.png` / `render_edit.png` / `render_prompt.png` e comparado com
Fira Code em `render_comparo_fontes.png`.

---

## 1. Por que existe um pipeline (e não só "colocar um .ttf na pasta")

O Slint 1.12 **pré-rasteriza os glifos em tempo de COMPILAÇÃO**. O
`slint-compiler` lê os `@font-face`/`import "*.ttf"` do `.slint`, rasteriza
cada glifo do charset em cada pixel-size usado e embute o bitmap no firmware.
Consequências práticas (todas já mordidas neste projeto):

1. **O charset é global**: vale a união de todas as strings LITERAIS dos
   `.slint` + a propriedade `charset:` (a "âncora" de rasterização em
   `main/ui/app_ui.slint`). Texto vindo do C++ em runtime (nomes de arquivo,
   notas, OSK) só sai com glifo se estiver na âncora.
2. **Glifo ausente = tofu silencioso** (ou fallback p/ fonte do host, que não
   existe no device). Por isso o subset precisa ser *superset* do que a UI usa.
3. **Tamanho do binário cresce por glifo×pixel-size**: subsetar é obrigatório
   (a Roboto Mono completa + proporcionais estourava a partição `factory`).
4. **A família precisa existir no `.slint`**: sem `default-font-family`, o
   compilador embute o sans do *fontconfig da máquina de build*.

## 2. O que o pipeline faz, passo a passo

```
fonte de origem (TTF/OTF, variável ou estática)
        │ 1. instância de peso   (--weight / --bold-weight; fontTools.varLib.instancer)
        │ 2. remove ligaturas    (calt/liga/dlig/clig — ver §5)
        │ 3. renomeia a família  (name-table IDs 1/2/4/6/16/17 -> "PDA Mono")
        │ 4. subset pelo charset (literal dos .slint + âncora + pisos ASCII/Latin-1/box)
        │ 5. valida              (família, peso, avanço 0.600em, cobertura 100% do crítico)
        │ 6. licença             (proveniência + texto integral em assets/LICENSE-mono.txt)
        ▼
main/ui/assets/Mono-Base.ttf  +  Mono-Bold.ttf
```

Detalhes que importam:

- **Colheita de charset** (`harvest_ui_charset`): varre todos os `.slint` de
  `main/ui/`, tira comentários (`strip_comments` — senão um comentário como
  `// "█" não existe` vira charset obrigatório e a validação acusa falha
  falsa) e colhe cada literal de string. Isso inclui automaticamente o
  `text:` da âncora em `app_ui.slint` (Latin-1 suplementar + tipográficos),
  que é o que garante acentos em runtime. Codepoints privados U+E000–F8FF
  (ícones Material) são descartados aqui — pertencem ao outro TTF.
- **Validação em dois baldes**: chars vindos da UI são *críticos* (ausência =
  `ERRO` e o script falha); chars dos pisos do script (box-drawing, setas…)
  são *opcionais* (aviso, porque Roboto Mono simplesmente não os tem).
- **Renomear a família p/ `PDA Mono`** é o que torna a troca de fonte um
  assunto SÓ do pipeline: `default-font-family: "PDA Mono"` e os
  `font-family: "PDA Mono"` nunca mudam, não importa a origem.
- **`usWeightClass` = peso real da instância.** O match de runtime do Slint é
  "família exata + peso mais próximo": com base 400 e bold 700, texto normal
  pega a base e `font-weight: 700` pega o negrito, sem ambiguidade.

## 3. Ajustar o peso (o pedido "quero um pouco mais de corpo")

```bash
# 400 = Regular (padrão desde M4.9); 450/500 p/ mais corpo; 350 p/ voltar p/ leve
python tools/font_pipeline.py --mono /tmp/RobotoMono-VF.ttf --weight 400 \
    --license /tmp/Apache-2.0.txt --license-url https://www.apache.org/licenses/LICENSE-2.0
idf.py build && idf.py flash
```

O intervalo aceito é o eixo `wght` da fonte (Roboto Mono VF: 100–700). Depois
de regenerar, rode o harness (§6) para ver o peso em pixel antes do hardware.
Histórico: M4.8 usou 300 (Light) — legível porém fino no painel; M4.9 subiu
para 400 a pedido do usuário.

## 4. Trocar de fonte (ex.: Fira Code) — receita completa

```bash
# 1) baixar as faces (Google Fonts serve estáticas por peso)
#    https://fonts.googleapis.com/css2?family=Fira+Code:wght@300..700
# 2) gerar (note: --bold-src separado; --family continua "PDA Mono")
python tools/font_pipeline.py --mono /tmp/FiraCode-400.ttf --weight 400 \
    --bold-src /tmp/FiraCode-700.ttf \
    --license /tmp/FiraCode-OFL.txt --license-url https://scripts.sil.org/OFL
# 3) validar offscreen e flashar
tools/render_offscreen.sh && idf.py build && idf.py flash
```

**Nenhum `.slint` muda** (a família interna é fixa) e **nenhum C++ muda**,
desde que a fonte respeite o contrato do editor (§5). Já testado no sandbox
com Fira Code 400/700: cobertura 99,2% (tem box-drawing e setas que a Roboto
Mono não tem), avanço 0.600em idêntico, `calt` removido com sucesso — ver
`render_comparo_fontes.png` (faixas 2 e 4).

Fontes candidatas e o que o `verify_face` olha:

| fonte | licença | avanço | box-drawing | observação |
|---|---|---|---|---|
| Roboto Mono (VF 100–700) | Apache-2.0 | 0.600em | não | **embarcada hoje** |
| Fira Code (estática/peso) | SIL OFL 1.1 | 0.600em | sim | liga por padrão → pipeline remove `calt` |
| JetBrains Mono (VF) | SIL OFL 1.1 | 0.600em | sim | idem, remove `calt` |
| IBM Plex Mono | SIL OFL 1.1 | 0.600em | sim | idem |

Qualquer TTF/OTF funciona; se o avanço não for 0.600em o `verify_face` avisa e
o §5 explica o que ajustar.

## 5. Contratos e armadilhas (leia antes de trocar de fonte)

1. **Avanço mono = 0.600em NO ARQUIVO; célula efetiva = round(0.6em × fsize_px).**
   O renderer Slint 1.12 grava o avanço do glifo na grade de pixels (medido
   com a sonda `fontpitch` do harness: 18px→11.000, 20px→12.000, 23px→14.000,
   min==max em todas as faces). Por isso o posicionamento por coluna usa a
   célula INTEIRA: `cell = Math.round(fsize/1px * 6/10) * 1px` no `.slint`
   (MdRow + EditorScreen) e `ED_CHAR_W = 11.0f` no `main.cpp` (M4.13). Com
   10.8 fracionário o overlay do cursor-bloco derivava 0.2 px/col (≈9 px na
   col 43). Fonte com avanço ≠ 0.600em desalinha tudo de novo: ajuste o
   `verify_face` E os dois espelhos na proporção `round(avanço_em × fsize)`.
2. **Ligaturas OFF por padrão.** Fira Code & cia. ligam `=>`, `->`, `!=` via
   `calt`; o Slint shapeia com rustybuzz, que aplica `calt` por padrão → nº de
   glifos < nº de chars → o cursor de slot desalinha no meio da linha. O
   pipeline remove essas features do GSUB (`--keep-ligatures` desliga o corte,
   por sua conta e risco).
3. **Charset**: o subset precisa cobrir a âncora de `app_ui.slint`. Se você
   adicionar um caractere novo em texto de runtime (ex.: um símbolo novo no
   OSK), ponha-o no literal da âncora E rode o pipeline (a colheita pega a
   âncora sozinha; o `verify_face` acusa se a fonte escolhida não tiver o
   glifo).
4. **Licença é obrigatória no binário distribuído.** Apache-2.0 e OFL exigem
   o texto da licença acompanhando; o pipeline grava
   `assets/LICENSE-mono.txt` = proveniência (origem, pesos, comando) + texto
   integral. Trocou de fonte ⇒ troque `--license`/`--license-url`.
5. **Quirk de path do slint-compiler 1.12**: `import "../assets/Mono-Base.ttf"`
   resolve a partir de `screens/`, mas `"./assets/..."` a partir de `main/ui/`
   NÃO. Por isso os imports vivem em `screens/editor.slint`.
6. **Não suba para o Slint 1.18+** esperando "resolver fontes": versões novas
   embutem 31–35 MB de glifos (flash overflow). Ver `docs/DEPENDENCIAS.md`.
7. **O set ASCII do compiler NÃO tem crase.** `embed_glyphs.rs` (1.12.1)
   pré-rasteriza `a-zA-Z0-9` + `" '!\"#$%&()*+,-./:;<=>?@\\[]{}^_|~"` +
   `●…` — o 0x60 (`` ` ``) fica de fora. Sem ele num literal estático
   (âncora), a crase some de texto de runtime E do rótulo da tecla do OSK.
   Está na âncora de `app_ui.slint`; não remova.
8. **Line-box = 1.3188em** no PDA Mono (hhea asc 2146 / desc -555 @ upem
   2048). `Text` com `height` menor que a line-box renderiza ZERO linhas
   no renderer 1.12: h1 23px ⇒ 30.3px > fileira 28px. O `MdRow` do
   editor.slint compensa (height 34px, y -3px) p/ fsize >= 22px. Fonte
   nova com métrica maior que 1.22em quebra h1 de novo — confira hhea.
9. **Glifo de espaço não é confiável em Text longo.** Medido em pixel no
   1.12: dentro de um Text comprido, um espaço pode avançar 2 células e
   deslizar o resto da linha. Contrato do modo leitura: runs SEM espaço
   (parser quebra em palavras; vãos = células em branco por coluna).
10. **Grade de pixel do renderer (M4.13).** Sonda de regressão:
    `./harness fontpitch` (24 'M' idênticos por estilo h1/h2/h3/parágrafo →
    pitch esperado round(0.6em×fsize), min==max) e `./harness coltest`
    (overlay do cursor-bloco na col 43 vs `|` in-flow na mesma coluna →
    mesma célula). Trocou de versão do Slint ou de fonte: rode as duas
    antes de flashar. (Hipótese da causa: hinting grid-fit na rasterização;
    o efeito é estável e agora é o contrato.)

## 6. Validação sem hardware (harness offscreen)

```bash
tools/render_offscreen.sh        # SLINT_COMPILER/SLINT_CPP via env; padrão /tmp
```

Renderiza os 4 modos (`reading`, `reading2` = leitura com scroll p/ ver o
fim do doc, `edit`, `prompt`) com o MESMO backend de software do device e
grava `render_*.png` na raiz. O modo leitura passa o documento de teste pelo
MESMO parser do firmware (`main/md_render.h`), então o PNG prova o pipeline
inteiro. É assim que se confere peso novo, troca de fonte, markdown e
regressões de cursor/OSK antes de flashar. O `main.cpp` do harness é
`tools/offscreen_render_harness.cpp`.

## 7. Arquivos

| caminho | papel |
|---|---|
| `tools/font_pipeline.py` | gera/valida as faces + licença |
| `main/ui/assets/Mono-Base.ttf` | face base (`--weight`, hoje 400) |
| `main/ui/assets/Mono-Bold.ttf` | negrito (`--bold-weight`, hoje 700) |
| `main/ui/assets/MaterialIcons-Subset.ttf` | ícones (intocado) |
| `main/ui/assets/LICENSE-mono.txt` | proveniência + licença da origem |
| `main/ui/app_ui.slint` | `default-font-family: "PDA Mono"` + âncora de charset |
| `main/ui/screens/editor.slint` | imports das faces + `cell` (avanço) |
| `tools/render_offscreen.sh` | build+render do harness no host |

## 7. Itálico (wishlist prio 3) — ADIADO por decisão do usuário (2026-10-05)

Caminho quando voltar: `--italic-src` no pipeline (Roboto Mono Italic,
Apache-2.0) + terceira família no runtime + kinds de run itálico no
`md_render.h` + faces no editor. Não é "fácil": mexe em charset/âncora,
orçamento de flash e no contrato de avanço 0.600em.

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
- **State layers** do M3: overlay alpha (`state-pressed`/`state-hover`) em
  botões e linhas de lista, em vez de trocar a cor de fundo.
- **Shape**: sm 8 / md 12 / lg 16 (botões) / xl 28 (reservado p/ destaques).
- **Type scale** comprimida: display 30 · headline 24 · title 20 · body 16
  (piso de conforto no painel) · label 13 · tiny 11.
- Botões por papel: `Filled` = ação primária (Salvar), `Tonal` = secundária
  (Att/Sobe/Tecl/tiles), `Danger` = destrutiva (Apagar/Hibernar),
  `Ghost` = neutra (Voltar/Limpar).
- Semânticas: `success` p/ diretórios/scripts/SD, `tertiary` p/ dirty/bateria,
  `error*` p/ destrutivo.

## Gotchas conhecidos

- Plugin Slint do VSCode roda Slint mais novo que o build (1.12.1): avisos de
  deprecação dele (`viewport-*` → `content-*`) são ruído; no 1.12.1 o válido
  é `viewport-*`. **Erros** do plugin valem para qualquer versão.
- `int` não converte para `length` implicitamente: multiplique por `1px`
  ou declare a propriedade como `length`.
- property e callback não podem ter o mesmo nome num component.

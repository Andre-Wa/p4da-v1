# Matriz de dependências × ESP-IDF 5.5.1

Auditoria feita contra o registry (`components.espressif.com`) e contra o
build real (log de 2026-09-22). **Conclusão: nenhuma incompatibilidade
declarada com o IDF 5.5.1** — todas as dependências exigem `idf >= 5.0`
ou `>= 5.1`, e o Component Manager valida isso na resolução (se houvesse
conflito declarado, o resolve falharia antes de compilar).

O que já nos mordeu não foi incompatibilidade declarada, e sim **mudança de
API dentro da mesma major** permitida pelos ranges `^`. Por isso o
`idf_component.yml` agora usa `==` para congelar o conjunto validado.

## Versões resolvidas (build de 2026-09-22, IDF v5.5.1, GCC 14.2)

| Componente | Range antigo | Resolvido | Exigência de IDF declarada | Status no build |
|---|---|---|---|---|
| `espressif/esp_lcd_st7701` | `^1.1.5` | **1.1.5** | >= 5.x (série 1.x; 2.x pede IDF 6.0+) | ✅ compila |
| `espressif/esp_lcd_touch_gt911` | `^1.1.3` | **1.2.1** | >= 5.0 | ✅ compila |
| `espressif/esp_lcd_touch` (transitiva) | — | **1.2.1** | >= 5.0 | ✅ (warning de deprecação vem do Slint, não nosso) |
| `espressif/usb_host_hid` | `^1.0.0` | **1.2.1** | >= 5.0 (+ `espressif/usb ^1.0.0`, satisfeito pelo `usb` do próprio IDF 5.5) | ✅ compila (warnings de campos novos em `usb_host_config_t`, inofensivos) |
| `slint/slint` | `^1.12.1` | 1.18.1 → **rebaixado p/ 1.12.1** | >= 5.1 | ⛔ 1.18.1: regressão de fontes (abaixo); ✅ 1.12.1 é a versão provada no protótipo |
| `joltwallet/littlefs` | `^1.14.8` | **1.22.3** | >= 5.0 | ✅ compila (nossos campos de `esp_vfs_littlefs_conf_t` existem nessa versão) |
| `espressif/lua` | `^5.5.0` | **5.5.0** | >= 5.0 | ⚠️ compila (API 5.5: `lua_newstate` c/ seed, `LUA_RELEASE`), **mas a VM é 32-bit e o define é PRIVATE** — `main` precisa de `LUA_32BITS=1` (risco #6; causa raiz do INT_MAX, `docs/HARDWARE.md` achado #9) |

## Riscos residuais (monitorar, não bloqueantes)

1. **ABI do prebuilt do Slint no link.** O `.a` pré-compilado vem do Slint,
   não do seu toolchain. Se houver `undefined reference` ou símbolo duplicado
   no link, a causa é essa — e a saída é pinar um Slint cujo prebuilt declare
   suporte ao IDF 5.5 ou compilar o Slint from source. *Até o último log,
   todas as unidades compilaram; o link ainda não havia sido executado.*
2. **littlefs 1.14 → 1.22**: o formato on-disk do LittleFS é estável dentro da
   série 2.x, então uma partição formatada por uma versão monta na outra.
   Se um dia voltarmos o littlefs, particões escritas pela versão nova podem
   não montar na antiga (direção única).
3. **`esp_lcd_touch_get_coordinates` deprecated**: o aviso vem de dentro do
   `slint-esp.cpp` (Slint chamando API depreciada do esp_lcd_touch 1.2).
   Some quando o Slint migrar para `esp_lcd_touch_get_data`. Não é nosso.
4. **USB Host**: o IDF 5.5 acrescentou campos em `usb_host_config_t`
   (`root_port_unpowered`, `enum_filter_cb`, `fifo_settings_custom`,
   `peripheral_map`). Nosso init por agregado deixa-os zero — comportamento
   default correto. Warning cosmético.
5. **IDF 6.x**: não subir. Além do P4 engineering-sample (rev < 3.1 recusado),
   a série 2.x do `esp_lcd_st7701` e outras APIs mudam junto. O teto
   `idf: ">=5.3,<6.0"` no `idf_component.yml` protege o resolve.
6. **ABI 32-bit do `espressif/lua` (era bug, virou contrato nosso).** O
   componente compila a VM com `LUA_32BITS=1` (`lua_Number=float`,
   `lua_Integer=int32`) via `target_compile_definitions(... PRIVATE ...)` —
   o define NÃO chega aos consumidores (espressif/developer-portal
   discussion #188). Sem compensação, `lua_tonumber` cruza float→double e
   devolve lixo (foi o "INT_MAX" do system.lua, 2026-09-24..30). Nosso
   contrato: `main/CMakeLists.txt` define `LUA_32BITS=1` (PRIVATE) e
   `pda_config.c` tem `_Static_assert(sizeof(lua_Number)==4 &&
   sizeof(lua_Integer)==4)` — se alguém atualizar o componente para uma
   versão com o port header público (`port/include/luaconf.h`, adicionado
   no master em 2026-01-09) ou com ABI 64-bit, o assert falha o build com
   a explicação. Regras de fronteira C↔Lua: `docs/LUA.md` §3.

## Regressão do Slint 1.18.1 (motivo do pin em 1.12.1)

Sintomas medidos neste projeto (IDF 5.5.1, esp32p4):

| Build | External RAM `.rodata` | Total image | Desfecho |
|---|---|---|---|
| Slint 1.18.1, UI com emoji/símbolos | 35.370.148 B | 37.026.384 B | `elf2image`: > 16 MB |
| Slint 1.18.1, UI ASCII+Latin-1 | 31.783.096 B | 33.436.960 B | `elf2image`: > 16 MB |
| Slint 1.12.1 (protótipo, com emoji) | — | < 4 MiB | flashava e rodava |

Os símbolos gigantes são `slint_embedded_resource_*_gs_0_gd_N` (glifos de
fonte), com até 2,4 MiB **por glifo** numa UI cujas fontes são 13–26 px.
Sanitizar os caracteres ajudou pouco (-3,6 MiB): o raster/embedding do
1.18.1 infla glifos mesmo para ASCII. Como o 1.12.1 é a versão já validada
neste hardware (protótipo), o pin desce para `==1.12.1` até segunda ordem.
Consequências de API já aplicadas: `viewport-height` (não `content-height`)
e `SlintPlatformConfiguration` sem `panel_type`.

## Regra de ouro do repositório

- **`dependencies.lock` é commitado.** Ele é a garantia de que outro
  checkout/máquina resolve exatamente o conjunto da tabela acima.
- Para atualizar uma dependência: mude o `==`, rode o build, rode no hardware,
  e atualize esta tabela com data e versão.
- Se o resolve começar a falhar do nada: `rm -rf build managed_components
  dependencies.lock sdkconfig` e reconstrua (cache velho de componente).

## Planejado (sem componente novo por enquanto)

- **M4c BLE**: NimBLE/Bluedroid já vêm no ESP-IDF (componente `bt`); o
  transporte HCI usa o `esp_hosted` 2.12.9 já pinado (slave atual já tem BT
  HCI — não precisa reflash do C6). Receita de sdkconfig e escopo em
  `docs/BLUETOOTH.md`.
- **M5 áudio**: drivers I2S/ES8311 são nativos do IDF (`esp_driver_i2s`,
  `esp_codec_dev` opcional). Saída imediata DECIDIDA (2026-09-30): usuário
  não tem o conector JST do falante → **M5a = USB-C UAC** → entra
  `espressif/usb_host_uac` (esp-iot-solution) + decoder (WAV primeiro;
  `espressif/esp_audio_codec` p/ MP3 depois). Ver `docs/ROADMAP.md` M5.
- **App companheiro Android** (`docs/ANDROID_SYNC.md`): HTTP usa
  `esp_http_server` (nativo do IDF) e descoberta usa `espressif/mdns`
  (novo pin quando F0/F1 começar); BLE usa o mesmo NimBLE do M4c. Nada
  entra no `idf_component.yml` antes da fase F0 ser aprovada.

## Fontes embutidas da app (M4.9 — família interna "PDA Mono", peso 400/700)

| asset | origem | peso | nota |
|---|---|---|---|
| `Mono-Base.ttf` | `RobotoMono[wght].ttf` instanciado wght=400, família RENOMEADA p/ "PDA Mono" | ~26 KB | default da app inteira (`default-font-family: "PDA Mono"`) |
| `Mono-Bold.ttf` | idem wght=700 | ~26 KB | bold REAL p/ `font-weight: 700` (títulos, botões) |
| `MaterialIcons-Subset.ttf` | material-design-icons | ~3,4 KB | inalterado |
| `LICENSE-mono.txt` | proveniência + Apache-2.0 integral | — | obrigação da licença; regenere junto com as faces |

Os nomes de arquivo são GENÉRICOS de propósito: trocar Roboto Mono por Fira
Code/JetBrains Mono/etc. é só rodar `tools/font_pipeline.py --mono ...` (ver
`docs/FONTES.md`) — nenhum `.slint` ou C++ muda. As faces proporcionais e os
subsets antigos do M4.7/M4.8 foram REMOVIDOS de assets/ (o pipeline regenera
tudo). Charset: colhido dos literais dos `.slint` (inclui a âncora Latin-1 de
runtime) + pisos ASCII/Latin-1/box; o `verify_face` do pipeline FALHA se um
char usado pela UI não tiver glifo. Avanço 0.600em é contrato do cursor do
editor. Licenças: Apache-2.0 (Roboto Mono) / OFL (alternativas).
**Por que família default explícita**: o slint-compiler embute como
"default" o sans do fontconfig da MÁQUINA DE BUILD; com
`default-font-family: "PDA Mono"` a query resolve nos imports do .slint
(faces custom carregadas antes da query) — independente do host. Ver
docs/UI.md §Fontes e docs/FONTES.md.

## Limitações do Slint 1.12 encontradas em campo (2026-10)

1. **Globals inacessíveis do C++**: nenhum `global<Theme>()` no código
   gerado; propriedades de global só mudam via binding/callback dentro
   do .slint (M5.0 usou `init`/`changed` no AppWindow).
2. **TouchArea sem callbacks pressed/released**: só a propriedade
   `pressed` — usar property-espelho + `changed` (M4.16.1, KeyCap).
3. **Timer**: callback é `triggered`; controle por `running` (não há
   start()/stop() nem `tick`) — M4.16.1.
4. **Layouts**: uma fileira DEPOIS de um irmão de altura fixa dentro de
   VerticalLayout foi mispositioned p/ fora do diálogo (botões do
   prompt sumiam, M4.16.2, provado por bisseção de render). Regra:
   geometria absoluta p/ fileiras críticas; auditar `if`/alturas fixas
   em layouts (suspeitos listados no ROADMAP M5.0b/A5).
5. **`if` em layout + irmãos posteriores**: mesmo family do item 4 —
   o campo do prompt virou Rectangle incondicional (altura 0/visible)
   p/ eliminar a variável.

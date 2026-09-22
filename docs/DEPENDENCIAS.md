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
| `espressif/lua` | `^5.5.0` | **5.5.0** | >= 5.0 | ✅ compila (API 5.5: `lua_newstate` c/ seed, `LUA_RELEASE`) |

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

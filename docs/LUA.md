# Lua no PDA

Lua 5.5 via componente oficial **`espressif/lua`** (MIT, IDF ≥ 5.0, `LUA_32BITS`:
inteiros de 32 bits — evite contadores/timestamps gigantes).

Dois usos distintos:

## 1. Configuração do sistema — `config/system.lua`

Chunk que **retorna uma tabela**. Chaves ausentes = default do firmware.
Valores insanos são clampados (ex.: standby ≥ dim+5 s). A tela Config
reescreve este arquivo ao salvar; editar à mão no cartão também vale
(basta reboot, ou nada: é lido no boot).

```lua
return {
  display = { brightness = 80 },
  power   = { dim_after_s = 30, screen_off_after_s = 120,
              deep_sleep_after_s = 0, wake_on_touch = false,
              boot_btn_standby = false },
  locale  = { timezone = "America/Sao_Paulo", ntp_server = "pool.ntp.org" },
  net     = { wifi_enabled = true },
  ui      = { onscreen_keyboard_auto = true, accent = "cyan",
              cursor = "bar" },
}
```

Chaves aceitas (as mesmas da API `pda.settings.*`):
`display.brightness`, `power.dim_after_s`, `power.screen_off_after_s`,
`power.deep_sleep_after_s`, `power.wake_on_touch` (M4.14.2: gateia SÓ o
wake do STANDBY por toque; toque como atividade é sempre ativo),
`power.boot_btn_standby` (M4.14.4: BOOT em ACTIVE/DIM pede standby),
`power.light_sleep`, `locale.timezone`, `locale.ntp_server`,
`net.wifi_enabled` (M5.3: rádio on/off; NVS `wifion`),
`ui.onscreen_keyboard_auto`, `ui.accent` (M5.0: cyan|violet|green|amber|pink;
sanitize p/ cyan), `ui.cursor` (`"bar"` | `"under"` | `"block"` — glifo do
cursor do editor).

Regras de ordenação (clamp silencioso, `clamp_all`): `screen_off ≥ dim+5`;
`deep ≠ 0 ⇒ deep ≥ screen_off+10` (caso real: deep=40 c/ off=56 virou 66);
a UI ecoa os valores clampados desde a M5.0b. O formato de
`config/wifi.lua` (lista de redes, M5.2) mora em `docs/WIFI.md`.

## 2. Pequenos programas — `scripts/*.lua`

Tocáveis na tela **Scripts** (lista + console). Rodam serializados, com
limite de instruções e de memória; erro vira mensagem no console.

### API `pda.*`

| Função | Retorno | Notas |
|---|---|---|
| `pda.log(...)` | — | ecoa no console da tela + log serial |
| `pda.toast(msg)` | — | mensagem curta (hoje: console) |
| `pda.uptime()` | int | segundos desde o boot |
| `pda.millis()` | int | ms (envolve em ~24,8 d, int32) |
| `pda.version()` | string | versão do firmware |
| `pda.root()` | string | raiz ativa (`/sdcard/pda` ou `/internal/pda`) |
| `pda.sd_mounted()` | bool | |
| `pda.settings.get(k)` | num/bool/string | ver chaves acima |
| `pda.settings.set(k, v)` | — | marca sujo; reaplica brilho na hora |
| `pda.settings.save()` | bool | persiste em `config/system.lua` |
| `pda.fs.read(p)` | string \| nil,err | |
| `pda.fs.write(p, s)` | bool \| nil,err | sobrescreve |
| `pda.fs.append(p, s)` | bool | |
| `pda.fs.list(dir)` | table \| nil,err | diretórios com sufixo `/` |
| `pda.fs.exists(p)` | bool | |
| `pda.fs.size(p)` | int | -1 se não existe |
| `pda.fs.remove(p)` | bool | |

**Caminhos**: relativos à raiz ativa (`"notes/x.txt"`) ou absolutos
(`/sdcard/...`, `/internal/...`). `..` é rejeitado.

### Exemplos

Ver `sdcard-template/pda/scripts/`: `exemplo.lua` (tour pela API) e
`organiza_notas.lua` (gera `notes/_indice.md`).

### O que Lua NÃO faz (por decisão de escopo, ver roadmap)

Criar telas/widgets próprios (binding Slint↔Lua), spawnar outros scripts,
acessar rede/áudio (chegam em M4/M5 como novas funções `pda.net.*`/`pda.audio.*`).

## 3. Fronteira C↔Lua — ABI 32 bits (LER ANTES DE TOCAR EM `lua.h`)

O componente `espressif/lua` 5.5.0 compila a VM com `LUA_32BITS=1`
(`lua_Number` = `float`, `lua_Integer` = `int32`), mas o define é
**PRIVATE** no CMakeLists do componente — ele NÃO chega aos headers de quem
consome. Sem compensação, o `main` compila com `double`/`int64` e o link
casa duas ABIs diferentes (bug público: espressif/developer-portal
discussion #188). Foi a causa raiz do mistério "INT_MAX no system.lua"
(boots de 2026-09-24..30): `lua_tonumber` devolvia `float` nos 32 bits
baixos de `fa0`, o caller lia `double` → lixo finito → `(int)` saturava em
2147483647 no RISC-V. `docs/HARDWARE.md` (achado #9) tem a autópsia
completa.

Regras do repositório:

1. **`main/CMakeLists.txt` define `LUA_32BITS=1` (PRIVATE).** Não remover.
   O `_Static_assert(sizeof(lua_Number)==4 && sizeof(lua_Integer)==4)` no
   topo de `pda_config.c` falha o build se a premissa mudar (ex.: upgrade
   do componente para ABI diferente).
2. **Prefira o caminho inteiro ao cruzar a fronteira**: `lua_tointegerx` /
   `lua_pushinteger` / `luaL_checkinteger`. `tbl_int` do `pda_config.c` é
   integer-first de propósito (defesa em profundidade). Só use
   `lua_tonumber`/`lua_pushnumber` quando fração for realmente necessária —
   e teste em hardware, não só no host (o Lua 5.4 do x86 é 64 bits e NÃO
   reproduz o bug de ABI).
3. **Nunca usar `LUA_REGISTRYINDEX`, `lua_upvalueindex`, `luaL_ref` ou
   `luaL_Buffer` sem o define do item 1** — `LUAI_MAXSTACK` também muda com
   `LUA_32BITS`, e pseudo-índices calculados com o valor errado leem slots
   aleatórios da stack. (Hoje o código não usa nenhum dos quatro.)
4. Testes de host (`tools/hosttest/`) rodam com o Lua 5.4 do sistema
   (64 bits) — eles validam LÓGICA (round-trip, cura, shadow), não ABI.
   O `parser self-test` no boot é quem valida a ABI no alvo.

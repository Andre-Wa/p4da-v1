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
              deep_sleep_after_s = 0, wake_on_touch = false },
  locale  = { timezone = "America/Sao_Paulo", ntp_server = "pool.ntp.org" },
  ui      = { onscreen_keyboard_auto = true },
}
```

Chaves aceitas (as mesmas da API `pda.settings.*`):
`display.brightness`, `power.dim_after_s`, `power.screen_off_after_s`,
`power.deep_sleep_after_s`, `power.wake_on_touch`, `locale.timezone`,
`locale.ntp_server`, `ui.onscreen_keyboard_auto`, `ui.scale`
(1.00 = 100%; multiplica tokens de tamanho/espaço do Theme; 0.90–1.50).

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

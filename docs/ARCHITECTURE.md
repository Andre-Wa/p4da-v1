# Arquitetura do sistema (M1)

## Visão em camadas

```
┌────────────────────────────────────────────────────────────┐
│ UI (Slint, C++)            main/ui/app_ui.slint            │
│  status bar · launcher · arquivos · notas · scripts · cfg  │
├────────────────────────────────────────────────────────────┤
│ Orquestração (main.cpp)                                    │
│  threads de I/O · invoke_from_event_loop · roteamento      │
├──────────────┬──────────────┬──────────────┬───────────────┤
│ pda_config   │ lua_runtime  │ power_mgmt   │ storage_init  │
│ settings em  │ VM Lua 5.5 + │ degraus de   │ SD 4-bit +    │
│ Lua (SD/flash│ API pda.*    │ energia      │ LittleFS +    │
│ espelho)     │ (protegida)  │ (light/deep) │ raiz /pda     │
├──────────────┴──────────────┴──────────────┴───────────────┤
│ BSP mínimo: display_init · touch_init · usb_hid_keyboard   │
│ (pinagem reconciliada em board_config.h)                   │
└────────────────────────────────────────────────────────────┘
```

## Modelo de armazenamento

- `/internal` — LittleFS (partição `storage`, 8 MB). Sempre montado.
- `/sdcard` — FatFS via SDMMC 4-bit @ 40 MHz. Montado se houver cartão.
- **Raiz lógica do sistema**: `pda_root()` = `/sdcard/pda` com cartão,
  `/internal/pda` sem. Toda a camada de cima usa `pda_path()` e nunca
  hardcoded um mount específico.
- Árvore semeada no boot: `config/ scripts/ notes/ music/ logs/ .state/`.
- **Espelho de settings**: `pda_config_save()` grava na raiz ativa e, em
  melhor esforço, na outra raiz montada. Primeira vez com cartão novo:
  settings do interno são promovidas (copiadas) para o cartão.
- Sem hot-plug em M1: o cartão é lido no boot. (Remontagem a quente = M3.)

## Regras de threading (não negociáveis)

1. **UI nunca bloqueia**: toda I/O de disco acontece em `std::thread`
   destacada e o resultado volta via `slint::invoke_from_event_loop`.
2. **VM Lua é single-thread**: execuções serializadas por `g_lua_mtx`
   (uma de cada vez); scripts rodam em thread própria para não travar a UI.
3. **Mutação de propriedades Slint só no event loop** (regra 1 é o mecanismo).
4. `power_mgmt_activity()` deve ser chamado em todo handler de UI/teclado —
   é o que segura os degraus de energia.

## Proteção da VM Lua

- `lua_pcall` sempre → erro de script vira mensagem, não panic.
- Hook de contagem: `PDA_LUA_INSTRUCTION_LIMIT` (20M) mata loop infinito.
- Allocator próprio com teto `PDA_LUA_MEM_LIMIT_BYTES` (2 MB) mata runaway alloc.
- Caminhos: relativos à raiz ativa ou absolutos `/sdcard|/internal`; `..` rejeitado.
- Config parseada em **estado Lua temporário separado**: script quebrado não
  corrompe settings; settings inválidas caem em defaults com clamp de sanidade
  (ex.: `screen_off_after_s` nunca menor que `dim_after_s + 5`).

## Fonte da verdade das configurações

Struct C `pda_settings_t` em RAM. O arquivo `config/system.lua` é a
serialização legível (chunk que retorna tabela). UI e scripts alteram via
`pda_settings_set()`; persistência explícita via `pda_config_save()`
(botão Salvar / `pda.settings.save()`).

## O que deliberadamente NÃO está em M1

teclado virtual (M2), editor com cursor/scroll horizontal (M2), remontagem
hot-plug do SD (M3), Wi-Fi/BT/NTP/OTA (M4), áudio/player (M5), apps
Agenda/Contatos/Relógio (M6). Botões cinzas no launcher = contrato visual do
roadmap (`docs/ROADMAP.md`).

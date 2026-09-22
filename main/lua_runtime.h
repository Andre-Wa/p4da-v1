#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runtime Lua do PDA (componente oficial espressif/lua, Lua 5.5).
 *
 * Um lua_State persistente executa os "pequenos programas" do usuário
 * (/pda/scripts/*.lua). A configuração do sistema (/pda/config/system.lua)
 * é parseada separadamente pela pda_config (estado temporário), então um
 * script quebrado nunca corrompe as settings.
 *
 * Proteções: limite de instruções por execução (loops infinitos), limite
 * de memória alocada pela VM e erros sempre capturados (lua_pcall) e
 * devolvidos como mensagem — script ruim não derruba o firmware.
 *
 * API exposta aos scripts (ver docs/LUA.md):
 *   pda.log(...)  pda.toast(msg)  pda.uptime()  pda.version()
 *   pda.root()  pda.sd_mounted()
 *   pda.settings.get(k) / pda.settings.set(k,v) / pda.settings.save()
 *   pda.fs.read(p) / pda.fs.write(p,s) / pda.fs.append(p,s)
 *   pda.fs.list(dir) / pda.fs.exists(p) / pda.fs.size(p) / pda.fs.remove(p)
 */

/** Teto de instruções por execução de script (~20M = alguns segundos). */
#define PDA_LUA_INSTRUCTION_LIMIT 20000000
/** Teto de memória que a VM Lua pode alocar (PSRAM). */
#define PDA_LUA_MEM_LIMIT_BYTES   (2 * 1024 * 1024)

typedef void (*pda_lua_log_fn)(const char *line, void *ctx);
typedef void (*pda_lua_toast_fn)(const char *msg, void *ctx);

esp_err_t lua_runtime_init(void);

void lua_runtime_set_log_callback(pda_lua_log_fn fn, void *ctx);
void lua_runtime_set_toast_callback(pda_lua_toast_fn fn, void *ctx);
/** Chamado quando um script altera settings (p/ UI reaplicar brilho etc.). */
void lua_runtime_set_settings_changed_cb(void (*cb)(void));

/** Executa um arquivo .lua com as proteções acima.
 *  err_msg recebe a mensagem de erro Lua (inclui linha) se falhar. */
esp_err_t lua_runtime_run_file(const char *path, char *err_msg, size_t err_sz);

/** Idem, para um buffer em memória (usado p/ testar snippets da UI). */
esp_err_t lua_runtime_run_string(const char *src, char *err_msg, size_t err_sz);

/** Empilha uma linha no sink de log (usado pelo C para ecoar no console
 *  da tela de Scripts). */
void lua_runtime_emit_log(const char *line);

#ifdef __cplusplus
}
#endif

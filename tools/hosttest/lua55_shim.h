#pragma once
/* lua55_shim.h — shim p/ compilar lua_runtime.c no HOST (v5.16.2: movido
 * de /tmp/lrstub p/ dentro do repo — o teste LUA-BIND do run.sh precisa
 * dele e /tmp não sobrevive entre sessões).
 *
 *  - O Lua 5.4 do host declara lua_newstate com 2 args; o alvo usa Lua 5.5
 *    (componente espressif/lua), onde ganhou 3º param (seed do hash de
 *    strings). A macro casa a chamada de 3 args do lua_runtime.c com a
 *    declaração de 2 (só em teste de host — ABI de verdade é validada no
 *    alvo pelo parser self-test, docs/LUA.md §3).
 *  - O stub esp_err.h do repo não define ESP_ERR_INVALID_STATE (uso
 *    pré-existente em lua_runtime.c: run_protected sem VM inicializada). */
#define lua_newstate(f, ud, seed) lua_newstate((f), (ud))
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE 0x103
#endif

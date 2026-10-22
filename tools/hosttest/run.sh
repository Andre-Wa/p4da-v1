#!/usr/bin/env bash
# Testes de host do pda_config.c real (stub de ESP + Lua do sistema).
# Uso: tools/hosttest/run.sh
set -e
cd "$(dirname "$0")/../.."

# raízes fake: /sdcard e /internal apontam p/ /tmp/cfgtest (censo/stat reais)
rm -rf /tmp/cfgtest
mkdir -p /tmp/cfgtest/sdcard/pda/config /tmp/cfgtest/internal/pda/config
for link in /sdcard /internal; do
    [ -e "$link" ] && [ ! -L "$link" ] && { echo "ERRO: $link existe e não é symlink"; exit 2; }
    ln -sfn "/tmp/cfgtest${link}" "$link"
done

LUA_INC="${LUA_INC:-/usr/include/lua5.4}"
LUA_LIB="${LUA_LIB:-lua5.4}"

gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I tools/hosttest/stubs -I main -I "$LUA_INC" \
    main/pda_config.c tools/hosttest/host_cfg_test.c \
    -l"$LUA_LIB" -lm -o /tmp/cfgtest_bin

echo "--- rodando /tmp/cfgtest_bin ---"
/tmp/cfgtest_bin

# ---- BAT.2: máquina de estados de bateria (battery.c real, sem IDF) ----
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -DBATTERY_UNIT_TEST -I tools/hosttest/stubs -I main \
    main/battery.c tools/hosttest/host_batt_test.c \
    -o /tmp/batt_bin

echo "--- rodando /tmp/batt_bin ---"
/tmp/batt_bin

# ---- UI-LOOP-GATE (v5.16.0): ui_gate.h real, invoker mock (crash da 15ª
# rodada: invoke_from_event_loop antes do loop = assert xTaskToNotify) ----
g++ -std=c++17 -Wall -Wextra -pthread -I main \
    tools/hosttest/host_uigate_test.cpp -o /tmp/uigate_bin

echo "--- rodando /tmp/uigate_bin ---"
/tmp/uigate_bin

# ---- LUA-BIND (v5.16.2): lua_runtime.c REAL no Lua 5.4 do host (shim
# lua55_shim.h casa o lua_newstate de 3 args da 5.5) + o script REAL
# log_bateria.lua rodado end-to-end contra stubs de bateria/storage/
# relógio — o CSV do logger nasce e é afirmado aqui, não só no alvo. ----
rm -rf /tmp/luabind
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -include tools/hosttest/lua55_shim.h \
    -I tools/hosttest/stubs -I main -I "$LUA_INC" \
    main/lua_runtime.c tools/hosttest/host_lua_test.c \
    -l"$LUA_LIB" -lm -o /tmp/luabind_bin

echo "--- rodando /tmp/luabind_bin ---"
/tmp/luabind_bin

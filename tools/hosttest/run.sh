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

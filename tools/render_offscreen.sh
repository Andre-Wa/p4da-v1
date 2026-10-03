#!/usr/bin/env bash
# render_offscreen.sh — valida a UI SEM hardware (mesmo backend do device).
#
# Compila main/ui/app_ui.slint com o slint-compiler 1.12.1 em modo
# `embed-for-software-renderer`, linka com o Slint-cpp de Linux e renderiza
# os 8 modos do harness (reading, reading2, edit, prompt, panel, paneldrag,
# coltest, fontpitch) para PNG na raiz do repo.
#
# Pré-requisitos no host (Linux x86_64):
#   - slint-compiler 1.12.1   (release do GitHub, binário único)
#   - Slint-cpp 1.12.1        (tarball Linux-x86_64: include/slint + libslint_cpp.so)
#   - g++ C++20, Qt6/xkb/gbm (apt: qt6-base-dev libxkbcommon-dev libgbm-dev)
#   - python3 com PIL (só p/ converter PPM->PNG)
#
# Uso:
#   SLINT_COMPILER=/caminho/slint-compiler SLINT_CPP=/caminho/Slint-cpp-1.12.1 \
#     tools/render_offscreen.sh
#
# No sandbox do projeto os padrões já apontam para /tmp (baixados uma vez).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLINT_COMPILER="${SLINT_COMPILER:-/tmp/slint-compiler}"
SLINT_CPP="${SLINT_CPP:-/tmp/Slint-cpp-1.12.1-Linux-x86_64}"
WORK="${WORK:-/tmp/harness}"

[ -x "$SLINT_COMPILER" ] || { echo "ERRO: slint-compiler não achado em $SLINT_COMPILER"; exit 1; }
[ -f "$SLINT_CPP/lib/libslint_cpp.so" ] || { echo "ERRO: libslint_cpp.so não achada em $SLINT_CPP/lib"; exit 1; }

mkdir -p "$WORK"
cp "$REPO/tools/offscreen_render_harness.cpp" "$WORK/main.cpp"
cd "$WORK"

echo "== slint-compiler (embed p/ software renderer) =="
"$SLINT_COMPILER" --embed-resources embed-for-software-renderer \
    -o gen.h --cpp-file gen_a.cpp --cpp-file gen_b.cpp \
    "$REPO/main/ui/app_ui.slint"

echo "== g++ =="
g++ -std=c++20 -O1 -I"$SLINT_CPP/include/slint" -I"$REPO/main" -I. -c gen_a.cpp -o gen_a.o
g++ -std=c++20 -O1 -I"$SLINT_CPP/include/slint" -I"$REPO/main" -I. -c gen_b.cpp -o gen_b.o
g++ -std=c++20 -O1 -I"$SLINT_CPP/include/slint" -I"$REPO/main" -I. \
    main.cpp gen_a.o gen_b.o -o harness \
    -L"$SLINT_CPP/lib" -lslint_cpp -Wl,-rpath,"$SLINT_CPP/lib"

echo "== render =="
# coltest/fontpitch = sondas M4.13 do pitch de glifo (drift do cursor-bloco):
# coltest compara overlay vetorial vs glifo in-flow na col 43; fontpitch mede
# o avanço real por (face,tamanho) com 24 'M' idênticos por estilo.
for mode in reading reading2 edit prompt panel paneldrag coltest fontpitch; do
    ./harness "$mode"
    python3 - "$mode" "$REPO" <<'PY'
import sys
from PIL import Image
mode, repo = sys.argv[1], sys.argv[2]
Image.open(f"/tmp/harness/render_{mode}.ppm").save(f"{repo}/render_{mode}.png")
print(f"  -> {repo}/render_{mode}.png")
PY
done
echo "== ok: abra render_reading.png / render_edit.png / render_prompt.png =="

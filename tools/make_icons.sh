#!/usr/bin/env bash
# Regenera o subset de Material Icons usado pela UI.
# Adicionar ícone novo: 1) ponha o nome em tools/icons.json (mesma forma do
# codepoints oficial); 2) rode este script; 3) crie o wrapper em
# main/ui/elements/icons.slint; 4) rebuild.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=/tmp/MaterialIcons-Regular.ttf
[ -f "$SRC" ] || curl -sfL -o "$SRC" \
  https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf
UNIS=$(python3 -c "import json;print(','.join('U+'+v for v in json.load(open('tools/icons.json')).values()))")
pyftsubset "$SRC" --unicodes="$UNIS" \
  --output-file=main/ui/assets/MaterialIcons-Subset.ttf \
  --layout-features= --no-hinting --desubroutinize --name-IDs=1,2,3,4,5,6
ls -l main/ui/assets/MaterialIcons-Subset.ttf

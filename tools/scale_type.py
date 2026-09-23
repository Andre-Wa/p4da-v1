#!/usr/bin/env python3
"""Escala TODOS os tokens estáticos do theme.slint (exige rebuild).

Uso:  python3 tools/scale_type.py 1.15     # aumenta 15%
      python3 tools/scale_type.py 0.87     # volta ao tamanho anterior (1/1.15)

Por que compile-time e não runtime:
  - o software renderer do Slint pré-rasteriza glifos por font-size estático
    (font-size dinâmico => todos os textos num tamanho único);
  - tokens mutáveis em runtime já causaram bootloop por NaN (docs/POWER.md).
O fator é aplicado sobre os valores ATUAIS do arquivo; para escala absoluta,
restaure o theme (git checkout) antes de rodar.
"""
import re, sys, pathlib

f = pathlib.Path(__file__).resolve().parent.parent / "main/ui/theme.slint"
factor = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
s = f.read_text()

PAT = re.compile(
    r"(in property <length> (?:type-[\w]+|key-fsize|shape-[\w]+|sp-[\w]+|"
    r"btn-h|btn-h-sm|row-h|line-h|status-h|osk-h)): (\d+(?:\.\d+)?)px;")

def bump(m):
    return f"{m.group(1)}: {max(1, round(float(m.group(3)) * factor))}px;"

s2, n = PAT.subn(bump, s)
f.write_text(s2)
print(f"{n} tokens x{factor} -> idf.py build")
print("Lembre: main.cpp ED_LINE_H/ED_OSK_H/ED_STATUS_H/ED_BTN_SM/ED_CHAR_W espelham o theme.")

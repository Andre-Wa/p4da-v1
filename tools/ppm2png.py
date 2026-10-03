#!/usr/bin/env python3
"""ppm2png.py — converte os PPM do harness offscreen em PNG na raiz do repo.

Uso: python3 tools/ppm2png.py [dir_harness] [dir_saida]
Padrão: /tmp/harness -> raiz do repo (evidência visual da rodada).
"""
import glob
import os
import sys

from PIL import Image

src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/harness"
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for p in sorted(glob.glob(os.path.join(src, "render_*.ppm"))):
    out = os.path.join(dst, os.path.basename(p).replace(".ppm", ".png"))
    Image.open(p).save(out)
    print(out)

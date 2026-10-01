#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
font_pipeline.py — gera as fontes monoespaçadas EMBEDDADAS do p4da.

Por que isto existe
-------------------
O Slint 1.12 pré-rasteriza os glifos em tempo de COMPILAÇÃO a partir de
`@font-face`/`import` com string literal. O charset é global: o compilador usa a
união de todas as strings literais do projeto + a propriedade `charset:` (fonte
compartilhada). Então o TTF embutido precisa cobrir exatamente o charset que a
UI usa — nem mais (incha o binário), nem menos (glifo ausente = retângulo/tofu
ou, pior, fallback silencioso para a fonte do sistema, que não existe no alvo).

O que este script faz
---------------------
1. Carrega a fonte fonte (`--mono`), variável ou estática.
2. Se for variável, instancia no peso pedido (`--weight`, `--bold-weight`).
3. Renomeia a família para um nome INTERNO fixo (`--family`, padrão "PDA Mono"),
   para que trocar de fonte não exija mudar NENHUM arquivo .slint.
4. Remove ligaturas (`calt`/`liga`/`dlig`/`clig`) por padrão — ver aviso abaixo.
5. Faz o subsetting pelo charset do projeto (mantendo as tabelas de métricas,
   kerning e o mapa Unicode).
6. Grava `Mono-Base.ttf`, `Mono-Bold.ttf` e `LICENSE-mono.txt` em `--out-dir`.

AVISO — ligaturas x cursor do editor
------------------------------------
Fontes como Fira Code/JetBrains Mono trazem `calt` (ligaturas: `=>` `->` `!=`
`>=` viram um único glifo). O Slint shapeia com rustybuzz, que aplica `calt`
por padrão. No editor, o cursor ocupa o SLOT do caractere (1 char = 1 slot);
com ligaturas, nº de glifos < nº de chars e o cursor desalinha. Por isso o
padrão é REMOVER as features de ligatura. Use `--keep-ligatures` apenas se
você aceitar esse efeito colateral (ou depois que o cursor passar a usar
índices de glifo).

Uso
---
  # Roboto Mono peso 400 (padrão do projeto)
  python tools/font_pipeline.py --mono /tmp/RobotoMono-VF.ttf --weight 400

  # Fira Code (estática, já baixada no peso certo)
  python tools/font_pipeline.py --mono /tmp/FiraCode-400.ttf --weight 400 \
      --bold-src /tmp/FiraCode-700.ttf

  # experimentar outro peso sem trocar de fonte
  python tools/font_pipeline.py --mono /tmp/RobotoMono-VF.ttf --weight 450

Dependências: fonttools + brotli  (pip install fonttools brotli)
Licenças: Roboto Mono = Apache 2.0; Fira Code = SIL OFL 1.1; JetBrains Mono =
SIL OFL 1.1. Sempre acompanhe o arquivo de licença em assets/LICENSE-mono.txt.
"""

import argparse
import io
import os
import re
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

# ---------------------------------------------------------------- charset ---
# Precisa ser SUPERSET do `charset:` da âncora em main/ui/app_ui.slint.
# Regra: se aparecer um glifo novo em string dinâmica, adicione-o AQUI e na
# âncora (a âncora é o que garante o glifo em strings formadas em runtime).
BASE_CHARSET = "".join(chr(c) for c in range(0x20, 0x7F)) + \
    "".join(chr(c) for c in range(0xA0, 0x100))  # Latin-1 suplementar (acentos pt-BR)
EXTRA_CHARS = "…—–·×÷→←↑↓■≠≈∞"          # reticências, travessões, setas, blocos, matemáticos
BOX_DRAWING = "│─┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬"   # desenho de caixa (ASCII art em notas)
ICON_FALLBACK = "☰⚙🖫"                    # só entram se a fonte tiver (fallback raro)

# features de ligatura que quebram o mapeamento 1:1 char↔slot do editor
LIGATURE_FEATURES = {"calt", "liga", "dlig", "clig", "rlig"}

# layout mínimo que vale a pena preservar (métricas + posicionamento)
KEEP_LAYOUT = {
    "kern", "mark", "mkmk", "ccmp", "locl", "tnum", "lnum",
    "smcp", "sups", "subs", "frac", "numr", "dnom", "onum", "pnum",
}


LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def unescape(body: str) -> str:
    """Desfaz os escapes de literal do Slint (\n, \", \\u{2026}...)."""
    out = []
    i = 0
    while i < len(body):
        ch = body[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        i += 1
        if i >= len(body):
            break
        esc = body[i]
        if esc == "u" and i + 1 < len(body) and body[i + 1] == "{":
            end = body.find("}", i + 2)
            if end == -1:
                break
            try:
                out.append(chr(int(body[i + 2:end], 16)))
            except ValueError:
                pass
            i = end + 1
            continue
        out.append({"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}.get(esc, esc))
        i += 1
    return "".join(out)


def strip_comments(text: str) -> str:
    """Remove comentários // e /* */ sem tocar no conteúdo de strings.

    Sem isto, um comentário como  // "█" não existe na Roboto  vira charset
    obrigatório e a validação acusa falha falsa.
    """
    out = []
    i, n, in_str = 0, len(text), False
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1]); i += 2; continue
            if ch == '"':
                in_str = False
            i += 1; continue
        if ch == '"':
            in_str = True; out.append(ch); i += 1; continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i); i = n if j == -1 else j; continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2); i = n if j == -1 else j + 2; continue
        out.append(ch); i += 1
    return "".join(out)


def harvest_ui_charset(ui_dir: str) -> tuple:
    """
    Colhe TODO caractere usado em literais de string nos .slint.

    Isso inclui automaticamente o `text:` da âncora de rasterização em
    app_ui.slint (Latin-1 suplementar + tipográficos), que é o que garante
    acentos em textos vindos do C++ em runtime. Também pega os codepoints
    privados dos ícones (Material Icons) — estes não existem na fonte mono e
    são ignorados no subsetting (ignore_missing_glyphs).
    """
    chars = set()
    files = sorted(str(f) for f in Path(ui_dir).rglob("*.slint"))
    for path in files:
        text = strip_comments(Path(path).read_text(encoding="utf-8", errors="replace"))
        for m in LITERAL_RE.finditer(text):
            chars |= set(unescape(m.group(1)))
    pua = {c for c in chars if 0xE000 <= ord(c) <= 0xF8FF}
    return chars - pua, pua, files


def verify_face(path: str, charset: str, critical: str = "") -> None:
    """Confere cobertura da face gerada contra o charset colhido."""
    font = TTFont(path)
    cmap = font.getBestCmap()
    missing = sorted(c for c in charset if ord(c) not in cmap and not c.isspace())
    name = font["name"].getDebugName(1)
    sub = name and None
    weight = font["OS/2"].usWeightClass if "OS/2" in font else -1
    upem = font["head"].unitsPerEm
    adv = font["hmtx"][font.getBestCmap().get(ord("M"), ".notdef")][0] if ord("M") in cmap else 0
    print(f"  {Path(path).name}: família='{name}' subfamília='{font['name'].getDebugName(17) or font['name'].getDebugName(2)}' "
          f"peso={weight} upem={upem} adv(M)={adv} ({adv/upem:.3f}em) "
          f"glifos={len(cmap)} cobertura={100*(len(charset)-len(missing))/max(1,len(charset)):.1f}%")
    hard = [c for c in missing if c in critical]
    soft = [c for c in missing if c not in critical]
    if hard:
        hexes = " ".join(f"{c}(U+{ord(c):04X})" for c in hard)
        raise SystemExit(f"ERRO: a face não cobre {len(hard)} chars USADOS PELA UI -> {hexes}\n"
                         "       Troque de fonte ou remova esses caracteres dos .slint.")
    if soft:
        print(f"    opcionais ausentes ({len(soft)}, a fonte não os tem — ok): "
              + "".join(soft))
    # sanity: acentos de runtime PRECISAM existir (âncora Latin-1)
    critical = "ÀÁÂÃÇÉÊÍÓÔÕÚàáâãçéêíóôõú"
    bad = [c for c in critical if ord(c) not in cmap]
    if bad:
        raise SystemExit(f"ERRO: face sem acentos críticos {''.join(bad)} — "
                         "textos em português virariam tofu em runtime")


def build_charset(extra: str = "") -> str:
    chars = set(BASE_CHARSET) | set(EXTRA_CHARS) | set(BOX_DRAWING) | set(ICON_FALLBACK)
    chars |= set(extra or "")
    return "".join(sorted(chars))


def varfont_instance(src: str, weight: float) -> TTFont:
    """Instancia uma fonte variável no peso pedido (retorna None se não for VF)."""
    from fontTools.varLib import instancer

    font = TTFont(src)
    if "fvar" not in font:
        return None
    axes = {a.axisTag: a.defaultValue for a in font["fvar"].axes}
    if "wght" not in axes:
        raise SystemExit("fonte variável sem eixo 'wght' — não dá para instanciar peso")
    lo = min(a.minValue for a in font["fvar"].axes if a.axisTag == "wght")
    hi = max(a.maxValue for a in font["fvar"].axes if a.axisTag == "wght")
    if not (lo <= weight <= hi):
        raise SystemExit(f"peso {weight} fora do intervalo da fonte ({lo}..{hi})")
    axes["wght"] = weight
    return instancer.instantiateVariableFont(font, axes, inplace=True)


def normalize_names(font: TTFont, family: str, subfamily: str, weight: int) -> None:
    """
    Renomeia a família para o nome INTERNO do projeto.

    Motivo: o Slint resolve `default-font-family` em tempo de compilação contra a
    família real da fonte embutida. Fixando "PDA Mono", trocar Roboto→Fira Code
    não exige editar nenhum .slint — só rodar este script de novo.
    """
    name = font["name"]
    ps = (family + "-" + subfamily).replace(" ", "")
    full = f"{family} {subfamily}".strip()
    # ID 1/2 = família/subfamília legadas (o par precisa ser Regular/Bold/Italic)
    legacy_sub = "Bold" if weight >= 650 else "Regular"
    name.setName(family, 1, 3, 1, 0x409)
    name.setName(legacy_sub, 2, 3, 1, 0x409)
    name.setName(full, 4, 3, 1, 0x409)
    name.setName(ps, 6, 3, 1, 0x409)
    # ID 16/17 = família/subfamília tipográficas (peso real)
    name.setName(family, 16, 3, 1, 0x409)
    name.setName(subfamily, 17, 3, 1, 0x409)
    # usWeightClass = peso real da instância (é o que o match_font compara)
    if "OS/2" in font:
        font["OS/2"].usWeightClass = int(weight)


def strip_ligatures(font: TTFont) -> list:
    """Remove as features de ligatura do GSUB (mantém o resto do shaping)."""
    if "GSUB" not in font:
        return []
    gsub = font["GSUB"].table
    removed = []
    if gsub.FeatureList:
        keep_records = []
        for rec in gsub.FeatureList.FeatureRecord:
            if rec.FeatureTag in LIGATURE_FEATURES:
                removed.append(rec.FeatureTag)
            else:
                keep_records.append(rec)
        gsub.FeatureList.FeatureRecord = keep_records
        gsub.FeatureList.FeatureCount = len(keep_records)
    return sorted(set(removed))


def subset_font(font: TTFont, charset: str, keep_ligatures: bool) -> bytes:
    options = subset.Options()
    options.layout_features = sorted(KEEP_LAYOUT | (LIGATURE_FEATURES if keep_ligatures else set()))
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.name_languages = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True
    options.recalc_timestamp = False
    options.prune_unicode_ranges = True
    options.ignore_missing_glyphs = True
    options.ignore_missing_unicodes = True
    options.hinting = True
    options.legacy_kern = True
    options.symbol_cmap = True
    options.legacy_cmap = True
    options.drop_tables += ["DSIG"]

    stream = io.BytesIO()
    font.save(stream)
    stream.seek(0)

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=charset)
    src = TTFont(stream)
    subsetter.subset(src)

    out = io.BytesIO()
    src.save(out)
    return out.getvalue()


def weight_name(w: float) -> str:
    table = {100: "Thin", 200: "ExtraLight", 300: "Light", 350: "SemiLight",
             400: "Regular", 450: "Book", 500: "Medium", 600: "SemiBold",
             700: "Bold", 800: "ExtraBold", 900: "Black"}
    return table.get(int(w), f"W{int(w)}")


def process(src_path: str, weight: float, family: str, charset: str,
            keep_ligatures: bool, out_path: str, label: str) -> None:
    font = varfont_instance(src_path, weight)
    instanced = font is not None
    if font is None:
        font = TTFont(src_path)
        real = font["OS/2"].usWeightClass if "OS/2" in font else 0
        if abs(real - weight) > 5:
            print(f"  aviso: {os.path.basename(src_path)} é estática com peso {real} "
                  f"(pedido {int(weight)}) — usando como está", file=sys.stderr)

    removed = [] if keep_ligatures else strip_ligatures(font)
    normalize_names(font, family, weight_name(weight), int(weight))
    data = subset_font(font, charset, keep_ligatures)

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"{label}: {len(data)/1024:.0f} KB -> {out_path}")
    print(f"    origem={os.path.basename(src_path)} "
          f"({'variável instanciada' if instanced else 'estática'}) "
          f"peso={int(weight)} família='{family}' subfamília='{weight_name(weight)}'")
    if removed:
        print(f"    ligaturas removidas: {', '.join(removed)} (protege o cursor 1:1 do editor)")


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mono", required=True,
                    help="TTF/OTF de origem da fonte monoespaçada (variável ou estática)")
    ap.add_argument("--bold-src", default=None,
                    help="origem separada para o negrito (padrão: mesmo arquivo, --bold-weight)")
    ap.add_argument("--weight", type=float, default=400,
                    help="peso da face base (padrão 400)")
    ap.add_argument("--bold-weight", type=float, default=700,
                    help="peso da face negrito (padrão 700)")
    ap.add_argument("--family", default="PDA Mono",
                    help="nome INTERNO da família (padrão 'PDA Mono' — não precisa mudar)")
    ap.add_argument("--extra-chars", default="",
                    help="caracteres extras para incluir no subsetting")
    ap.add_argument("--keep-ligatures", action="store_true",
                    help="mantém calt/liga/dlig (NÃO recomendado: desalinha o cursor do editor)")
    ap.add_argument("--out-dir", default=os.path.join(repo, "main", "ui", "assets"),
                    help="diretório de destino (padrão main/ui/assets)")
    ap.add_argument("--license", dest="license_src", default=None,
                    help="texto integral da licença da fonte de origem "
                         "(gravado em assets/LICENSE-mono.txt com proveniência)")
    ap.add_argument("--license-url", default=None,
                    help="URL da licença, registrada na proveniência")
    ap.add_argument("--scan-ui", default=os.path.join(repo, "main", "ui"),
                    help="pasta dos .slint para colher o charset (padrão main/ui)")
    ap.add_argument("--no-scan", action="store_true",
                    help="não colhe dos .slint (usa só o charset embutido — NÃO recomendado)")
    args = ap.parse_args()

    if not os.path.exists(args.mono):
        raise SystemExit(f"não achei a fonte: {args.mono}")
    os.makedirs(args.out_dir, exist_ok=True)

    harvested, pua, files = ((set(), set(), []) if args.no_scan
                             else harvest_ui_charset(args.scan_ui))
    print(f"scan de UI: {len(files)} arquivos .slint -> {len(harvested)} chars em literais "
          f"(+{len(pua)} codepoints privados de ícone, ignorados aqui)")
    charset = build_charset(args.extra_chars) + "".join(sorted(harvested))
    charset = "".join(sorted(set(charset)))
    print(f"charset final: {len(charset)} caracteres")

    process(args.mono, args.weight, args.family, charset, args.keep_ligatures,
            os.path.join(args.out_dir, "Mono-Base.ttf"), "base  ")
    process(args.bold_src or args.mono, args.bold_weight, args.family, charset,
            args.keep_ligatures, os.path.join(args.out_dir, "Mono-Bold.ttf"), "negrito")

    # Licença: proveniência + texto integral (obrigação da Apache-2.0 e da OFL).
    lic_path = os.path.join(args.out_dir, "LICENSE-mono.txt")
    provenance = (
        "PDA Mono — fontes embutidas do p4da (main/ui/assets/Mono-Base.ttf, Mono-Bold.ttf)\n"
        "\n"
        f"Família interna : {args.family}\n"
        f"Fonte de origem : {os.path.basename(args.mono)} (peso base {int(args.weight)})\n"
        f"Negrito de      : {os.path.basename(args.bold_src or args.mono)} (peso {int(args.bold_weight)})\n"
        f"Licença de origem: {args.license_url or 'informe com --license-url'}\n"
        f"Gerado por      : tools/font_pipeline.py (fontTools instancer + pyftsubset)\n"
        "Comando         : python tools/font_pipeline.py --mono "
        f"{os.path.basename(args.mono)} --weight {int(args.weight)}\n"
        "\n"
        "---------------------------------------------------------------------\n\n"
    )
    if args.license_src and os.path.exists(args.license_src):
        with open(args.license_src, "r", encoding="utf-8", errors="replace") as src:
            body = src.read()
        with open(lic_path, "w", encoding="utf-8") as dst:
            dst.write(provenance + body)
        print(f"licença gravada em {lic_path} (proveniência + texto integral)")
    elif not os.path.exists(lic_path):
        print("ERRO: sem --license e sem LICENSE-mono.txt — a licença da fonte "
              "precisa acompanhar o binário distribuído", file=sys.stderr)
        return 2

    print("validação:")
    critical = "".join(sorted(harvested))
    for fn in ("Mono-Base.ttf", "Mono-Bold.ttf"):
        verify_face(os.path.join(args.out_dir, fn), charset, critical)

    print("\nPróximo passo: python tools/offscreen_check.py  (ou o harness C++) para")
    print("conferir o tamanho dos glifos e o render antes de mandar para o hardware.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

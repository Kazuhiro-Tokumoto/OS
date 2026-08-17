#!/usr/bin/env python3
# ============================================================================
# make_font.py  -  GUI 用の 8x8 ビットマップフォントヘッダを生成する
#
# Linux カーネルソースの lib/fonts/font_8x8.c (cpi2fnt が生成した
# 標準的な VGA 8x8 フォント) から、ASCII 範囲 (0x20-0x7E) だけを抜き出して
# src/gui/font8x8.h を作る。
#
# 手で 96 文字ぶんのビットパターンを打ち込むのは現実的でないので、
# 生成物をリポジトリにコミットしておき、ビルド時にはこのスクリプトを
# 走らせなくてよいようにしている。
#
# ライセンス: 元データは Linux カーネル (GPL-2.0) の一部。
#            そのため生成される font8x8.h も GPL-2.0 になる。
#
# 使い方:
#   python3 tools/make_font.py ~/kernelbuild/linux-6.12.9/lib/fonts/font_8x8.c
# ============================================================================
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "gui" / "font8x8.h"

FIRST = 0x20
LAST = 0x7E


def main() -> int:
    if len(sys.argv) < 2:
        print(f"使い方: {sys.argv[0]} <linux>/lib/fonts/font_8x8.c", file=sys.stderr)
        return 1

    src = Path(sys.argv[1])
    text = src.read_text()

    # "0x7e," の形の並びを全部拾う。先頭の struct font_data の
    # { 0, 0, FONTDATAMAX, 0 } は 10進なのでマッチしない。
    values = [int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{2}),", text)]
    if len(values) < 2048:
        print(f"ERROR: 抽出できたバイト数が足りません ({len(values)})", file=sys.stderr)
        return 1
    values = values[:2048]

    lines = []
    lines.append("/* SPDX-License-Identifier: GPL-2.0 */")
    lines.append("/* ==========================================================================")
    lines.append(" * font8x8.h  -  GUI 用 8x8 ビットマップフォント (ASCII 0x20-0x7E)")
    lines.append(" *")
    lines.append(" * tools/make_font.py が Linux カーネルの lib/fonts/font_8x8.c から生成。")
    lines.append(" * 手で書き起こしたものではないので、直接編集しないこと。")
    lines.append(" *")
    lines.append(" * 1 文字 8 バイト。1 バイトが 1 行で、bit7 が左端。")
    lines.append(" * ========================================================================== */")
    lines.append("#ifndef MYOS_FONT8X8_H")
    lines.append("#define MYOS_FONT8X8_H")
    lines.append("")
    lines.append(f"#define FONT_FIRST 0x{FIRST:02X}")
    lines.append(f"#define FONT_LAST  0x{LAST:02X}")
    lines.append("#define FONT_W 8")
    lines.append("#define FONT_H 8")
    lines.append("")
    lines.append("static const unsigned char font8x8[FONT_LAST - FONT_FIRST + 1][8] = {")

    for ch in range(FIRST, LAST + 1):
        rows = values[ch * 8:(ch + 1) * 8]
        body = ", ".join(f"0x{b:02x}" for b in rows)
        printable = chr(ch) if ch != 0x5C else "\\\\"
        lines.append(f"    {{ {body} }},  /* 0x{ch:02X} '{printable}' */")

    lines.append("};")
    lines.append("")
    lines.append("#endif /* MYOS_FONT8X8_H */")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n")
    print(f"[FONT] wrote {OUT} ({LAST - FIRST + 1} glyphs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

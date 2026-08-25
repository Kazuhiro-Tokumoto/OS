#!/bin/sh
# ============================================================================
# make_kheaders.sh  -  自作カーネルのヘッダを 1 個の tar に固める
#
#   sh tools/make_kheaders.sh <カーネルの木> <出力先.tar.zst>
#   例: sh tools/make_kheaders.sh /mnt/myos/kernelbuild/linux-6.12.9 \
#                                 build/myos-kheaders-1.6.0.tar.zst
#
# 何のために要るか
# ----------------
# NVIDIA の公式ドライバは「動いているカーネルに対して自分をコンパイル
# する」作りになっている。そのために
#
#   /lib/modules/<uname -r>/build
#
# の下に、Makefile・Module.symvers・.config・include・scripts が
# 揃っている必要がある。ディストリビューションの linux-headers-* が
# 配っているのがこれ。
#
# myOS はカーネルを自分で作っているので、Debian の linux-headers は
# **一切合わない**。バージョンが同じでも .config が違えば Module.symvers
# のチェックサムが合わず、作ったモジュールは読み込めない。だから
# 自分で作って、リリースに一緒に置く。
#
# 配るのはカーネルのソースの一部 (GPL) だけ。NVIDIA のものは 1 バイトも
# 含まない。ドライバ本体は使う人が NVIDIA から落としてくる
# (Windows 95 の「ディスク使用...」と同じ考え方)。
#
# 何を入れるか
# ------------
# 木は 3GB ある。ほとんどが .o なので、モジュールを作るのに要るものだけ
# 拾う。Arch や Debian の linux-headers が入れているものと同じ並び。
#
#   Makefile / Kconfig / Kbuild / *.sh / *.pl / *.lds
#       ビルドの仕掛けそのもの
#   include/ と arch/x86 の include/
#       ヘッダ。生成済みの include/generated と include/config も要る
#       (.config から作られるもので、これが無いと何も通らない)
#   scripts/
#       **コンパイル済みのホスト用バイナリごと**入れる。fixdep や
#       modpost が無いと make が最初の 1 行で止まる
#   tools/objtool/objtool
#       CONFIG_OBJTOOL=y なので要る。無いとリンクの最後で転ぶ
#   .config / Module.symvers
#       設定と、カーネルが公開している記号の一覧。**Module.symvers が
#       食い違うと、出来たモジュールは insmod で弾かれる**
# ============================================================================
set -e

usage() { echo "usage: make_kheaders.sh <kernel-tree> <out.tar.zst>" >&2; exit 2; }
[ $# -eq 2 ] || usage

TREE="$1"
OUT="$2"

[ -f "$TREE/Makefile" ] || { echo "$TREE がカーネルの木に見えません" >&2; exit 1; }
[ -f "$TREE/.config" ] || { echo "$TREE/.config がありません (未ビルド?)" >&2; exit 1; }
[ -f "$TREE/Module.symvers" ] || \
    { echo "$TREE/Module.symvers がありません (まだビルドが終わっていない)" >&2; exit 1; }

command -v zstd >/dev/null 2>&1 || { echo "zstd がありません" >&2; exit 1; }

LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT

cd "$TREE"

# --- ビルドの仕掛け -------------------------------------------------------
# arch は x86 だけでよい。他のアーキテクチャの Kconfig まで入れると
# 無駄に膨らむ。
find . -path ./arch -prune -o \
     \( -name 'Makefile*' -o -name 'Kconfig*' -o -name 'Kbuild*' \
        -o -name '*.sh' -o -name '*.pl' -o -name '*.lds' \) -print \
     | sed 's|^\./||' >> "$LIST"
find arch/x86 \( -name 'Makefile*' -o -name 'Kconfig*' -o -name 'Kbuild*' \
     -o -name '*.sh' -o -name '*.lds' -o -name 'Platform' \) -print >> "$LIST"

# --- ヘッダ ---------------------------------------------------------------
find include -type f >> "$LIST"
find arch/x86 -type d -name include -exec find {} -type f \; >> "$LIST"

# --- 生成済みの道具 -------------------------------------------------------
# ここはバイナリごと入れる。ソースだけ入れても、入れた先に gcc が
# 無い時点で詰む……のではなく、実際には make が scripts を作り直そうと
# して時間を食う。出来上がりを入れておけばそのまま使える。
find scripts -type f >> "$LIST"
[ -f tools/objtool/objtool ] && echo tools/objtool/objtool >> "$LIST"
[ -f tools/bpf/resolve_btfids/resolve_btfids ] && \
    echo tools/bpf/resolve_btfids/resolve_btfids >> "$LIST"

# --- 設定と記号 -----------------------------------------------------------
echo .config        >> "$LIST"
echo Module.symvers >> "$LIST"
[ -f System.map ] && echo System.map >> "$LIST"

sort -u "$LIST" -o "$LIST"
n="$(wc -l < "$LIST")"

mkdir -p "$(dirname "$OUT")"
tar -c -f - -T "$LIST" | zstd -q -19 -T0 -f -o "$OUT"

sz="$(wc -c < "$OUT")"
echo "make_kheaders: ok  $OUT  $n 個  $((sz / 1024 / 1024)) MB"

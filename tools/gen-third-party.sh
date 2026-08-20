#!/bin/sh
# ============================================================================
# gen-third-party.sh  -  同梱しているものの一覧を作る
#
#   sh tools/gen-third-party.sh <ルートのパス> <出力先>
#
# myOS の配布物には他人の書いたものが 500 個以上入っている。
# ライセンス文そのものは各パッケージの
#   /usr/share/doc/<パッケージ>/copyright
# にそのまま入れてあるが、それだけでは「何が入っているのか」が
# 見渡せない。索引を作る。
#
# 手で書かない。手で書くと必ず古くなるし、569 行を人が正しく写せる
# わけがない。入っているものから毎回作る。
#
# ライセンス名の取り方:
#   DEP-5 (Format: が 1 行目にある機械可読なもの) は License: を読む。
#   そうでないものは、よく使われる名前を本文から探す。
#   どちらでも決められなければ (see copyright) と書く。
#   分からないものを分かったように書くほうが害が大きい。
# ============================================================================
set -e
ROOT="${1:-/}"
OUT="${2:-/dev/stdout}"
DOC="$ROOT/usr/share/doc"

lic_of() {
    f="$1"
    if head -1 "$f" 2>/dev/null | grep -q '^Format:'; then
        # DEP-5。License: の値を集めて、多いものから並べる。
        awk '/^License:[ \t]*[^ \t]/ {
                 sub(/^License:[ \t]*/, ""); sub(/[ \t]*$/, "");
                 if ($0 != "") print $0
             }' "$f" | sort | uniq -c | sort -rn | head -3 |
            awk '{ $1=""; sub(/^ /,""); printf "%s%s", sep, $0; sep=", " }'
        return
    fi
    # 自由形式。名前を探す。並びは「具体的なものから先に」。
    for pat in "GNU General Public License.*[Vv]ersion 3" \
               "GNU General Public License.*[Vv]ersion 2" \
               "GNU Lesser General Public License" \
               "Apache License" "Mozilla Public License" \
               "BSD" "MIT" "ISC" "public domain"; do
        if grep -qi "$pat" "$f" 2>/dev/null; then
            case "$pat" in
                *"Version 3"|*"version 3") echo "GPL-3"; return;;
                *"Version 2"|*"version 2") echo "GPL-2"; return;;
                *Lesser*)   echo "LGPL"; return;;
                *Apache*)   echo "Apache-2.0"; return;;
                *Mozilla*)  echo "MPL"; return;;
                *BSD*)      echo "BSD"; return;;
                *MIT*)      echo "MIT"; return;;
                *ISC*)      echo "ISC"; return;;
                *)          echo "public domain"; return;;
            esac
        fi
    done
    echo "(see copyright)"
}

{
    echo "# 同梱しているもの"
    echo
    echo "myOS の配布物には、他の人が書いたものが多数入っている。"
    echo "**これらは myOS 本体の MIT ではなく、それぞれのライセンスに従う。**"
    echo
    echo "ライセンス文そのものは、入れたあとの"
    echo '`/usr/share/doc/<パッケージ名>/copyright` にそのまま入っている。'
    echo "下の表はその索引で、ビルドのたびに実物から作り直している。"
    echo
    echo "GPL のもののソースは、無改変で使っているので上流がそのまま"
    echo "出どころになる。カーネルは kernel.org、Debian のパッケージは"
    echo '`apt-get source <パッケージ名>` で取れる。'
    echo "詳しくは \`LICENSE.ja.md\`。"
    echo

    n=0
    if [ -d "$DOC" ]; then
        n=$(find "$DOC" -mindepth 2 -maxdepth 2 -name copyright 2>/dev/null | wc -l)
    fi
    echo "収録数: $n"
    echo
    echo "| パッケージ | ライセンス |"
    echo "| --- | --- |"

    find "$DOC" -mindepth 2 -maxdepth 2 -name copyright 2>/dev/null |
        sort | while read -r f; do
            pkg=$(basename "$(dirname "$f")")
            [ "$pkg" = "myos" ] && continue
            l=$(lic_of "$f")
            [ -n "$l" ] || l="(see copyright)"
            printf '| %s | %s |\n' "$pkg" "$l"
        done
} > "$OUT"

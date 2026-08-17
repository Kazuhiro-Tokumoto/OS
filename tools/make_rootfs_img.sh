#!/bin/sh
# ============================================================================
# make_rootfs_img.sh  -  ルートファイルシステムを ext4 イメージに固める
#
# mkfs.ext4 の -d オプションを使うので、マウントは要らない
# (コンテナや CI の中でも作れる)。
#
# 使い方:
#   sh tools/make_rootfs_img.sh [rootfs ディレクトリ] [出力先] [サイズ]
#   sh tools/make_rootfs_img.sh /home/user/rootfs build/rootfs.ext4 2G
# ============================================================================
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-/home/user/rootfs}"
OUT="${2:-$ROOT/build/rootfs.ext4}"
SIZE="${3:-2G}"

if [ ! -d "$SRC" ]; then
    echo "ERROR: $SRC がありません。先に tools/build_rootfs.sh を実行してください。" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"

echo "=== ext4 イメージを作成 ($SIZE) ==="
# -m 0     : root 用の予約領域を作らない (使える容量を無駄にしない)
# -d       : ディレクトリの中身をそのまま流し込む
# -F       : 通常ファイルへの mkfs を許可
mkfs.ext4 -F -q -m 0 -L myos -d "$SRC" "$OUT" "$SIZE"

echo "=== 確認 ==="
ls -l "$OUT"

#!/bin/sh
# ============================================================================
# make_initramfs.sh  -  最小の initramfs (cpio) を作る
#
# 中身は 2 つだけ:
#   /init          … src/init/init.c をビルドした静的バイナリ (libc 非依存)
#   /dev/console   … カーネルが PID 1 の fd 0/1/2 として開くデバイスノード
#
# newc 形式で固めて gzip する。Linux が initramfs として読めるのはこの形式。
# mknod を使うので root 権限が要る。
#
# 使い方:
#   sh tools/make_initramfs.sh [出力先]
#   → 既定では build/initramfs.cpio.gz
# ============================================================================
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/initramfs.cpio.gz}"
WORK="$ROOT/build/initramfs-root"

mkdir -p "$ROOT/build"
rm -rf "$WORK"
mkdir -p "$WORK/dev"

echo "=== /init をビルド ==="
gcc -static -nostdlib -nostartfiles -ffreestanding -Os \
    -fno-stack-protector -fno-asynchronous-unwind-tables \
    -o "$WORK/init" "$ROOT/src/init/init.c"
chmod 755 "$WORK/init"
strip "$WORK/init" 2>/dev/null || true
ls -l "$WORK/init"

echo "=== /dev/console を作成 ==="
mknod "$WORK/dev/console" c 5 1
mknod "$WORK/dev/null" c 1 3

echo "=== cpio (newc) に固める ==="
( cd "$WORK" && find . -print0 | cpio --null -o -H newc --quiet ) | gzip -9 > "$OUT"

echo
echo "=== 完成 ==="
ls -l "$OUT"

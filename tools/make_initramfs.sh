#!/bin/sh
# ============================================================================
# make_initramfs.sh  -  最小の initramfs (cpio) を作る
#
# 中身:
#   /init          … src/gui/win98.c   … Win98 もどきの GUI (既定の PID 1)
#   /init-text     … src/init/init.c   … 文字だけの動作確認用 init
#                     (rdinit=/init-text で切り替えられる)
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

CFLAGS="-static -nostdlib -nostartfiles -ffreestanding -Os \
        -fno-stack-protector -fno-asynchronous-unwind-tables"

echo "=== /init (Win98 もどき GUI) をビルド ==="
gcc $CFLAGS -I "$ROOT/src/gui" -o "$WORK/init" "$ROOT/src/gui/win98.c"

echo "=== /init-text (文字だけの動作確認用) をビルド ==="
gcc $CFLAGS -o "$WORK/init-text" "$ROOT/src/init/init.c"

chmod 755 "$WORK/init" "$WORK/init-text"
strip "$WORK/init" "$WORK/init-text" 2>/dev/null || true
ls -l "$WORK/init" "$WORK/init-text"

echo "=== デバイスノードを作成 ==="
# devtmpfs を待たずに使えるよう、必要なものは自分で作っておく。
# メジャー/マイナー番号は Documentation/admin-guide/devices.txt のとおり。
mknod "$WORK/dev/console" c 5 1     # PID 1 の fd 0/1/2
mknod "$WORK/dev/null"    c 1 3
mknod "$WORK/dev/tty0"    c 4 0
mknod "$WORK/dev/fb0"     c 29 0    # フレームバッファ。GUI はここへ描く
mkdir -p "$WORK/dev/input"
mknod "$WORK/dev/input/mice"   c 13 63   # PS/2 互換のマウス
for n in 0 1 2 3; do
    mknod "$WORK/dev/input/event$n" c 13 $((64 + n))
done
ls -l "$WORK/dev" "$WORK/dev/input"

echo "=== cpio (newc) に固める ==="
( cd "$WORK" && find . -print0 | cpio --null -o -H newc --quiet ) | gzip -9 > "$OUT"

echo
echo "=== 完成 ==="
ls -l "$OUT"

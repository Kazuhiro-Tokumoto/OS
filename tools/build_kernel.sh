#!/bin/sh
# ============================================================================
# build_kernel.sh  -  myOS が起動する Linux カーネルをビルドする
#
# 方針 (引き継ぎ資料 1章):
#   カーネルは自作せず Linux カーネルを採用する。
#   最終的には .ko (ローダブルモジュール) を全て =y にして
#   モノリシックな bzImage 1 個で完結させる。
#
# 使い方:
#   sh tools/build_kernel.sh [作業ディレクトリ] [バージョン]
#   sh tools/build_kernel.sh ~/kernelbuild 6.12.9
#
# 環境変数:
#   MONOLITHIC=1   … .config の =m を全て =y に変換してからビルドする
#                    (ビルド時間とサイズが大幅に増える。最終形はこちら)
# ============================================================================
set -e

WORKDIR="${1:-$HOME/kernelbuild}"
VERSION="${2:-6.12.9}"
SRC="$WORKDIR/linux-$VERSION"
JOBS="$(nproc)"

echo "=== ビルド依存パッケージ ==="
if command -v apt-get >/dev/null 2>&1; then
    apt-get install -y --no-install-recommends \
        build-essential bc bison flex libelf-dev libssl-dev cpio >/dev/null
fi

echo "=== ソースの取得 ==="
mkdir -p "$WORKDIR"
if [ ! -d "$SRC" ]; then
    MAJOR="$(echo "$VERSION" | cut -d. -f1)"
    cd "$WORKDIR"
    [ -f "linux-$VERSION.tar.xz" ] || \
        curl -sS -O "https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-$VERSION.tar.xz"
    tar xf "linux-$VERSION.tar.xz"
fi

cd "$SRC"

echo "=== 設定 ==="
make defconfig

# 自作ブートローダーからの起動を確認しやすくするための設定。
# シリアルコンソールに出せると QEMU の -serial でログを丸ごと取れる。
./scripts/config --enable SERIAL_8250
./scripts/config --enable SERIAL_8250_CONSOLE
./scripts/config --enable EARLY_PRINTK
./scripts/config --enable VT
./scripts/config --enable VT_CONSOLE
./scripts/config --enable VGA_CONSOLE
./scripts/config --enable BLK_DEV_INITRD
./scripts/config --enable RD_GZIP
./scripts/config --enable BINFMT_ELF

# --- フェーズ5 (GUI) 用: フレームバッファと入力 ---------------------------
# 自作ブートローダーが VBE でリニアフレームバッファを設定し、
# screen_info.orig_video_isVGA = VIDEO_TYPE_VLFB (0x23) を立てて渡す。
# それを受けて vesafb が /dev/fb0 を作る。GUI はそこへ直接描く。
./scripts/config --enable FB
./scripts/config --enable FB_DEVICE          # /dev/fb0 を作るのに必要
./scripts/config --enable FB_VESA
./scripts/config --enable FB_VGA16
# SYSFB_SIMPLEFB は screen_info を見て "simple-framebuffer" プラットフォーム
# デバイスを登録するだけ。それに結び付くドライバ (FB_SIMPLE) を入れておかないと
# デバイスは出来るのに誰もバインドせず、/dev/fb0 が生えない。
# 症状は「Console: colour dummy device 80x25」と出て画面が死ぬこと。
./scripts/config --enable SYSFB_SIMPLEFB
./scripts/config --enable FB_SIMPLE
./scripts/config --enable FRAMEBUFFER_CONSOLE
./scripts/config --enable FRAMEBUFFER_CONSOLE_DETECT_PRIMARY
# マウスとキーボードを読むため
./scripts/config --enable INPUT_MOUSEDEV     # /dev/input/mice (PS/2 形式)
./scripts/config --enable INPUT_EVDEV        # /dev/input/eventN
./scripts/config --enable SERIO_I8042
./scripts/config --enable MOUSE_PS2
./scripts/config --enable KEYBOARD_ATKBD
# ブートローダー側でまだ扱えないものを外しておく
./scripts/config --disable RANDOMIZE_BASE       # KASLR。切り分けを簡単にする

if [ "${MONOLITHIC:-0}" = "1" ]; then
    echo "=== モジュール (=m) を全て =y に変換 ==="
    sed -i 's/^\(CONFIG_[A-Z0-9_]*\)=m$/\1=y/' .config
fi

make olddefconfig

echo "=== ビルド (make -j$JOBS bzImage) ==="
make -j"$JOBS" bzImage

echo
echo "=== 完成 ==="
ls -l "$SRC/arch/x86/boot/bzImage"
echo "ヘッダを確認するには:"
echo "  python3 tools/bzimage_info.py $SRC/arch/x86/boot/bzImage"

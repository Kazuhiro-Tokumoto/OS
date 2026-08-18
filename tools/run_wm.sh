#!/bin/sh
# ============================================================================
# run_wm.sh  -  ウィンドウマネージャを Xvfb 上で動かして画面を撮る
#
# ディスクイメージを作り直さずに GUI を試すための足場。
# 2GB のイメージを毎回作り直していては手戻りが重すぎるので、
# 見た目の調整はここで回し、通ったものだけイメージに入れる。
#
# 使い方:
#   sh tools/run_wm.sh [出力PNG] [表示するアプリ...]
#   sh tools/run_wm.sh build/wm.png xclock xeyes
# ============================================================================
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/wm.png}"
shift 2>/dev/null || true
APPS="${*:-xclock}"

W=1024
H=768
DISP=:99

mkdir -p "$(dirname "$OUT")"

echo "=== ビルド ==="
gcc -O2 -o "$ROOT/build/myos-wm"    "$ROOT/src/gui/myos_wm.c"    -lX11
gcc -O2 -o "$ROOT/build/myos-files" "$ROOT/src/gui/myos_files.c" -lX11
gcc -O2 -o "$ROOT/build/myos-settings" "$ROOT/src/gui/myos_settings.c" -lX11

echo "=== Xvfb ==="
Xvfb $DISP -screen 0 ${W}x${H}x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1

cleanup() {
    kill $XVFB 2>/dev/null || true
    wait $XVFB 2>/dev/null || true
}
trap cleanup EXIT

echo "=== myos-wm ==="
DISPLAY=$DISP "$ROOT/build/myos-wm" &
WM=$!
sleep 1

if ! kill -0 $WM 2>/dev/null; then
    echo "ERROR: myos-wm が起動直後に落ちました" >&2
    exit 1
fi

for a in $APPS; do
    case "$a" in
        myos-files)    a="$ROOT/build/myos-files" ;;
        myos-settings) a="$ROOT/build/myos-settings" ;;
    esac
    DISPLAY=$DISP $a >/dev/null 2>&1 &
    sleep 1
done
sleep 2

echo "=== 画面を取得 ==="
DISPLAY=$DISP xwd -root -silent | convert xwd:- png:"$OUT"
kill $WM 2>/dev/null || true
ls -l "$OUT"

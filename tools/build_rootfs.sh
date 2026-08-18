#!/bin/sh
# ============================================================================
# build_rootfs.sh  -  myOS のルートファイルシステムを作る
#
# ここまで initramfs 1 個で完結させてきたが、Firefox のような本物の GUI
# アプリを載せるには足りない。Firefox は素のフレームバッファでは動かず、
# X11 と大量の共有ライブラリを要求するため。
#
# そこで引き継ぎ資料の方針どおり
#   「動的リンカーも共有ライブラリも自作せず、既存の glibc をそのまま流用する」
# を素直に実行する = 既存のディストリのルートファイルシステムを土台にする。
#
# 構成:
#   Debian bookworm (最小構成)
#     + Xorg (fbdev ドライバ。自作ブートローダーが VBE で設定した
#             /dev/fb0 の上で動かすので DRM は要らない)
#     + Firefox ESR
#     + myOS の GUI (X のウィンドウマネージャとして動く)
#
# 使い方:
#   sh tools/build_rootfs.sh [作業ディレクトリ]
# ============================================================================
set -e

WORK="${1:-/home/user/rootfs}"
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"

# 最小限に絞る。--no-install-recommends 相当にするため個別指定する。
PACKAGES="\
xserver-xorg-core,\
xserver-xorg-video-fbdev,\
xserver-xorg-input-libinput,\
xinit,\
x11-xserver-utils,\
xfonts-base,\
firefox-esr,\
fonts-dejavu-core,\
libgtk-3-0,\
dbus-x11,\
kmod,\
udev,\
util-linux,\
mount,\
procps"

echo "=== debootstrap ($SUITE) ==="
if [ ! -d "$WORK/etc" ]; then
    debootstrap --variant=minbase \
        --include="$PACKAGES" \
        "$SUITE" "$WORK" "$MIRROR"
else
    echo "既に $WORK があるのでスキップ"
fi

echo "=== 基本設定 ==="
echo "myos" > "$WORK/etc/hostname"
cat > "$WORK/etc/fstab" <<'EOF'
/dev/sda2  /      ext4  defaults  0 1
proc       /proc  proc  defaults  0 0
sysfs      /sys   sysfs defaults  0 0
EOF

# root はパスワード無しで入れるようにしておく (開発用)
sed -i 's|^root:[^:]*:|root::|' "$WORK/etc/shadow"

echo "=== myOS の起動スクリプト ==="
# systemd は使わない。カーネルに init=/myos-init を渡して、
# 必要なものだけ自分で用意してから X を上げる。
# 起動が速く、何が動いているか把握しやすい。
cat > "$WORK/myos-init" <<'EOF'
#!/bin/sh
# myOS の PID 1。systemd の代わり。
mount -t proc     proc     /proc
mount -t sysfs    sys      /sys
mount -t devtmpfs dev      /dev  2>/dev/null
mkdir -p /dev/pts /dev/shm /run /run/dbus
mount -t devpts   devpts   /dev/pts
mount -t tmpfs    tmpfs    /dev/shm
mount -t tmpfs    tmpfs    /run
mount -t tmpfs    tmpfs    /tmp
mount -o remount,rw /

hostname myos

echo "[myos-init] framebuffer:"
ls -l /dev/fb* 2>&1
echo "[myos-init] starting X on the framebuffer set up by our bootloader"
/usr/bin/xinit /myos-session -- /usr/bin/X :0 vt1 -nolisten tcp -novtswitch -logverbose 6
echo "[myos-init] X exited with $?. Xorg log follows:"
cat /var/log/Xorg.0.log 2>&1

# PID 1 は絶対に終了してはいけない (終了するとカーネルパニックになる)。
# 失敗しても shell を出して調べられるようにしておく。
echo "[myos-init] dropping to a shell"
exec /bin/sh
EOF

# X のセッション: 自作のウィンドウマネージャを上げてから Firefox を出す
cat > "$WORK/myos-session" <<'EOF'
#!/bin/sh
xset -dpms s off 2>/dev/null
# 自作ウィンドウマネージャ (まだ無ければ黙って飛ばす)
if [ -x /usr/local/bin/myos-wm ]; then
    /usr/local/bin/myos-wm &
    sleep 1
fi
exec firefox-esr --no-remote about:blank
EOF

chmod 755 "$WORK/myos-init" "$WORK/myos-session"

echo "=== Xorg の設定 (fbdev) ==="
# 自作ブートローダーが VBE で設定したフレームバッファをそのまま使う。
# DRM ドライバは要らない。
mkdir -p "$WORK/etc/X11"
cat > "$WORK/etc/X11/xorg.conf" <<'EOF'
Section "ServerFlags"
    Option "AutoAddDevices" "true"
    Option "DontVTSwitch"   "true"
    Option "BlankTime"      "0"
EndSection

# modesetting は xserver-xorg-core に同梱されているドライバで、
# カーネルの DRM (QEMU なら bochs-drm) の上で動く。
# 自作ブートローダーが VBE で設定したモードは早期コンソール用に使われ、
# X が上がる段階では DRM がモード設定を引き継ぐ。
Section "Device"
    Identifier  "myos-gpu"
    Driver      "modesetting"
EndSection

Section "Monitor"
    Identifier  "myos-monitor"
EndSection

Section "Screen"
    Identifier   "myos-screen"
    Device       "myos-gpu"
    Monitor      "myos-monitor"
    DefaultDepth 24
EndSection
EOF

echo "=== myOS ウィンドウマネージャをビルド ==="
# rootfs の中でコンパイルする。
# ホスト (Ubuntu 24.04 / glibc 2.39) と rootfs (Debian bookworm / glibc 2.36)
# では glibc の版が違うので、ホストで作ったバイナリはそのままでは動かない。
# 静的リンクで逃げようとすると、libX11 がカーソル作成で libXcursor を
# dlopen した瞬間に別版の glibc を読み込んで落ちる (SIGFPE)。
# 中でコンパイルしてしまうのがいちばん素直。
ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -x "$WORK/usr/bin/gcc" ]; then
    echo "--- rootfs に gcc を入れる ---"
    cp /etc/resolv.conf "$WORK/etc/resolv.conf"
    mount --bind /proc "$WORK/proc" 2>/dev/null || true
    chroot "$WORK" sh -c \
        'apt-get update -qq && apt-get install -y --no-install-recommends \
         gcc libc6-dev libx11-dev >/dev/null && apt-get clean'
    umount "$WORK/proc" 2>/dev/null || true
fi

mkdir -p "$WORK/usr/local/src"
cp "$ROOTDIR/src/gui/myos_wm.c" "$WORK/usr/local/src/myos_wm.c"
chroot "$WORK" gcc -O2 -Wall -o /usr/local/bin/myos-wm \
    /usr/local/src/myos_wm.c -lX11
chmod 755 "$WORK/usr/local/bin/myos-wm"
ls -l "$WORK/usr/local/bin/myos-wm"

echo "=== 完成 ==="
du -sh "$WORK"

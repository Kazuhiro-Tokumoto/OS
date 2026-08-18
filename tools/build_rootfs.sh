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

# debootstrap の後から足すもの。
# Java と OpenGL (ソフトウェアレンダリング) はここで入れる。
# Minecraft のような LWJGL を使うアプリは libGL と X11 の拡張を要求する。
EXTRA="openjdk-17-jre \
       libgl1-mesa-dri libglx-mesa0 libgl1 mesa-utils \
       libxrandr2 libxxf86vm1 libxcursor1 libxi6 libxinerama1 \
       xterm"

# ドライバのファームウェア。
# カーネルにドライバを組み込んでも、ファームウェアの blob は別途要る
# (Wi-Fi、最近の GPU、一部の有線 LAN など)。
# Debian では non-free-firmware コンポーネントに入っている。
FIRMWARE="firmware-linux-free firmware-misc-nonfree firmware-realtek \
          firmware-iwlwifi firmware-atheros firmware-brcm80211 \
          firmware-amd-graphics firmware-intel-sound"

echo "=== 追加パッケージ ==="
cp /etc/resolv.conf "$WORK/etc/resolv.conf"
mount --bind /proc "$WORK/proc" 2>/dev/null || true
# non-free-firmware を有効にする (Debian 12 から独立した component)
cat > "$WORK/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main contrib non-free-firmware
deb $MIRROR $SUITE-updates main contrib non-free-firmware
EOF
chroot "$WORK" sh -c "apt-get update -qq && \
    apt-get install -y --no-install-recommends $EXTRA >/dev/null && \
    apt-get clean"

echo "=== ファームウェア ==="
# 入らないものがあっても致命的ではないので、1 つずつ試して続行する。
for fw in $FIRMWARE; do
    chroot "$WORK" sh -c \
        "apt-get install -y --no-install-recommends $fw >/dev/null 2>&1" \
        && echo "  $fw" || echo "  $fw (見つからずスキップ)"
done
chroot "$WORK" apt-get clean
umount "$WORK/proc" 2>/dev/null || true

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

# udev を上げる。これが無いと X が入力デバイスを見つけられず、
# マウスもキーボードも効かない画面になる (症状が地味なので注意)。
# systemd は使わないが、udevd 単体なら普通に動く。
if [ -x /lib/systemd/systemd-udevd ]; then
    /lib/systemd/systemd-udevd --daemon
    udevadm trigger --action=add >/dev/null 2>&1
    udevadm settle --timeout=10 >/dev/null 2>&1
fi

# USB メモリなどを自動でマウントする常駐を上げる
/usr/local/bin/myos-automount &

echo "[myos-init] input devices:"
ls /dev/input 2>&1
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

# X のセッション。
# 自作のウィンドウマネージャがセッションの主になる。
# これが終わると X も終わる。デスクトップのアイコンから
# Firefox なりファイルマネージャなりを起動する。
cat > "$WORK/myos-session" <<'EOF'
#!/bin/sh
xset -dpms s off 2>/dev/null
xsetroot -solid teal 2>/dev/null
exec /usr/local/bin/myos-wm
EOF

chmod 755 "$WORK/myos-init" "$WORK/myos-session"

echo "=== リムーバブルメディアの自動マウント ==="
# USB メモリを挿したら /media/<デバイス名> にマウントする常駐スクリプト。
# udev のルールでもできるが、ポーリングのほうが挙動が読みやすい。
cat > "$WORK/usr/local/bin/myos-automount" <<'EOF'
#!/bin/sh
# myOS: リムーバブルメディアを自動でマウント / アンマウントする。
# /sys/block/*/removable が 1 のものだけを対象にする
# (内蔵ディスクを勝手に触らないため)。
mkdir -p /media

while true; do
    for dev in /sys/block/sd*; do
        [ -e "$dev" ] || continue
        name=$(basename "$dev")
        [ "$(cat "$dev/removable" 2>/dev/null)" = "1" ] || continue

        # パーティションがあればそれを、無ければデバイス自体をマウントする
        parts=$(ls -d "$dev"/"$name"[0-9]* 2>/dev/null)
        [ -n "$parts" ] || parts="$dev"

        for p in $parts; do
            pn=$(basename "$p")
            mp="/media/$pn"
            mountpoint -q "$mp" 2>/dev/null && continue
            mkdir -p "$mp"
            if mount -o rw,noatime "/dev/$pn" "$mp" 2>/dev/null; then
                echo "[automount] mounted /dev/$pn on $mp"
            else
                rmdir "$mp" 2>/dev/null
            fi
        done
    done

    # 抜かれたメディアの後始末
    for mp in /media/*; do
        [ -d "$mp" ] || continue
        pn=$(basename "$mp")
        [ -b "/dev/$pn" ] && continue
        umount -l "$mp" 2>/dev/null
        rmdir "$mp" 2>/dev/null
        echo "[automount] removed $mp"
    done

    sleep 2
done
EOF
chmod 755 "$WORK/usr/local/bin/myos-automount"

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

# ポインタの加速を切って 1:1 で動くようにする。
# Win98 の操作感に近いのと、QEMU に mouse_move を送って自動検証するとき
# 移動量がそのまま座標になるので都合がよい。
Section "InputClass"
    Identifier   "myos-pointer"
    MatchIsPointer "on"
    Driver       "libinput"
    Option       "AccelProfile" "flat"
    Option       "AccelSpeed"   "0"
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
cp "$ROOTDIR/src/gui/myos_wm.c" "$ROOTDIR/src/gui/myos_files.c" \
   "$ROOTDIR/src/gui/myos_settings.c" \
   "$ROOTDIR/src/gui/x98.h" "$WORK/usr/local/src/"
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-wm \
    /usr/local/src/myos_wm.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-files \
    /usr/local/src/myos_files.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-settings \
    /usr/local/src/myos_settings.c -lX11
chmod 755 "$WORK/usr/local/bin/myos-wm" "$WORK/usr/local/bin/myos-files" \
    "$WORK/usr/local/bin/myos-settings"
ls -l "$WORK/usr/local/bin/"

echo "=== デスクトップのリンク ==="
# ここに 1 行足すだけでデスクトップにアイコンが増える。
# ファイルマネージャの右クリックからも追記される。
mkdir -p "$WORK/etc/myos"
cat > "$WORK/etc/myos/desktop.conf" <<'EOF'
# ラベル|アイコン|コマンド
# アイコン: computer / folder / file / app / globe / java
My Computer|computer|/usr/local/bin/myos-files /
My Documents|folder|/usr/local/bin/myos-files /root
Firefox|globe|/usr/bin/firefox-esr
Java Demo|java|java -jar /usr/local/share/myos/hello.jar
OpenGL Test|app|/usr/bin/glxgears
MS-DOS Prompt|app|/usr/bin/xterm -bg black -fg lightgray -fa Monospace -fs 11
Settings|app|/usr/local/bin/myos-settings
Removable|folder|/usr/local/bin/myos-files /media
EOF

cat > "$WORK/etc/myos/startmenu.conf" <<'EOF'
# スタートメニュー。ラベル|アイコン|コマンド
Programs|app|/usr/local/bin/myos-files /usr/bin
Documents|folder|/usr/local/bin/myos-files /root
Settings|app|/usr/local/bin/myos-settings
Removable|folder|/usr/local/bin/myos-files /media
Web|globe|/usr/bin/firefox-esr
MS-DOS Prompt|app|/usr/bin/xterm -bg black -fg lightgray
Shut Down|app|/sbin/poweroff
EOF

echo "=== Java のデモアプリを配置 ==="
# Java のバイトコードは可搬なので、ホスト側でコンパイルしたものを置くだけでよい。
# rootfs に JDK を入れずに済む (JRE だけで動く)。
mkdir -p "$WORK/usr/local/share/myos"
if [ -f "$ROOTDIR/build/hello.jar" ]; then
    cp "$ROOTDIR/build/hello.jar" "$WORK/usr/local/share/myos/hello.jar"
else
    echo "  build/hello.jar が無いのでスキップ (make java-demo で作る)"
fi

echo "=== 完成 ==="
du -sh "$WORK"

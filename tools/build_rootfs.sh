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
       xterm imagemagick \
       sudo passwd python3 \
       xserver-xorg-legacy"

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

# root は直接ログインさせない。
# 管理者の仕事は初回セットアップで作るユーザーから sudo でやる。
# (パスワード無しの root を残すと、そこがそのまま素通しになる)
chroot "$WORK" passwd -l root >/dev/null 2>&1 || \
    sed -i 's|^root:[^:]*:|root:!:|' "$WORK/etc/shadow"

# X を root 以外から起動できるようにする。
# 起動するのは PID 1 (root) だが、セッションは一般ユーザーに落とすので、
# ここを開けておかないと Xorg.wrap に弾かれる。
mkdir -p "$WORK/etc/X11"
cat > "$WORK/etc/X11/Xwrapper.config" <<'EOF'
allowed_users=anybody
needs_root_rights=yes
EOF

# sudo は「sudo グループなら自分のパスワードで昇格できる」だけにする。
# NOPASSWD にはしない。それでは管理者権限を分ける意味が無くなる。
mkdir -p "$WORK/etc/sudoers.d"
cat > "$WORK/etc/sudoers.d/myos" <<'EOF'
# 管理者 (sudo グループ) は自分のパスワードで昇格できる
%sudo ALL=(ALL:ALL) ALL

# 電源を切る / 再起動するのだけはパスワード無しで通す。
# 目の前の機械の電源を落とすのに認証を求めても意味が無く、
# 出来ることも「切る」だけなので、ここは開けておく。
%sudo ALL=(ALL) NOPASSWD: /sbin/poweroff, /sbin/reboot, /sbin/halt
EOF
chmod 440 "$WORK/etc/sudoers.d/myos"

echo "=== myOS の起動スクリプト ==="
# systemd は使わない。カーネルに init=/myos-init を渡して、
# 必要なものだけ自分で用意してから X を上げる。
# 起動が速く、何が動いているか把握しやすい。
cat > "$WORK/myos-init" <<'EOF'
#!/bin/sh
# myOS の PID 1。systemd の代わり。

# PID 1 には PATH が無い。そのままだと execvp が /bin:/usr/bin しか見ず、
# /usr/sbin にある useradd や /sbin の poweroff が「無い」ことになる。
# (セットアップがユーザーを作れずに黙って失敗する、という形で刺さる)
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

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
# X の中で最初に動くもの。まだ root。
# ここで「セットアップ -> ログオン -> 一般ユーザーへ降りる」を通す。
xset -dpms s off 2>/dev/null
xsetroot -solid teal 2>/dev/null

conf=/etc/myos/login.conf
get() { sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$conf" 2>/dev/null | head -1; }

# 初回だけセットアップ。ここでユーザーとパスワードが決まる。
if [ ! -e /etc/myos/setup-done ]; then
    /usr/local/bin/myos-setup
fi

user="$(get user)"
auto="$(get autologin)"

# セットアップを飛ばされた (Cancel) 場合は root のまま出す。
# 何も出来ないより、直せる画面が出るほうがいい。
if [ -z "$user" ]; then
    echo "[myos-session] setup not completed, running as root"
    exec /usr/local/bin/myos-desktop
fi

if [ "$auto" != "1" ]; then
    u="$(/usr/local/bin/myos-login)"
    [ -n "$u" ] && user="$u"
fi

# 一般ユーザーから X に繋げるようにする。
#   1. サーバー側で「このローカルユーザーは可」と登録する
#   2. 念のため認証クッキーもコピーしておく (1 が使えない X 用)
xhost "+si:localuser:$user" >/dev/null 2>&1
home="$(getent passwd "$user" | cut -d: -f6)"
if [ -n "$home" ] && [ -f "$XAUTHORITY" ]; then
    cp "$XAUTHORITY" "$home/.Xauthority" 2>/dev/null
    chown "$user" "$home/.Xauthority" 2>/dev/null
fi

echo "[myos-session] starting the desktop as $user"
exec su - "$user" -c \
    "DISPLAY='$DISPLAY' XAUTHORITY='$home/.Xauthority' /usr/local/bin/myos-desktop"
EOF

# 一般ユーザー側のセッション。ここから先は root ではない。
cat > "$WORK/usr/local/bin/myos-desktop" <<'EOF'
#!/bin/sh
# デスクトップ本体。一般ユーザーで動く。
exec /usr/local/bin/myos-wm
EOF
chmod 755 "$WORK/usr/local/bin/myos-desktop"

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

# デスクトップを動かしているユーザー。FAT の所有者に使う。
deskuser="$(sed -n 's/^user[[:space:]]*=[[:space:]]*//p' /etc/myos/login.conf 2>/dev/null | head -1)"
[ -n "$deskuser" ] || deskuser=root

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
            # FAT / exFAT / NTFS には所有者という考えが無いので、
            # マウントするときに「誰のものにするか」を指定しないと
            # root のものになり、デスクトップのユーザーが書けなくなる。
            uid="$(id -u "$deskuser" 2>/dev/null || echo 0)"
            gid="$(id -g "$deskuser" 2>/dev/null || echo 0)"
            if mount -o rw,noatime,uid=$uid,gid=$gid,umask=0022 \
                     "/dev/$pn" "$mp" 2>/dev/null; then
                echo "[automount] mounted /dev/$pn on $mp (owner $deskuser)"
            elif mount -o rw,noatime "/dev/$pn" "$mp" 2>/dev/null; then
                # uid= を受け付けない ext4 などはこちら
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
   "$ROOTDIR/src/gui/myos_settings.c" "$ROOTDIR/src/gui/myos_notepad.c" \
   "$ROOTDIR/src/gui/myos_image.c" "$ROOTDIR/src/gui/myos_open.c" \
   "$ROOTDIR/src/gui/myos_runas.c" "$ROOTDIR/src/gui/myos_setup.c" \
   "$ROOTDIR/src/gui/myos_login.c" \
   "$ROOTDIR/src/gui/x98.h" "$ROOTDIR/src/gui/myosconf.h" \
   "$ROOTDIR/src/gui/filetypes.h" "$WORK/usr/local/src/"
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-wm \
    /usr/local/src/myos_wm.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-files \
    /usr/local/src/myos_files.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-settings \
    /usr/local/src/myos_settings.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-notepad \
    /usr/local/src/myos_notepad.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-image \
    /usr/local/src/myos_image.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-runas \
    /usr/local/src/myos_runas.c -lX11
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-setup \
    /usr/local/src/myos_setup.c -lX11
# ログオン画面は /etc/shadow を crypt() で照合するので libcrypt が要る
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-login \
    /usr/local/src/myos_login.c -lX11 -lcrypt
# myos-open は X に触らない。端末やスクリプトからも使う。
chroot "$WORK" gcc -O2 -o /usr/local/bin/myos-open \
    /usr/local/src/myos_open.c
chmod 755 "$WORK"/usr/local/bin/myos-*

echo "=== ファイルの関連付け ==="
# Win98 の「フォルダオプション -> ファイルの種類」にあたる表。
# どの拡張子を何で開くかは、コードではなくここで決まる。
# myos-open がこれを読んで振り分ける。
mkdir -p "$WORK/etc/myos"
cat > "$WORK/etc/myos/filetypes.conf" <<'EOF'
# myOS: ファイルの関連付け
#
#   拡張子(カンマ区切り)|説明|アイコン|開く|実行(任意)
#
# %1 がファイルのパスに置き換わる。シェルは通さないので、
# 空白の入ったファイル名でも壊れない。
# 「開く」がダブルクリックの動作。「実行」は右クリックに出る 2 つ目の動詞。
# アイコン: computer / folder / file / app / globe / java
#
# ここを直せば挙動が変わる。~/.myos/filetypes.conf に置くと自分の分だけ変わる。

# --- 文書 -------------------------------------------------------------------
txt,log,md,ini,cfg,conf,inc,csv|Text Document|file|myos-notepad %1|
html,htm,xhtml|HTML Document|globe|firefox-esr %1|myos-notepad %1
pdf|PDF Document|globe|firefox-esr %1|

# --- 画像 -------------------------------------------------------------------
png,jpg,jpeg,gif,bmp,ppm,pgm,tif,tiff,webp,ico,xpm|Image|file|myos-image %1|

# --- プログラム -------------------------------------------------------------
# 「開く」はメモ帳で中身を見る、「実行」で走らせる。Windows と同じ考え方。
py|Python Script|app|myos-notepad %1|myos-term python3 %1
sh|Shell Script|app|myos-notepad %1|myos-term sh %1
c|C Source|file|myos-notepad %1|myos-term myos-cc %1
h|C Header|file|myos-notepad %1|
asm,s|Assembler Source|file|myos-notepad %1|
java|Java Source|java|myos-notepad %1|
json,xml,css,js|Source File|file|myos-notepad %1|

# --- Java -------------------------------------------------------------------
# JRE だけ入れてある。.java のコンパイルは出来ない (javac は同梱していない)。
jar|Java Archive|java|myos-java -jar %1|myos-term myos-java -jar %1
class|Java Class|java|myos-java %1|

# --- 書庫 -------------------------------------------------------------------
zip,gz,xz,bz2,tar,tgz|Archive|file|myos-term tar -tvf %1|
EOF

echo "=== 補助コマンド ==="
# 端末の中で走らせて、終わっても窓を閉じない。
# py や sh を実行したときに出力が読めるようにするため。
cat > "$WORK/usr/local/bin/myos-term" <<'EOF'
#!/bin/sh
# myos-term <コマンド> [引数...]
# Win98 の DOS 窓の見た目で、コマンドを実行して結果を残す。
exec xterm -title "myOS - $1" \
    -fg lightgray -bg black -cr white -fn 9x15 -geometry 80x25 \
    -e /usr/local/bin/myos-term-run "$@"
EOF

cat > "$WORK/usr/local/bin/myos-term-run" <<'EOF'
#!/bin/sh
# xterm の中で実行される側。閉じる前に止まる。
# 引数をそのまま渡すので、空白入りのパスでも壊れない。
"$@"
st=$?
echo
echo "----- finished (exit status $st) -----"
printf 'Press Enter to close this window.'
read dummy
EOF

# C のソースをその場でコンパイルして走らせる。
cat > "$WORK/usr/local/bin/myos-cc" <<'EOF'
#!/bin/sh
# myos-cc <ソース.c> [引数...]
src="$1"
[ -n "$src" ] || { echo "usage: myos-cc <file.c> [args...]"; exit 2; }
shift
out="/tmp/$(basename "$src" .c).out"
echo "cc -O2 -o $out $src"
cc -O2 -o "$out" "$src" || exit $?
echo "----- running -----"
exec "$out" "$@"
EOF

# Java の起動。ヒープの上限をメモリ量から決める。
cat > "$WORK/usr/local/bin/myos-java" <<'EOF'
#!/bin/sh
# myos-java [java の引数...]
# ヒープの上限を積んでいるメモリから決める。
# 既定のままだと、メモリの少ない実機で Java だけが全部持っていってしまう。
# /etc/myos/java.conf に heap_mb を書けばそれを優先する。
conf=/etc/myos/java.conf
[ -f "$HOME/.myos/java.conf" ] && conf="$HOME/.myos/java.conf"
heap="$(sed -n 's/^heap_mb[[:space:]]*=[[:space:]]*//p' "$conf" 2>/dev/null | head -1)"

if [ -z "$heap" ] || [ "$heap" = "auto" ]; then
    total_kb="$(sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo)"
    total_mb=$((total_kb / 1024))
    heap=$((total_mb / 4))                 # 全体の 1/4
    [ "$heap" -lt 192 ] && heap=192        # 下限。これ未満だと Swing が苦しい
    [ "$heap" -gt 1024 ] && heap=1024      # 上限。デスクトップ用途ならこれで十分
fi

exec java -Xmx${heap}m -Dsun.java2d.opengl=false "$@"
EOF

chmod 755 "$WORK/usr/local/bin/myos-term" "$WORK/usr/local/bin/myos-term-run" \
          "$WORK/usr/local/bin/myos-cc" "$WORK/usr/local/bin/myos-java"

cat > "$WORK/etc/myos/java.conf" <<'EOF'
# myOS: Java の走らせ方
#
# heap_mb = auto   … メモリ量の 1/4 (192MB 〜 1024MB の範囲で自動)
# heap_mb = 512    … 固定したいときは数字を書く
heap_mb = auto
EOF

echo "=== MS-DOS プロンプト ==="
# 見た目は Win98 の MS-DOS プロンプト、中身は普通の Linux のシェル。
# DOS のコマンド名でも通るよう別名を用意しておく。
mkdir -p "$WORK/etc/myos"
cat > "$WORK/etc/myos/dosrc" <<'EOF'
# myOS: MS-DOS プロンプト風の設定 (bash から読まれる)

# パスを DOS 風に見せる。中身は普通の Linux のパス。
# パス区切りを DOS 風に見せる。bash のパラメータ展開でも書けるが、
# ヒアドキュメントを何段も通すとエスケープが壊れやすいので tr に任せる。
dos_pwd() {
    printf 'C:'
    printf '%s' "$PWD" | tr '/' '\\'
}
PS1='$(dos_pwd)> '

# DOS のコマンド名で叩けるようにする。実体は Linux のコマンド。
alias dir='ls -la'
alias cls='clear'
alias copy='cp -i'
alias move='mv -i'
alias del='rm -i'
alias ren='mv'
alias md='mkdir'
alias rd='rmdir'
alias type='cat'
alias ver='uname -a'
alias mem='free -h'
alias edit='myos-notepad'
alias exit='exit'

# バナーは 9x15 のビットマップフォントで出るので ASCII だけで書く。
# (日本語を入れると xterm 側にグリフが無く文字化けする)
cat <<'BANNER'

Microsoft(R) Windows 98
   (C)Copyright Microsoft Corp 1981-1999.

  ...is what it looks like.  Inside, this is Linux.
  DOS-style commands work too: dir / cls / copy / move / del / type / ver

BANNER
EOF

cat > "$WORK/usr/local/bin/myos-prompt" <<'EOF'
#!/bin/sh
# MS-DOS プロンプト。xterm を Win98 風の配色と等幅フォントで出す。
# xterm は起動時に $SHELL を絶対パスで解決しようとする。
# PID 1 から来る環境では $SHELL が入っていないことがあり、
# "No absolute path found for shell" という警告が 1 行出る
# (警告だけで動作には影響しない)。明示しておけば黙る。
SHELL=/bin/bash
export SHELL
exec xterm \
    -title "MS-DOS Prompt" \
    -fg lightgray -bg black -cr white \
    -fn 9x15 \
    -geometry 80x25 \
    -e /bin/bash --rcfile /etc/myos/dosrc -i
EOF
chmod 755 "$WORK/usr/local/bin/myos-prompt"

ls -l "$WORK/usr/local/bin/"

echo "=== デスクトップのリンク ==="
# ここに 1 行足すだけでデスクトップにアイコンが増える。
# ファイルマネージャの右クリックからも追記される。
mkdir -p "$WORK/etc/myos"
cat > "$WORK/etc/myos/desktop.conf" <<'EOF'
# ラベル|アイコン|コマンド
# アイコン: computer / folder / file / app / globe / java
My Computer|computer|/usr/local/bin/myos-files /
My Documents|folder|/usr/local/bin/myos-files ~
Firefox|globe|/usr/bin/firefox-esr
Java Demo|java|/usr/local/bin/myos-java -jar /usr/local/share/myos/hello.jar
OpenGL Test|app|/usr/bin/glxgears
MS-DOS Prompt|app|/usr/local/bin/myos-prompt
Notepad|file|/usr/local/bin/myos-notepad
Settings|app|/usr/local/bin/myos-settings
Removable|folder|/usr/local/bin/myos-files /media
EOF

cat > "$WORK/etc/myos/startmenu.conf" <<'EOF'
# スタートメニュー。ラベル|アイコン|コマンド
Programs|app|/usr/local/bin/myos-files /usr/bin
Documents|folder|/usr/local/bin/myos-files ~
Settings|app|/usr/local/bin/myos-settings
Removable|folder|/usr/local/bin/myos-files /media
Web|globe|/usr/bin/firefox-esr
Notepad|file|/usr/local/bin/myos-notepad
MS-DOS Prompt|app|/usr/local/bin/myos-prompt
Shut Down|app|sudo /sbin/poweroff
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

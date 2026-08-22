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

# このスクリプトの置き場所から見たリポジトリの根。
# 途中で参照するものがあるので、早い段階で決めておく。
ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"
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
# Java は JRE。JDK は javac のぶん 110MB 増えるので、
# インストールディスクを CD-R (700MB) に収めるために外している。
# .jar と .class は動く。.java のコンパイルは出来ない。
EXTRA="openjdk-17-jre \
       libgl1-mesa-dri libglx-mesa0 libgl1 mesa-utils \
       libxrandr2 libxxf86vm1 libxcursor1 libxi6 libxinerama1 \
       xterm imagemagick \
       sudo passwd python3 \
       xserver-xorg-legacy"

# --- 一般の Linux アプリを入れて動かすためのもの -----------------------------
# 配布物 (.deb / AppImage / tar.gz) を落としてきて入れる、という
# 普通の使い方ができるようにする。GUI アプリが要求する共有ライブラリと、
# メニューに出すための .desktop まわりを先に入れておく。
# コードエディタ (geany) は入れない。容量のため。
# メモ帳で開けるので、書けなくなるわけではない。
APPS="gtk2-engines-pixbuf libgtk-3-0 libgtk2.0-0 libcanberra-gtk3-module \
      libnss3 libnspr4 libasound2 libxss1 libgbm1 libdrm2 libcups2 \
      libatk1.0-0 libatk-bridge2.0-0 libpango-1.0-0 libpangocairo-1.0-0 \
      libcairo2 libgdk-pixbuf-2.0-0 libxcomposite1 libxdamage1 libxfixes3 \
      libxkbcommon0 libsecret-1-0 libnotify4 libvulkan1 \
      libfuse2 fuse3 desktop-file-utils shared-mime-info xdg-utils \
      unzip zip xz-utils p7zip-full \
      dbus-x11 at-spi2-core \
      fonts-dejavu fonts-liberation \
      ca-certificates wget curl"

# --- セキュリティ ------------------------------------------------------------
# ufw はファイアウォール、ClamAV はウイルス対策。
# cron は定期スキャン、inotify-tools はダウンロード監視に使う。
# インストーラが使う道具。
# これが無いとパーティションを切れず、コピーもできない。
# (入っていなくてもインストーラは起動するので、
#  「確認画面まで進んで急に中止される」という形で刺さる)
INSTALLER="fdisk parted squashfs-tools dosfstools e2fsprogs zstd cpio"

# ネットワーク。
# systemd も NetworkManager も使っていないので、
# リンクを上げる道具 (ip) と DHCP クライアントは自前で持つ必要がある。
# これが無いと「デスクトップは出るのに Firefox が何も開けない」という、
# 原因の分かりにくい状態になる。実際そうなった。
# 無線は別立てにする。
#   wpasupplicant  暗号化された AP に繋ぐのに要る。これが無いと
#                  ドライバが動いていても一切繋がらない
#   iw             AP を探す (scan) / 状態を見る
#   wireless-regdb regulatory.db。CFG80211_REQUIRE_SIGNED_REGDB=y なので
#                  これが無いとチャンネルが world ロックのままになる
WIFI="wpasupplicant iw wireless-regdb rfkill"

NET="iproute2 isc-dhcp-client $WIFI"

# 音。
# alsa-utils が要るのは再生のためだけでなく、ミュートを外すため。
# ALSA は初期状態でミュートになっている機械が多く、
# 「音が出ない = 壊れている」と誤解される。
# pulseaudio は Firefox とゲームがまず前提にしているので入れる。
# pulseaudio-utils は pactl のため。出口の選び直しに使う。
SOUND="alsa-utils pulseaudio pulseaudio-utils libopenal1"

# 時刻合わせ。
# 本体の時計 (RTC) は放っておくと月に何分もずれる。ずれた時計は
# 「証明書がまだ有効でない」で HTTPS が全部落ちる、という形で刺さり、
# 原因がまるで分からない壊れ方をする。
#
# chrony にしたのは systemd を使っていないから。timesyncd は systemd の
# 一部で単体では動かない。chronyd は自分で daemon になるので、
# myos-init から 1 行呼ぶだけで済む。
TIMESYNC="chrony"

# 日本語の字形。
# 無いと日本語が全部豆腐になる。ファイル名もメモ帳も化けるので、
# 見た目の話ではなく使えるかどうかの話。
FONTS="fonts-vlgothic"

# 日本語入力。
# これまで IME が 1 つも入っておらず、かなも漢字も一切打てなかった。
#   fcitx5        本体。XIM のサーバも兼ねるので、自作の Xlib アプリからも使える
#   fcitx5-mozc   変換エンジン。ibus-anthy 経路 (38MB) より小さく質も上
#   設定 GUI (fcitx5-config-qt) は入れない。Qt を丸ごと引いてくるうえ、
#   既定のままで使えるので割に合わない
#   locales       XIM は UTF-8 のロケールでないと動かない
# 合計 21MB ほど。
IME="fcitx5 fcitx5-mozc fcitx5-frontend-gtk3 locales"

SECURITY="ufw iptables nftables \
          clamav clamav-freshclam clamav-daemon \
          cron inotify-tools \
          openssh-server"

# ドライバのファームウェア。
# カーネルにドライバを組み込んでも、ファームウェアの blob は別途要る
# (Wi-Fi、最近の GPU、一部の有線 LAN など)。
# Debian では non-free-firmware コンポーネントに入っている。
FIRMWARE="firmware-linux-free firmware-misc-nonfree firmware-realtek \
          firmware-iwlwifi firmware-atheros firmware-brcm80211 \
          firmware-amd-graphics firmware-intel-sound \
          firmware-sof-signed"

echo "=== 要らなくなったものを外す ==="
# ビルドは既存のルートに上書きしていくので、
# 「入れないようにした」だけでは前回入れたものが残る。明示的に外す。
#
# 追加パッケージを入れる *前* にやること。
# JDK を消すと、その依存として自動で入っていた JRE も
# 「もう誰も要らない」と判断されて autoremove に持っていかれる。
# 先に消してから JRE を入れ直せば、その順序の問題が起きない。
# (順序を逆にすると java だけ残ってライブラリが消え、
#  "libjli.so が無い" という分かりにくい壊れ方をする)
chroot "$WORK" sh -c "apt-get purge -y \
    openjdk-17-jdk openjdk-17-jdk-headless \
    geany geany-common >/dev/null 2>&1; \
    apt-get autoremove --purge -y >/dev/null 2>&1; \
    apt-get clean" || true

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

# まとめて入れて、駄目なら 1 つずつ入れ直す。
#
# apt はリストに 1 つでも知らない名前があると、その 1 つのために
# 全部を入れずに終わる。パッケージ名は Debian の版で変わる
# (gvfs-bin のように消えるものがある) ので、まとめ入れが失敗したら
# 1 つずつ試して、駄目なものだけを飛ばす。
# 黙って何も入らないより、何が入らなかったかが分かるほうがいい。
apt_install() {
    label="$1"; shift
    opts="$1"; shift
    if chroot "$WORK" sh -c "apt-get install -y $opts $* >/dev/null 2>&1"; then
        echo "  $label: まとめて入った"
    else
        echo "  $label: まとめ入れに失敗。1 つずつ試す"
        for pkg in $*; do
            chroot "$WORK" sh -c \
                "apt-get install -y $opts $pkg >/dev/null 2>&1" \
                || echo "    $pkg >>> 入らなかった"
        done
    fi
    chroot "$WORK" apt-get clean
}

echo "=== 一般アプリ用のランタイム ==="
# ここは --no-install-recommends を外す。GTK のテーマや
# アイコンの読み込みは recommends 側に入っていることが多く、
# 外すと落としてきたアプリが起動時に転ぶ。
apt_install "アプリ用ランタイム" "" $APPS

echo "=== セキュリティ (ufw / ClamAV / sshd) ==="
apt_install "セキュリティ" "--no-install-recommends" $SECURITY

echo "=== インストーラが使う道具 ==="
apt_install "インストーラ" "--no-install-recommends" $INSTALLER

echo "=== ネットワーク ==="
apt_install "ネットワーク" "--no-install-recommends" $NET

echo "=== 音 ==="
apt_install "音" "--no-install-recommends" $SOUND
apt_install "時刻合わせ" "--no-install-recommends" $TIMESYNC

echo "=== 日本語フォント ==="
apt_install "フォント" "--no-install-recommends" $FONTS

echo "=== 日本語入力 ==="
apt_install "日本語入力" "--no-install-recommends" $IME

# XIM は UTF-8 のロケールでないと動かない。Debian の最小構成には C しか
# 無いので、ja_JP.UTF-8 を作る。作らずに setlocale すると XSupportsLocale が
# 偽を返し、日本語入力が黙って死ぬ。
if [ -f "$WORK/etc/locale.gen" ] || chroot "$WORK" sh -c "command -v locale-gen" >/dev/null 2>&1; then
    printf 'ja_JP.UTF-8 UTF-8\nen_US.UTF-8 UTF-8\n' > "$WORK/etc/locale.gen"
    chroot "$WORK" sh -c "locale-gen >/dev/null 2>&1" || true
    printf 'LANG=ja_JP.UTF-8\n' > "$WORK/etc/default/locale"
    echo "  ja_JP.UTF-8 / en_US.UTF-8"
fi

# 証明書。これが無いと HTTPS が全部こけるので、
# ClamAV の定義取得より先に必ず通しておく。
chroot "$WORK" sh -c "apt-get install -y --no-install-recommends \
    ca-certificates >/dev/null 2>&1; update-ca-certificates >/dev/null 2>&1" || true

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

# 自分の名前を引けるようにしておく。
# 無いと sudo が毎回 "unable to resolve host myos" を出し、
# しかも名前解決の待ちが入るので体感で分かるほど遅くなる。
# 127.0.1.1 は Debian の作法 (127.0.0.1 は localhost 専用にしておく)。
cat > "$WORK/etc/hosts" <<'HOSTS'
127.0.0.1	localhost
127.0.1.1	myos

::1		localhost ip6-localhost ip6-loopback
ff02::1		ip6-allnodes
ff02::2		ip6-allrouters
HOSTS
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
# (systemd を使っていないので poweroff コマンドは無い。自前の
#  myos-poweroff が reboot(2) を呼ぶ。ここを /sbin/poweroff の
#  ままにしていたため、メニューの Shut Down が無反応だった)
%sudo ALL=(ALL) NOPASSWD: /usr/local/bin/myos-poweroff

# 無線 LAN の操作も同じ扱いにする。
# scan も wpa_supplicant も dhclient も CAP_NET_ADMIN が要るが、
# デスクトップは一般ユーザーで動くので、そのままでは何も出来ない。
# 出来ることを myos-wifi の 5 つ (list/scan/connect/status/disconnect) に
# 絞ってあるので、ここを開けても増えるのはその 5 つだけ。
%sudo ALL=(ALL) NOPASSWD: /usr/local/bin/myos-wifi
# 閲覧ソフトなどを入れるのに要る。myos-pkg はカタログ
# (/etc/myos/catalog.conf) に載っている id しか受け付けないので、
# ここが「パスワード無しで何でも入れられる」口にはならない。
%sudo ALL=(ALL) NOPASSWD: /usr/local/bin/myos-pkg
# カーネルの入れ替え。取ってくる先は myos-update の中で決め打ちに
# してあり、引数から URL は渡せない。渡せるようにすると、任意の中身を
# 生領域へ書かせる口になる。
%sudo ALL=(ALL) NOPASSWD: /usr/local/bin/myos-update
EOF
chmod 440 "$WORK/etc/sudoers.d/myos"

# --- 時計 -------------------------------------------------------------------
# 本体の時計 (RTC) に何が入っているかは OS ごとの取り決めで、
#   Windows  ローカル時刻
#   Linux    UTC
# と割れている。myOS は 98 の顔をしているので、載っている機械の RTC は
# ほぼ間違いなく Windows が書いたローカル時刻になっている。VirtualBox も
# 既定でホストのローカル時刻を渡してくる。
#
# 何も書かないと Linux 側の既定 (UTC) で読むので、日本だと 9 時間ずれる。
# 実際 VirtualBox で 21:46 が 06:46 と出ていた。
mkdir -p "$WORK/etc"
cat > "$WORK/etc/adjtime" <<'EOF'
0.0 0 0.0
0
LOCAL
EOF

# 時刻合わせの設定。
#
# rtcsync は入れない。あれは chronyd に本体の時計を書かせる仕組みで、
# 必ず UTC で書く。myOS は RTC をローカル時刻として扱う (上の LOCAL) ので、
# 両方を有効にすると起動のたびに 9 時間ずれる。
# 本体の時計への書き戻しは、終了するときに myos-poweroff が
# hwclock --systohc でやる。あちらは /etc/adjtime を見るので食い違わない。
#
# makestep を無制限にしてあるのは、置いてある機械の時計が
# 何年もずれていることがあるため。既定 (最初の 3 回だけ) だと、
# 大きくずれた時計を少しずつしか直せず、いつまでも合わない。
mkdir -p "$WORK/etc/chrony" "$WORK/var/lib/chrony"
cat > "$WORK/etc/chrony/chrony.conf" <<'EOF'
pool 2.debian.pool.ntp.org iburst
driftfile /var/lib/chrony/chrony.drift
makestep 1.0 -1
logdir /var/log/chrony
EOF

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

# /dev/root を実体へ向けておく。
#
# カーネルが root= でそのままマウントすると (myOS は initramfs に /init を
# 置かないのでこの道を通る)、/proc/mounts に出てくる名前が **/dev/root**
# になる。ところがそんなデバイスファイルは無い。実機で踏んだ:
#
#   myos-setres: /dev/root がありません
#
# 道具の側で名前に頼らないようにはしたが (デバイス番号から引く)、
# df や mount の表示は /dev/root のままだし、ここを見に行くものが
# 他にもあれば同じことが起きる。**張っておくのが筋。**
#
# 場所は /proc/self/mountinfo の 3 列目 (major:minor) から引く。
# 名前ではなく番号なので、/dev/root と書かれていようが関係ない。
if [ ! -e /dev/root ]; then
    mm=$(awk '$5 == "/" { print $3; exit }' /proc/self/mountinfo 2>/dev/null)
    if [ -n "$mm" ]; then
        real=$(readlink -f "/sys/dev/block/$mm" 2>/dev/null)
        name=${real##*/}
        # ルートは普通パーティション (sda2)。ディスクそのものではない。
        # 意味のとおりに、パーティションへ向ける。
        if [ -n "$name" ] && [ -b "/dev/$name" ]; then
            ln -sf "/dev/$name" /dev/root
        fi
    fi
fi

hostname myos

# 本体の時計を読む。/etc/adjtime の LOCAL を見て、ローカル時刻として
# 解釈してくれる。systemd を使っていないので、これを呼ばないと誰も
# やらない。カーネルが起動時に入れた値は UTC 扱いのままで、日本だと
# 9 時間進んだ時刻になる。
#
# 記録の時刻がずれると追いかけるのが面倒なので、ログを書き始める前にやる。
# 失敗しても起動は続けるが、黙らせない。
# ここが動いていないと時刻が UTC 扱いのままになり、日本だと 9 時間ずれる。
# 黙って捨てていると、ずれているのに理由が分からない。
hwclock --hctosys || echo "[myos-init] hwclock failed; the clock may be off"

# スワップを有効にする。/etc/fstab はインストーラが書く。
# 無い機械 (切れなかった / 手で消した) では何も起きない。
#
# これが無いと、メモリ 1-2GB の機械で Firefox を開いたときに
# OOM killer が走る。前触れなくアプリが消えるので「重い」ではなく
# 「壊れた」に見える。
swapon -a 2>/dev/null || true

# --- 起動中の画面 ---------------------------------------------------------
# ここから先の出力は画面に出さず BOOTLOG.TXT に残す。98 と同じ考え方で、
# 普段は静かに上げて、おかしいときだけ後からログを読む。
# カーネル側は cmdline の quiet loglevel=3 で黙らせてある。
#
# fd 3 と 4 に元のコンソールを取っておく。X が落ちたときだけ、
# ここへ戻して理由を画面に出す。黙って真っ黒のまま止まるのが一番困る。
BOOTLOG=/var/log/bootlog.txt
mkdir -p /var/log
exec 3>&1 4>&2
: > "$BOOTLOG"
exec >>"$BOOTLOG" 2>&1

# 画面には点だけ出す。ブートローダーと installer の initramfs が
# 同じ調子で点を回しているので、そこから続いているように見せる。
say() { printf '%s' "$1" >&3; }

# 起動画面を出す。/dev/fb0 に直接描くので X は要らない。
# 使えるときは点の代わりにこちらを出す。両方出すと、5 回/秒 描き直す
# 起動画面が点を消してしまい、ちらついて見えるだけになる。
SPLASH=""
if [ -x /usr/local/bin/myos-splash ] && \
   /usr/local/bin/myos-splash --test 2>/dev/null; then
    /usr/local/bin/myos-splash >/dev/null 2>&1 &
    SPLASH=$!
    say() { :; }
else
    say '
  Starting myOS '
fi

# udev を上げる。これが無いと X が入力デバイスを見つけられず、
# マウスもキーボードも効かない画面になる (症状が地味なので注意)。
# systemd は使わないが、udevd 単体なら普通に動く。
if [ -x /lib/systemd/systemd-udevd ]; then
    /lib/systemd/systemd-udevd --daemon
    udevadm trigger --action=add >/dev/null 2>&1
    udevadm settle --timeout=10 >/dev/null 2>&1
fi
say .

# --- ファームウェアが要るドライバを繋ぎ直す -------------------------------
# .ko を全て =y にしてある副作用で、PCI の probe はルートのマウントより
# 先に走る。実測で 0.3 秒ほど早い。
#
#   [  2.864773] Direct firmware load for regulatory.db failed
#   [  3.158288] EXT4-fs (sda2): mounted filesystem      ← ルートはここ
#
# そのため無線のファームウェアが読めず、カードが上がってこない。
# GPU は画面を持っているので initramfs にファームを入れて解いたが、
# 無線はここで bind し直すだけで済む。もうルートが見えているので
# /lib/firmware から普通に読める。initramfs に積むと 36MB が 92MB になり、
# ブートローダーが読む時間だけで 14 秒延びたので、こちらを採る。
#
# SOF (最近のノートの音源) も同じ事情なので一緒に扱う。あちらは
# intel/sof/*.ri と音の配線図 (topology) を probe で読む。
rebind_late() {
    for bus in pci usb; do
        for drv in /sys/bus/$bus/drivers/*; do
            [ -d "$drv" ] || continue
            case "${drv##*/}" in
                iwlwifi|ath9k|ath9k_htc|ath10k_pci|ath11k_pci|\
                rtw_8822be|rtw_8822ce|rtw88_pci|rtw89_pci|rtl8xxxu|\
                brcmfmac|b43|mt7601u|mt7921e|rt2800pci|rt2800usb|rt73usb|\
                sof-audio-pci-intel-tgl|sof-audio-pci-intel-cnl|\
                sof-audio-pci-intel-apl|sof-audio-pci-intel-icl|\
                sof-audio-pci-intel-mtl|snd_sof_amd_renoir|\
                snd_sof_amd_rembrandt) ;;
                *) continue ;;
            esac
            for dev in "$drv"/*:*; do
                [ -e "$dev" ] || continue
                id="${dev##*/}"
                echo "$id" > "$drv/unbind" 2>/dev/null || continue
                echo "$id" > "$drv/bind"   2>/dev/null
                echo "[myos-init] rebound $id (${drv##*/})"
            done
        done
    done
}
rebind_late
say .

# --- ネットワーク -------------------------------------------------------
# systemd も NetworkManager も使っていないので、ここで自分で上げる。
#
# 待たないのが肝。DHCP の応答が無い環境 (LAN に繋いでいない等) で
# 起動が何十秒も止まると、故障と区別がつかない。
# 裏で走らせておいて、繋がったら使えるようになる形にする。
ip link set lo up 2>/dev/null

for dev in /sys/class/net/*; do
    [ -e "$dev" ] || continue
    n=$(basename "$dev")
    [ "$n" = "lo" ] && continue

    if [ -d "$dev/wireless" ]; then
        # 無線は鍵が要るので勝手には繋がない。ただし前に繋いだ設定が
        # 残っていれば、そこへは繋ぎ直す。毎回 GUI を開かせるのは
        # 「繋がらない」と同じくらい困る。
        conf="/etc/myos/wpa-$n.conf"
        if [ -f "$conf" ] && command -v wpa_supplicant >/dev/null 2>&1; then
            ip link set "$n" up 2>/dev/null
            wpa_supplicant -B -i "$n" -c "$conf" >/dev/null 2>&1 &&
                dhclient -nw "$n" >/dev/null 2>&1
            echo "[myos-init] wifi: reconnecting $n"
        fi
        continue
    fi

    ip link set "$n" up 2>/dev/null
    if [ -x /sbin/dhclient ] || [ -x /usr/sbin/dhclient ]; then
        dhclient -nw "$n" >/dev/null 2>&1
    fi
done

# DHCP が DNS を教えてくれない網もある。
# そのとき resolv.conf が空だと、繋がっているのに何も引けない。
# 一番分かりにくい壊れ方なので、控えを入れておく。
# (dhclient が持ってきたら、そちらで上書きされる)
if ! grep -q '^nameserver' /etc/resolv.conf 2>/dev/null; then
    printf 'nameserver 1.1.1.1\nnameserver 1.0.0.1\n' >> /etc/resolv.conf
fi

# ファイアウォール。ネットワークが上がる前に入れておく。
# 効いているかどうかは起動ログに出す。黙って失敗していると
# 「入れたつもりで素通し」になり、それが一番まずい。
if [ -x /usr/sbin/ufw ]; then
    /usr/sbin/ufw --force enable >/dev/null 2>&1
    echo "[myos-init] firewall: $(/usr/sbin/ufw status 2>&1 | head -1)"
fi

# 時刻合わせ。網が上がったあとに始める。
# 繋がっていない機械では黙って諦めるだけなので、失敗しても構わない。
# 合わせた結果は終了するときに myos-poweroff が本体の時計へ書き戻す。
if [ -x /usr/sbin/chronyd ]; then
    /usr/sbin/chronyd >/dev/null 2>&1 && echo "[myos-init] time: chronyd started"
fi

# cron。ClamAV の定期スキャンと定義更新がこれで動く。
if [ -x /usr/sbin/cron ]; then
    /usr/sbin/cron
fi

# ウイルス定義。イメージに焼いてあるが、
# 何かの理由で入っていなければ起動後に取りに行く。
# 定義の無い ClamAV は「入っているのに何も見つけない」状態になり、
# 守られているつもりで守られていないのが一番まずい。
if [ -x /usr/bin/freshclam ] && ! ls /var/lib/clamav/*.c[vl]d >/dev/null 2>&1; then
    echo "[myos-init] virus definitions missing, fetching in the background"
    (sleep 20; /usr/bin/freshclam --quiet) &
fi

# --- 音 -----------------------------------------------------------------
# ALSA は初期状態でミュートになっている機械が多い。外しておかないと
# 「音が出ない = 壊れている」と思われる。原因が見えない類の不具合。
if [ -x /usr/sbin/alsactl ]; then
    /usr/sbin/alsactl init >/dev/null 2>&1
fi
if [ -x /usr/bin/amixer ] && [ -r /proc/asound/cards ]; then
    for c in $(sed -n 's/^ *\([0-9]\+\) .*/\1/p' /proc/asound/cards); do
        # 音量も一緒に上げる。ミュートを外しただけで 0% のままだと、
        # 「鳴らない」という同じ結果になる。AC97 は PCM が別系統なので
        # Master だけ上げても出ない。
        for ctl in Master PCM Speaker Headphone Front "Master Mono" \
                   "Line Out" "Digital" "Speaker+LO" "PCM Out"; do
            amixer -c "$c" sset "$ctl" unmute  >/dev/null 2>&1
            amixer -c "$c" sset "$ctl" 80%     >/dev/null 2>&1
        done
        # ノートによっては、挿していないヘッドホンを検出したことにして
        # スピーカーを黙らせる設定が既定で入っている。
        amixer -c "$c" sset "Auto-Mute Mode" Disabled >/dev/null 2>&1
        # AC97 の外部アンプ。切れていると何をしても出ない。
        amixer -c "$c" sset "External Amplifier" unmute >/dev/null 2>&1
    done
fi

# USB メモリなどを自動でマウントする常駐を上げる
/usr/local/bin/myos-automount &
say .

echo "[myos-init] input devices:"
ls /dev/input 2>&1
echo "[myos-init] framebuffer:"
ls -l /dev/fb* 2>&1
echo "[myos-init] starting X on the framebuffer set up by our bootloader"
say .

# X に画面を渡す前に起動画面を止める。残しておくと 5 回/秒 で
# デスクトップの上に描き続けることになる。
if [ -n "$SPLASH" ]; then
    kill "$SPLASH" 2>/dev/null
    wait "$SPLASH" 2>/dev/null
fi

# デスクトップが落ちたら立て直す。
#
# タスクマネージャからウィンドウマネージャを終了させると、xinit は
# 「セッションの主が終わった」と見なして X ごと畳む。今まではそこで
# シェルに落ちていたので、間違って殺すと再起動するしか戻る道が無かった。
# Windows が explorer.exe を立て直すのと同じで、勝手に戻すのが正しい。
#
# ただし無条件に繰り返すと、X がそもそも起動できない機械 (設定が壊れた、
# ドライバが無い) で永久に画面が明滅するだけになる。すぐ死んだ場合だけを
# 「失敗」と数えて、3 回続いたら諦めてシェルを出す。
# 一度でも長く動いていれば数え直す (使っている最中の事故は何度でも直す)。
X_FAILS=0
while :; do
    X_START=$(cut -d. -f1 /proc/uptime)

    /usr/bin/xinit /myos-session -- \
        /usr/bin/X :0 vt1 -nolisten tcp -novtswitch -logverbose 6
    rc=$?

    # 電源を切るときも X は落ちる。ここで抜ける。
    [ -e /run/myos-shutdown ] && break

    X_END=$(cut -d. -f1 /proc/uptime)
    X_LIVED=$((X_END - X_START))

    if [ "$X_LIVED" -ge 20 ]; then
        X_FAILS=0
    else
        X_FAILS=$((X_FAILS + 1))
    fi

    if [ "$X_FAILS" -ge 3 ]; then
        echo "[myos-init] the desktop failed $X_FAILS times in a row; giving up"
        break
    fi

    echo "[myos-init] the desktop exited with $rc after ${X_LIVED}s; restarting"
    printf '\033[2J\033[H' >&3 2>/dev/null
    printf '\n  The desktop stopped. Starting it again...\n' >&3 2>/dev/null
    sleep 1
done

# 電源を切るときも X は落ちる。myos-poweroff がその直前に置く印を見て、
# 正常な終了と異常な終了を分ける。
#
# 印が無いころは、電源を切るたびに下の cat が走って Xorg のログ全文が
# 画面に流れていた。「シャットダウンでログが滝のように出る」の正体。
if [ -e /run/myos-shutdown ]; then
    # 何も出さない。画面を消して、電源が落ちるのを待つだけ。
    # このあと myos-poweroff が終了の画面を描く。
    printf '\033[2J\033[H' >&3 2>/dev/null
    echo "[myos-init] X exited with $rc (shutting down)"
    while :; do sleep 5; done
fi

# ここから先は「X が落ちた」= 異常なので、画面に戻して理由を見せる。
# rc は exec より前に取っておくこと。順番を逆にすると exec の結果になる。
exec >&3 2>&4
echo
echo "[myos-init] X exited with $rc. Xorg log follows:"
cat /var/log/Xorg.0.log 2>&1
echo "[myos-init] boot log is in $BOOTLOG"

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

# キーボード配列。ログオン画面でパスワードを打つ時点でもう要る。
kl=$(sed -n 's/^XKBLAYOUT="\(.*\)"$/\1/p' /etc/default/keyboard 2>/dev/null | head -1)
[ -n "$kl" ] && setxkbmap -model pc105 -layout "$kl" >/dev/null 2>&1

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

# --- 日本語入力 ---------------------------------------------------------
# fcitx5 は XIM のサーバも兼ねる。自作の Xlib アプリは XMODIFIERS を見て
# そこへ繋ぐ (x98.h の x98_im_setup_locale)。GTK の Firefox は
# GTK_IM_MODULE を見る。両方立てておく。
#
# LANG も要る。XIM は UTF-8 のロケールでないと動かない。
export LANG="${LANG:-ja_JP.UTF-8}"
export XMODIFIERS="@im=fcitx"
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx

# fcitx5 は dbus を使う。systemd を使っていないので誰も立てていない。
# ここでセッションバスを起こす。無いと fcitx5 が黙って死ぬ。
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] && command -v dbus-launch >/dev/null 2>&1; then
    eval "$(dbus-launch --sh-syntax --exit-with-session 2>/dev/null)"
fi

# 配列をいまの X に入れ直す。
# xorg.conf.d でも効くが、こちらが最後の砦。
# fcitx5 は起動時に X から配列を読み取ってそれを自分の既定にするので、
# fcitx5 を上げる前にやる必要がある。逆にすると fcitx5 が us を握って
# しまい、以後 fcitx5 が配列を戻してくる。
kl=$(sed -n 's/^XKBLAYOUT="\(.*\)"$/\1/p' /etc/default/keyboard 2>/dev/null | head -1)
[ -n "$kl" ] && setxkbmap -model pc105 -layout "$kl" >/dev/null 2>&1

if command -v fcitx5 >/dev/null 2>&1; then
    fcitx5 -d >/dev/null 2>&1
fi

# 音。pulseaudio はユーザーごとに 1 つ動く作りなのでここで上げる。
# Firefox もゲームもまずこれを探す。
# --exit-idle-time=-1 は「誰も使っていなくても落ちない」。
# 落ちたあと再生しようとしたアプリが黙って無音になるのを防ぐ。
if command -v pulseaudio >/dev/null 2>&1; then
    pulseaudio --start --exit-idle-time=-1 >/dev/null 2>&1

    # 画面と音の出口が別々にあると、pulseaudio が HDMI のほうを既定に
    # することがある。挿していないケーブルへ流れるので、本体の
    # スピーカーからは何も聞こえない。実機でよくある「音が出ない」の
    # 正体がこれ。HDMI 以外の出口があれば、そちらへ寄せる。
    if command -v pactl >/dev/null 2>&1; then
        cur=$(pactl get-default-sink 2>/dev/null)
        case "$cur" in
            *hdmi*|*HDMI*)
                alt=$(pactl list short sinks 2>/dev/null |
                      awk '$2 !~ /hdmi|HDMI/ { print $2; exit }')
                [ -n "$alt" ] && pactl set-default-sink "$alt" >/dev/null 2>&1
                ;;
        esac
    fi
fi

# 落としてきたファイルをその場で検査する常駐。
# 一般ユーザーで動かす (見張るのは本人のダウンロード先なので)。
/usr/local/bin/myos-scan-daemon >/dev/null 2>&1 &

# 閲覧ソフトが 1 つも無ければ、一度だけ選ぶ画面を出す。
#
# 1.5.2 からインストールディスクに閲覧ソフトを載せていない。
# デスクトップの Internet を押せばいつでも開けるが、初回は
# 「何を押せばいいか」から分からないので、こちらから出す。
#
# 出すのは一度だけ。断った人に毎回出すのは押し付けになる。
# 印を消せばまた出る。
if [ ! -e "$HOME/.myos/getapps-done" ]; then
    have=""
    for b in firefox-esr firefox epiphany-browser netsurf-gtk chromium; do
        command -v "$b" >/dev/null 2>&1 && { have=1; break; }
    done
    if [ -z "$have" ] && [ -x /usr/local/bin/myos-getapps ]; then
        mkdir -p "$HOME/.myos"
        touch "$HOME/.myos/getapps-done"
        /usr/local/bin/myos-getapps --need-browser >/dev/null 2>&1 &
    fi
fi

# 入っているアプリをスタートメニューに拾い直す。
# あとから .deb などを入れたものが、次のログオンで出てくる。
/usr/local/bin/myos-refresh-menu >/dev/null 2>&1

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

echo "=== 版を刻む ==="
# 入れたときの ISO の版。GUI もインストーラも /usr の中身も、全部この版。
#
# myos-update はここを書き換えない。あれが入れ替えるのはカーネルだけ
# なので、書き換えると「1.5.4 です」と名乗りながら 1.5.4 で直した GUI の
# 不具合を全部抱えている機械が出来る。次に報告を受けたとき、版番号から
# 原因を追えなくなる。カーネルの版は kernel = として別の行に入る。
# ビルドの外から MYOS_VERSION で渡す (ワークフローが渡している)。
# 渡されなければ 0 にしておく。0 は「どの版より古い」ので、
# 更新の確認をすると必ず「新しいのがある」と出る。手で作った
# rootfs でも動きは壊れない。
mkdir -p "$WORK/etc/myos"
printf '# myOS: 入れたときの ISO の版。カーネルを入れ替えても変わらない。\nversion = %s\n' \
    "${MYOS_VERSION:-0}" > "$WORK/etc/myos/version.conf"
cat "$WORK/etc/myos/version.conf"

echo "=== ライセンスと免責を同梱する ==="
# 「使った時点で同意したものとする」と決めた以上、入れた機械の中でも
# 読めないと筋が通らない。ソースを持っていない人のほうが多い。
mkdir -p "$WORK/usr/share/doc/myos"
cp "$ROOTDIR/LICENSE"         "$WORK/usr/share/doc/myos/LICENSE"
cp "$ROOTDIR/LICENSE.ja.md"   "$WORK/usr/share/doc/myos/LICENSE.ja.md"

echo "=== プログラムの追加と削除のカタログ ==="
# 閲覧ソフトはインストールディスクに載せない。
#
#   1. 場所。Firefox は squashfs に潰しても 84MiB あり、ISO の余裕
#      36MiB を 1 つで食い潰していた。外して 667 -> 583MiB になる。
#   2. 選べること。1GB の機械に Firefox は重い。機械に合ったものを
#      あとから選べるほうがいい。
#
# 網が無ければ apt も閲覧ソフトも使えないので、「網に繋がってから
# 落とす」で困る人はいない。
#
# 容量は bookworm のリポジトリで実測した値 (依存を全部足したもの)。
# 書式: 種別|id|名前|パッケージ|MB|デスクトップの名前|アイコン|コマンド|説明
# デスクトップの名前が空なら、アイコンは作らない
# (スタートメニューには myos-refresh-menu が .desktop から拾う)。
mkdir -p "$WORK/etc/myos"
cat > "$WORK/etc/myos/catalog.conf" <<'EOF'
# --- 閲覧ソフト。1 つ選べばよい ---
browser|firefox|Firefox ESR|firefox-esr|271|Internet|globe|myos-browser|Works with the most sites. Uses the most memory.
browser|epiphany|Epiphany (GNOME Web)|epiphany-browser|188|Internet|globe|myos-browser|Lighter. A few sites look wrong.
browser|netsurf|NetSurf|netsurf-gtk|6|Internet|globe|myos-browser|Tiny. Almost no JavaScript, so most of today's web will not open.
# --- そのほか ---
extra|devtools|C compiler (gcc)|gcc make libc6-dev|157|||| Build C programs on the machine itself.
extra|jdk|Java compiler (javac)|default-jdk-headless|75|||| Compile .java, not just run .jar.
extra|cjkfonts|More Japanese fonts|fonts-noto-cjk|88|||| Adds the Noto family next to VL Gothic.
extra|vlc|VLC media player|vlc|84|VLC|app|vlc|Plays video and audio files.
EOF

echo "=== 組み込みドライバの一覧を置く ==="
# .ko は 1 つも無い (全部 =y) が、/lib/modules は作る。
#
# 無いと modprobe が「そんなモジュールは無い」と言って失敗する。
# ルートを動かしている ext4 でさえそう言われる:
#   modprobe: FATAL: Module ext4 not found in directory /lib/modules/6.12.9
# ドライバは動いているのに、である。切り分けのとき紛らわしいし、
# modalias で自動ロードを試みる udev もいちいち空振りする。
#
# 中身は .ko ではなく「組み込まれている 627 個の一覧」。
# tools/modules-builtin に控えてあるものを置いて depmod にかけると、
# modprobe が「それは組み込み済み」と理解して黙って成功するようになる。
# 全部で 324KB。
#
# この一覧はカーネルの設定を変えたときだけ作り直す。作り方は
# docs/kernel.md に書いてある (build_kernel.sh には書かない。
# あれを触るとカーネルのキャッシュが外れて、中身が同じでも
# 作り直しになるため)。
MODSRC="$ROOTDIR/tools/modules-builtin"
if [ -d "$MODSRC" ]; then
    for d in "$MODSRC"/*/; do
        [ -d "$d" ] || continue
        kv="$(basename "$d")"
        mkdir -p "$WORK/lib/modules/$kv"
        cp "$d"* "$WORK/lib/modules/$kv/"
        chroot "$WORK" depmod -b / "$kv" 2>/dev/null || true
        echo "--- /lib/modules/$kv ($(du -sh "$WORK/lib/modules/$kv" | cut -f1)) ---"
    done
fi

echo "=== Xorg の設定 (modesetting + glamor) ==="
# GPU を使う。カーネルの DRM (i915 / amdgpu / nouveau) の上で
# modesetting ドライバを動かし、描画は glamor に任せる。
# これが無いと全部 CPU が塗ることになり、3D が実用にならない。
mkdir -p "$WORK/etc/X11"
cat > "$WORK/etc/X11/xorg.conf" <<'EOF'
Section "ServerFlags"
    Option "AutoAddDevices" "true"
    Option "DontVTSwitch"   "true"
    Option "BlankTime"      "0"
EndSection

# ドライバは決め打ちにしない。
#
# Driver を書いてしまうと、そのドライバが使えない機械で X が
# 起動しなくなる。書かなければ X が自分で選ぶ:
#   DRM がある     -> modesetting (下の OutputClass で glamor が付く)
#   DRM が無い     -> fbdev に落ちる (起動中に S を押した場合など)
#
# OutputClass は「当てはまったときだけ効く」ので、fbdev への
# 逃げ道を塞がずに glamor の設定だけを足せる。
Section "OutputClass"
    Identifier  "myos-gpu"
    MatchDriver "drm"
    Driver      "modesetting"
    # glamor = OpenGL で描く。GPU がある機械ではこれが効いて
    # ウィンドウの移動やスクロールが GPU 側で行われる。
    Option      "AccelMethod" "glamor"
    Option      "DRI"         "3"
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
    Monitor      "myos-monitor"
    DefaultDepth 24
EndSection
EOF

# キーボード配列。
#
# Debian は /etc/X11/xorg.conf.d を作らない。X の設定ディレクトリとして
# 読まれはするが、無ければ黙って飛ばされるだけ。
# セットアップ (myos-setup) はここへ 10-keyboard.conf を書くので、
# 先に作っておかないと fopen が失敗して、書けていないことに誰も気付かない。
# 実際これで「jp を選んだのに us のまま (@ を押すと [ が出る)」になっていた。
#
# 併せて既定を jp で置いておく。セットアップを通る前の live 環境
# (インストーラを動かしている間) もこれで日本語配列になる。
# us を選んだ場合はセットアップが上書きする。
mkdir -p "$WORK/etc/X11/xorg.conf.d"
cat > "$WORK/etc/X11/xorg.conf.d/10-keyboard.conf" <<'EOF'
Section "InputClass"
    Identifier "myos keyboard"
    MatchIsKeyboard "on"
    Option "XkbLayout" "jp"
    Option "XkbModel" "pc105"
EndSection
EOF

cat > "$WORK/etc/default/keyboard" <<'EOF'
XKBMODEL="pc105"
XKBLAYOUT="jp"
XKBVARIANT=""
XKBOPTIONS=""
BACKSPACE="guess"
EOF

# fcitx5 に配列を触らせない。
#
# fcitx5 の xcb モジュールは、既定で「自分が持っている配列を X に
# 押し付ける」動きをする (Allow Overriding System XKB Settings = True)。
# しかも初回起動時の X の配列をそのまま自分の既定として憶えるので、
# 一度でも us の状態で起動されると、以後 X 側を jp にしても
# fcitx5 が毎回 us に戻してしまう。直したのに直らない、という形になる。
#
# 配列は X が持ち、fcitx5 は変換だけをやる、と決める。
# キーの名前は fcitx5 の設定キーそのもの (説明文と同じ文字列)。
mkdir -p "$WORK/etc/xdg/fcitx5/conf"
cat > "$WORK/etc/xdg/fcitx5/conf/xcb.conf" <<'EOF'
Allow Overriding System XKB Settings=False
EOF

echo "=== myOS ウィンドウマネージャをビルド ==="
# rootfs の中でコンパイルする。
# ホスト (Ubuntu 24.04 / glibc 2.39) と rootfs (Debian bookworm / glibc 2.36)
# では glibc の版が違うので、ホストで作ったバイナリはそのままでは動かない。
# 静的リンクで逃げようとすると、libX11 がカーソル作成で libXcursor を
# dlopen した瞬間に別版の glibc を読み込んで落ちる (SIGFPE)。
# 中でコンパイルしてしまうのがいちばん素直。

# gcc の有無だけでなくヘッダの有無も見る。
# 最後の掃除で gcc を消しているので、2 回目以降のビルドでは
# 「gcc は無いがヘッダはある」「その逆」といった半端な状態になりうる。
#
# 見るヘッダは Xlib.h だけでは足りない。
# Xlib.h は libx11-dev、X.h は x11proto-dev と、別のパッケージから来る。
# Xlib.h だけ見ていると、X.h が無い状態を「揃っている」と判断して
# ここを飛ばし、コンパイルの段になって
#   Xlib.h:44: fatal error: X11/X.h: No such file or directory
# で落ちる。実際に踏んだ。要るものは全部並べて見る。
need_dev=0
[ -x "$WORK/usr/bin/gcc" ]                    || need_dev=1
[ -f "$WORK/usr/include/X11/Xlib.h" ]         || need_dev=1
[ -f "$WORK/usr/include/X11/Xft/Xft.h" ]      || need_dev=1
[ -f "$WORK/usr/include/X11/X.h" ]            || need_dev=1
[ -f "$WORK/usr/include/X11/keysym.h" ]       || need_dev=1
[ -f "$WORK/usr/include/stdio.h" ]            || need_dev=1
if [ "$need_dev" = "1" ]; then
    echo "--- rootfs に gcc とヘッダを入れる ---"
    cp /etc/resolv.conf "$WORK/etc/resolv.conf"
    mount --bind /proc "$WORK/proc" 2>/dev/null || true
    # --reinstall を付ける。記録と実体がずれていても確実に戻せるように。
    chroot "$WORK" sh -c \
        'apt-get update -qq && apt-get install -y --reinstall \
         --no-install-recommends gcc libc6-dev libx11-dev x11proto-dev \
         libxft-dev \
         >/dev/null && apt-get clean'
    umount "$WORK/proc" 2>/dev/null || true
    for h in X11/Xlib.h X11/X.h X11/keysym.h stdio.h; do
        [ -f "$WORK/usr/include/$h" ] || {
            echo "$h を用意できませんでした" >&2; exit 1; }
    done
fi

mkdir -p "$WORK/usr/local/src"
cp "$ROOTDIR/src/gui/myos_wm.c" "$ROOTDIR/src/gui/myos_files.c" \
   "$ROOTDIR/src/gui/myos_settings.c" "$ROOTDIR/src/gui/myos_notepad.c" \
   "$ROOTDIR/src/gui/myos_image.c" "$ROOTDIR/src/gui/myos_open.c" \
   "$ROOTDIR/src/gui/myos_runas.c" "$ROOTDIR/src/gui/myos_setup.c" \
   "$ROOTDIR/src/gui/myos_login.c" "$ROOTDIR/src/gui/myos_alert.c" \
   "$ROOTDIR/src/gui/myos_shutdown.c" "$ROOTDIR/src/gui/myos_poweroff.c" \
   "$ROOTDIR/src/gui/myos_splash.c" "$ROOTDIR/src/gui/myos_net.c" \
   "$ROOTDIR/src/gui/myos_devmgr.c" "$ROOTDIR/src/gui/myos_taskman.c" \
   "$ROOTDIR/src/gui/myos_getapps.c" \
   "$ROOTDIR/src/gui/x98.h" "$ROOTDIR/src/gui/myosconf.h" \
   "$ROOTDIR/src/gui/filetypes.h" "$WORK/usr/local/src/"
# 起動画面。X はまだ無いので Xlib は使わず、freetype だけを直に叩く。
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-splash \
    /usr/local/src/myos_splash.c -lfreetype -lm
chroot "$WORK" gcc -O2 -I /usr/local/src -I /usr/include/freetype2 \
    -o /usr/local/bin/myos-net /usr/local/src/myos_net.c -lX11 -lXft
# デバイスマネージャー。sysfs と dmesg しか見ないので追加の依存は無い。
chroot "$WORK" gcc -O2 -I /usr/local/src -I /usr/include/freetype2 \
    -o /usr/local/bin/myos-devmgr /usr/local/src/myos_devmgr.c -lX11 -lXft
# タスクマネージャ。Ctrl+Alt+Del から出てくる。
chroot "$WORK" gcc -O2 -I /usr/local/src -I /usr/include/freetype2 \
    -o /usr/local/bin/myos-taskman /usr/local/src/myos_taskman.c -lX11 -lXft
# プログラムの追加と削除。閲覧ソフトはこれで入れる。
chroot "$WORK" gcc -O2 -I /usr/local/src -I /usr/include/freetype2 \
    -o /usr/local/bin/myos-getapps /usr/local/src/myos_getapps.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-wm \
    /usr/local/src/myos_wm.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-files \
    /usr/local/src/myos_files.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-settings \
    /usr/local/src/myos_settings.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-notepad \
    /usr/local/src/myos_notepad.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-image \
    /usr/local/src/myos_image.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-runas \
    /usr/local/src/myos_runas.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-setup \
    /usr/local/src/myos_setup.c -lX11 -lXft
# ログオン画面は /etc/shadow を crypt() で照合するので libcrypt が要る
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-login \
    /usr/local/src/myos_login.c -lX11 -lXft -lcrypt
# myos-open は X に触らない。端末やスクリプトからも使う。
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-open \
    /usr/local/src/myos_open.c
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-alert \
    /usr/local/src/myos_alert.c -lX11 -lXft
# 終了の画面と、実際に電源を切るほう。
# 後者は X に触らない (X が落ちたあとも動く必要がある)。
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-shutdown \
    /usr/local/src/myos_shutdown.c -lX11 -lXft
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-poweroff \
    /usr/local/src/myos_poweroff.c

# 画面の解像度を決める道具。設定アプリの「Display」タブから呼ばれる。
# X では解像度を変えられない (nomodeset + fbdev) ので、
# 次に起動するときの希望をディスクのペイロードテーブルへ書く。
cp "$ROOTDIR/src/gui/myos-setres" "$WORK/usr/local/bin/myos-setres"
chmod 755 "$WORK/usr/local/bin/myos-setres"

# --- インストーラ ---------------------------------------------------------
# インストールディスクの中身は、入る中身と同じ squashfs を使う。
# なのでインストーラもここに入れておく。
mkdir -p "$WORK/usr/local/src/install"
cp "$ROOTDIR/src/install/myos_install_text.c" \
   "$ROOTDIR/src/install/myos_install_gui.c" "$WORK/usr/local/src/install/"
chroot "$WORK" gcc -O2 -I /usr/include/freetype2 -o /usr/local/bin/myos-install-text \
    /usr/local/src/install/myos_install_text.c
chroot "$WORK" gcc -O2 -I /usr/local/src -I /usr/include/freetype2 -o /usr/local/bin/myos-install-gui \
    /usr/local/src/install/myos_install_gui.c -lX11 -lXft

cp "$ROOTDIR/src/install/myos-install-init"    "$WORK/myos-install-init"
cp "$ROOTDIR/src/install/myos-install-session" "$WORK/usr/local/bin/"
cp "$ROOTDIR/src/install/myos-writeboot"       "$WORK/usr/local/bin/"
cp "$ROOTDIR/src/install/myos-mkfwinit"        "$WORK/usr/local/bin/"
cp "$ROOTDIR/src/gui/myos-wifi"                "$WORK/usr/local/bin/"
cp "$ROOTDIR/src/gui/myos-pkg"                "$WORK/usr/local/bin/"
cp "$ROOTDIR/src/gui/myos-browser"            "$WORK/usr/local/bin/"
cp "$ROOTDIR/src/gui/myos-update"             "$WORK/usr/local/bin/"
chmod 755 "$WORK/myos-install-init" \
          "$WORK/usr/local/bin/myos-install-session" \
          "$WORK/usr/local/bin/myos-writeboot" \
          "$WORK/usr/local/bin/myos-mkfwinit" \
          "$WORK/usr/local/bin/myos-wifi" \
          "$WORK/usr/local/bin/myos-pkg" \
          "$WORK/usr/local/bin/myos-browser" \
          "$WORK/usr/local/bin/myos-update"
chmod 755 "$WORK"/usr/local/bin/myos-*

echo "=== 一般の Linux アプリを入れるための道具 ==="
# 「Minecraft みたいな普通の Linux アプリを落としてきて、
#   インストールして、GUI で動かす」を通すためのもの。
#
# 配布のされ方は主に 4 つ。全部 myos-install が引き受ける。
#   .deb        … dpkg で入れる (依存は apt に解決させる)
#   AppImage    … 実行属性を付けてそのまま動かす
#   tar.gz/zip  … ~/Applications に展開して中の実行ファイルを探す
#   .sh / .run  … インストーラなので端末で走らせる
cat > "$WORK/usr/local/bin/myos-install" <<'EOF'
#!/bin/sh
# myOS: 落としてきたアプリを入れる。
#   myos-install <ファイル>
set -e
f="$1"
[ -n "$f" ] || { echo "usage: myos-install <file>"; exit 2; }
[ -e "$f" ] || { echo "myos-install: $f: not found"; exit 1; }

# 入れる前に必ず検査する。ここを通すと素通しになる。
echo "Scanning $f ..."
if clamscan --infected --no-summary "$f" 2>/dev/null | grep -q FOUND; then
    echo
    echo "*** This file is infected. Installation stopped. ***"
    exit 1
fi
echo "Clean."
echo

appdir="$HOME/Applications"

case "$f" in
*.deb)
    echo "Installing package (this needs your password)..."
    sudo apt-get install -y "$(readlink -f "$f")"
    ;;
*.AppImage|*.appimage)
    mkdir -p "$appdir"
    cp -f "$f" "$appdir/"
    chmod +x "$appdir/$(basename "$f")"
    echo "Installed to $appdir/$(basename "$f")"
    echo "Starting it once so it can register itself..."
    "$appdir/$(basename "$f")" &
    ;;
*.tar.gz|*.tgz|*.tar.xz|*.tar.bz2|*.zip|*.7z)
    mkdir -p "$appdir"
    echo "Extracting to $appdir ..."
    case "$f" in
        *.zip) unzip -o -q "$f" -d "$appdir" ;;
        *.7z)  7z x -y -o"$appdir" "$f" >/dev/null ;;
        *)     tar xf "$f" -C "$appdir" ;;
    esac
    echo
    echo "Runnable files found:"
    find "$appdir" -maxdepth 3 -type f -perm -u+x -newer "$f" 2>/dev/null | head -20
    ;;
*.sh|*.run|*.bin)
    chmod +x "$f"
    echo "Running the installer..."
    "$f"
    ;;
*)
    echo "myos-install: don't know how to install $f"
    exit 2
    ;;
esac

echo
echo "Refreshing the Start menu..."
myos-refresh-menu
echo "Done."
EOF

# --- スタートメニューにアプリを拾い直す -------------------------------------
# Linux のアプリは /usr/share/applications に .desktop を置いていく。
# それを読んで myOS のスタートメニューの形に直す。
# あとから入れたアプリが、そのままメニューに出るようにするため。
cat > "$WORK/usr/local/bin/myos-refresh-menu" <<'EOF'
#!/bin/sh
# myOS: 入っているアプリをスタートメニューに拾い直す。
#
# /usr/share/applications と ~/.local/share/applications の .desktop から
# 「名前」と「コマンド」を取り出して ~/.myos/startmenu.conf に書く。
# 先頭には myOS 自前の項目を残す (システム既定をそのまま写す)。
out="$HOME/.myos/startmenu.conf"
mkdir -p "$HOME/.myos"
tmp="$(mktemp)"

# 1. myOS 自前の項目
if [ -f /etc/myos/startmenu.conf ]; then
    grep -v '^# --- installed' /etc/myos/startmenu.conf > "$tmp"
fi

# 2. 入っているアプリ
echo "# --- installed applications (myos-refresh-menu) ---" >> "$tmp"
for d in /usr/share/applications "$HOME/.local/share/applications"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.desktop; do
        [ -f "$f" ] || continue
        # 隠すことになっているもの、端末で動くものは出さない
        grep -qi '^NoDisplay=true' "$f" && continue
        grep -qi '^Terminal=true' "$f" && continue
        grep -qi '^Type=Application' "$f" || continue

        name="$(sed -n 's/^Name=//p' "$f" | head -1)"
        exec_="$(sed -n 's/^Exec=//p' "$f" | head -1)"
        [ -n "$name" ] && [ -n "$exec_" ] || continue
        # .desktop の書式にある %f %u などは落とす
        exec_="$(echo "$exec_" | sed 's/ *%[fFuUdDnNickvm]//g')"
        # メニューの区切りに使っている記号は消しておく
        name="$(echo "$name" | tr -d '|')"
        echo "$name|app|$exec_" >> "$tmp"
    done
done

# 3. AppImage と展開したもの
if [ -d "$HOME/Applications" ]; then
    for f in "$HOME/Applications"/*.AppImage "$HOME/Applications"/*.appimage; do
        [ -f "$f" ] || continue
        echo "$(basename "$f" | sed 's/\.[Aa]pp[Ii]mage$//')|app|$f" >> "$tmp"
    done
fi

# 重複を落として書き出す (同じアプリの .desktop が 2 箇所にあることがある)
awk -F'|' '!/^#/ && NF>=3 { if (seen[$1]++) next } { print }' "$tmp" > "$out"
rm -f "$tmp"
echo "wrote $out"

# 開いていればすぐ反映させる
pkill -USR1 -x myos-wm 2>/dev/null || true
EOF

chmod 755 "$WORK/usr/local/bin/myos-install" \
          "$WORK/usr/local/bin/myos-refresh-menu"

echo "=== ウイルス対策 (ClamAV) ==="
# 定義ファイルをイメージに焼き込んでおく。
# 初回起動時にネットが無くても検査できるようにするため
# (「繋がるまで無防備」を避ける)。約 250MB 増える。
mkdir -p "$WORK/var/lib/clamav"
cat > "$WORK/etc/clamav/freshclam.conf" <<'EOF'
DatabaseOwner clamav
UpdateLogFile /var/log/clamav/freshclam.log
LogVerbose false
LogSyslog false
DatabaseDirectory /var/lib/clamav
DNSDatabaseInfo current.cvd.clamav.net
DatabaseMirror db.local.clamav.net
DatabaseMirror database.clamav.net
Checks 24
EOF
mkdir -p "$WORK/var/log/clamav"
chroot "$WORK" chown -R clamav:clamav /var/lib/clamav /var/log/clamav 2>/dev/null || true

# ビルドしている環境がプロキシ越しに出ている場合の下ごしらえ。
# 普通の環境では何も起きない。
#
# freshclam は root で起動したあと clamav ユーザーに降りるので、
# CA バンドルは「そのユーザーが読める場所」に置く必要がある。
# /root の下に置いたままだと権限で読めず、
# "Problem with the SSL CA cert" とだけ言って落ちる (原因が見えない)。
BUILD_CA=""
for ca in /root/.ccr/ca-bundle.crt /etc/ssl/certs/ca-certificates.crt; do
    if [ -f "$ca" ]; then
        cp "$ca" "$WORK/etc/ssl/certs/myos-build-ca.crt" 2>/dev/null || true
        chmod 644 "$WORK/etc/ssl/certs/myos-build-ca.crt" 2>/dev/null || true
        BUILD_CA=/etc/ssl/certs/myos-build-ca.crt
        break
    fi
done

echo "  定義ファイルを取得中 (数分かかる)"
chroot "$WORK" env \
    CURL_CA_BUNDLE="$BUILD_CA" SSL_CERT_FILE="$BUILD_CA" \
    HTTPS_PROXY="${HTTPS_PROXY:-}" https_proxy="${HTTPS_PROXY:-}" \
    freshclam --quiet 2>/dev/null \
    && echo "  取得できた" || echo "  取得できず (初回起動時に取りに行く)"

# ビルド用の CA は成果物に残さない。
# 出来上がったイメージには関係の無いものなので。
rm -f "$WORK/etc/ssl/certs/myos-build-ca.crt"

# --- 定期スキャン -----------------------------------------------------------
# 「cron でできる?」に対しては、できる。cron を入れてある。
# 毎日 12:30 にホームと /media を検査し、見つかったら隔離して知らせる。
cat > "$WORK/etc/cron.d/myos-clamav" <<'EOF'
# myOS: ClamAV の定期実行
#   分 時 日 月 曜日 ユーザー コマンド
SHELL=/bin/sh
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# 定義ファイルの更新 (毎日 3:12)
12 3 * * *   root  /usr/bin/freshclam --quiet

# 全体のスキャン (毎日 12:30)
30 12 * * *  root  /usr/local/bin/myos-scan --quiet /home /media /tmp

# 起動から 5 分後にも 1 回。電源を切っている時間が長い機械向け
@reboot      root  sleep 300 && /usr/local/bin/myos-scan --quiet /home /media
EOF
chmod 644 "$WORK/etc/cron.d/myos-clamav"

# --- スキャン本体 -----------------------------------------------------------
cat > "$WORK/usr/local/bin/myos-scan" <<'EOF'
#!/bin/sh
# myOS: ClamAV でスキャンする。
#
#   myos-scan [--quiet] <パス...>
#
# 見つかったものは /var/lib/myos/quarantine へ移す (削除はしない。
# 誤検知だったときに戻せなくなるため)。
# 見つかったら画面にも知らせる。
quiet=0
[ "$1" = "--quiet" ] && { quiet=1; shift; }
[ $# -gt 0 ] || set -- "$HOME"

qdir=/var/lib/myos/quarantine
log=/var/log/myos-scan.log
mkdir -p "$qdir" 2>/dev/null
chmod 700 "$qdir" 2>/dev/null

out="$(clamscan -r --infected --no-summary \
        --move="$qdir" --exclude-dir='^/(proc|sys|dev)' \
        --exclude-dir="$qdir" "$@" 2>/dev/null)"
rc=$?

if [ -n "$out" ]; then
    echo "$(date '+%Y-%m-%d %H:%M:%S') $out" >> "$log"
    n="$(echo "$out" | grep -c ':')"
    # 誰かがデスクトップに居れば知らせる。cron から呼ばれると
    # DISPLAY が無いので、そこは自分で用意する。
    myos-notify -warn "Virus found" \
        "ClamAV found $n infected file(s).\nThey were moved to $qdir.\nSee $log for the list."
fi

[ "$quiet" = "1" ] || echo "${out:-No threats found.}"
exit $rc
EOF

# --- ダウンロードの監視 -----------------------------------------------------
# 「ダウンロードするたびに調べて」の部分。
# ダウンロード先を inotify で見張り、書き終わったファイルを検査する。
cat > "$WORK/usr/local/bin/myos-scan-daemon" <<'EOF'
#!/bin/sh
# myOS: ダウンロードされたファイルをその場で検査する常駐。
#
# 監視するのは書き込みが終わった合図 (close_write) と moved_to。
# ダウンロード中の半端なファイルを検査しても意味が無いため。
# 一般ユーザーで動く。デスクトップのセッションから起動される。
dirs=""
for d in "$HOME/Downloads" "$HOME/ダウンロード" "$HOME/Desktop" /media; do
    [ -d "$d" ] || mkdir -p "$d" 2>/dev/null
    [ -d "$d" ] && dirs="$dirs $d"
done
[ -n "$dirs" ] || exit 0

echo "[scan-daemon] watching:$dirs"

# shellcheck disable=SC2086
inotifywait -m -r -q -e close_write -e moved_to --format '%w%f' $dirs |
while read -r f; do
    [ -f "$f" ] || continue
    # 部分ダウンロードのファイルは相手にしない
    case "$f" in
        *.part|*.crdownload|*.tmp|*.partial) continue ;;
    esac
    sleep 1
    res="$(clamscan --infected --no-summary "$f" 2>/dev/null)"
    if [ -n "$res" ]; then
        name="$(basename "$f")"
        virus="$(echo "$res" | head -1 | sed 's/.*: //; s/ FOUND//')"
        # 隔離してから知らせる。知らせている間に開かれると意味が無い。
        qdir="$HOME/.myos/quarantine"
        mkdir -p "$qdir"; chmod 700 "$qdir"
        mv -f "$f" "$qdir/" 2>/dev/null
        myos-alert -error "Virus found" \
            "$name\nis infected with $virus.\n\nThe file was moved to\n$qdir"
    fi
done
EOF

# cron など DISPLAY を持たないところから知らせるための小物
cat > "$WORK/usr/local/bin/myos-notify" <<'EOF'
#!/bin/sh
# myOS: DISPLAY が無いところからでもデスクトップに知らせる。
# cron は環境変数を持っていないので、動いている X を探して差し込む。
if [ -z "$DISPLAY" ]; then
    DISPLAY=:0
    export DISPLAY
fi
user="$(sed -n 's/^user[[:space:]]*=[[:space:]]*//p' /etc/myos/login.conf 2>/dev/null | head -1)"
if [ -n "$user" ] && [ "$(id -un)" != "$user" ]; then
    home="$(getent passwd "$user" | cut -d: -f6)"
    exec su - "$user" -c \
        "DISPLAY='$DISPLAY' XAUTHORITY='$home/.Xauthority' myos-alert $(
            for a in "$@"; do printf "'%s' " "$a"; done)"
fi
exec myos-alert "$@"
EOF

chmod 755 "$WORK/usr/local/bin/myos-scan" \
          "$WORK/usr/local/bin/myos-scan-daemon" \
          "$WORK/usr/local/bin/myos-notify"

echo "=== ファイアウォール (ufw) ==="
# 既定は「入ってくるものは全部断る、出ていくものは許す」。
# 家庭の PC としてはこれが素直で、これだけでだいぶ違う。
cat > "$WORK/etc/ufw/ufw.conf" <<'EOF'
ENABLED=yes
LOGLEVEL=low
EOF
chroot "$WORK" sh -c "ufw default deny incoming >/dev/null 2>&1; \
                      ufw default allow outgoing >/dev/null 2>&1" || true

echo "=== SSH ==="
# 入れてはあるが、既定では起動しない。
# 使うときは myos-ssh on。ファイアウォールの穴もそのとき開ける。
# (最初から開けておくと、置いてあるだけの機械が外から叩かれる)
cat > "$WORK/etc/ssh/sshd_config.d/myos.conf" <<'EOF'
# myOS: ssh の既定
PermitRootLogin no
PasswordAuthentication yes
X11Forwarding yes
EOF

cat > "$WORK/usr/local/bin/myos-ssh" <<'EOF'
#!/bin/sh
# myOS: ssh の受け付けを入り切りする。
#   myos-ssh on | off | status
case "$1" in
on)
    mkdir -p /run/sshd
    ssh-keygen -A >/dev/null 2>&1
    /usr/sbin/sshd
    ufw allow 22/tcp >/dev/null 2>&1
    echo "sshd is now running (port 22 opened)."
    ;;
off)
    pkill -x sshd
    ufw delete allow 22/tcp >/dev/null 2>&1
    echo "sshd stopped (port 22 closed)."
    ;;
*)
    if pgrep -x sshd >/dev/null; then echo "sshd: running"; else echo "sshd: stopped"; fi
    ;;
esac
EOF
chmod 755 "$WORK/usr/local/bin/myos-ssh"

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
html,htm,xhtml|HTML Document|globe|myos-browser %1|myos-notepad %1
pdf|PDF Document|globe|myos-browser %1|

# --- 画像 -------------------------------------------------------------------
png,jpg,jpeg,gif,bmp,ppm,pgm,tif,tiff,webp,ico,xpm|Image|file|myos-image %1|

# --- プログラム -------------------------------------------------------------
# 「開く」はメモ帳で中身を見る、「実行」で走らせる。Windows と同じ考え方。
py|Python Script|app|myos-notepad %1|myos-term python3 %1
sh|Shell Script|app|myos-notepad %1|myos-term sh %1
c|C Source|file|myos-notepad %1|
h|C Header|file|myos-notepad %1|
asm,s|Assembler Source|file|myos-notepad %1|
json,xml,css,js|Source File|file|myos-notepad %1|

# --- Java -------------------------------------------------------------------
# JRE だけ入れてある (容量のため)。.jar と .class は動くが、
# .java のコンパイルは出来ないので「実行」の動詞は付けない。
java|Java Source|java|myos-notepad %1|
jar|Java Archive|java|myos-java -jar %1|myos-term myos-java -jar %1
class|Java Class|java|myos-java %1|

# --- 落としてきたアプリ -----------------------------------------------------
# ダブルクリックでインストーラが端末で走る。中で検査もする。
deb|Debian Package|app|myos-term myos-install %1|
appimage|AppImage Application|app|myos-run-appimage %1|myos-term myos-install %1
run,bin|Installer|app|myos-term myos-install %1|

# --- 書庫 -------------------------------------------------------------------
# 「開く」で中身を見て、「実行」で展開して入れる。
zip,gz,xz,bz2,tar,tgz,7z|Archive|file|myos-term myos-lsarchive %1|myos-term myos-install %1
EOF

# --- 他のアプリからの「開く」を myos-open に集める -------------------------
# filetypes.conf に deb の関連付けはあるのに、Firefox で落とした deb を
# 開こうとすると Firefox 自身の「どのアプリで開くか」が出ていた。
# あちらは freedesktop の関連付け (MIME と .desktop) を見るので、
# こちらの表は参照されない。両方から同じ所へ行くようにする。
mkdir -p "$WORK/usr/share/applications" "$WORK/etc/xdg"

# 壁紙の置き場。設定アプリはここと利用者の Pictures を覗いて一覧にする。
mkdir -p "$WORK/usr/share/myos/wallpapers"

# 受け皿の .desktop。メニューには出さない (NoDisplay)。
# 「開く」の入り口としてだけ登録する。
cat > "$WORK/usr/share/applications/myos-open.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=myOS
Comment=Open with myOS
Exec=/usr/local/bin/myos-open %f
Terminal=false
NoDisplay=true
MimeType=application/vnd.debian.binary-package;application/x-deb;application/x-shellscript;application/x-executable;application/java-archive;application/x-java-archive;application/zip;application/gzip;application/x-tar;application/x-xz;application/x-bzip2;application/x-7z-compressed;application/x-compressed-tar;text/plain;text/x-python;text/x-csrc;text/x-chdr;image/png;image/jpeg;image/gif;image/bmp;image/webp;image/tiff;
EOF

# 既定の割り当て。html と pdf はここに入れない。
# myos-open は html を firefox に渡す作りなので、Firefox からの
# 「開く」をここへ向けると行って戻ってくることになる。
cat > "$WORK/etc/xdg/mimeapps.list" <<'EOF'
[Default Applications]
application/vnd.debian.binary-package=myos-open.desktop
application/x-deb=myos-open.desktop
application/x-shellscript=myos-open.desktop
application/x-executable=myos-open.desktop
application/java-archive=myos-open.desktop
application/x-java-archive=myos-open.desktop
application/zip=myos-open.desktop
application/gzip=myos-open.desktop
application/x-tar=myos-open.desktop
application/x-xz=myos-open.desktop
application/x-bzip2=myos-open.desktop
application/x-7z-compressed=myos-open.desktop
application/x-compressed-tar=myos-open.desktop
text/plain=myos-open.desktop
image/png=myos-open.desktop
image/jpeg=myos-open.desktop
image/gif=myos-open.desktop
image/bmp=myos-open.desktop
image/webp=myos-open.desktop
EOF

# xdg-open そのものも myos-open に向ける。
# /usr/local/bin は PATH で /usr/bin より前なので、こちらが使われる。
# これで「アプリが xdg-open を呼ぶ」経路も 1 箇所に集まる。
cat > "$WORK/usr/local/bin/xdg-open" <<'EOF'
#!/bin/sh
# myOS 版の xdg-open。開く先の判断は myos-open に任せる。
#
# ただし URL はファイルではないので、そのまま渡すと myos-open が
# パスとして探して失敗する。scheme:// が付いていたら閲覧ソフトへ。
case "$1" in
    *://*|mailto:*)
        exec /usr/local/bin/myos-browser "$1"
        ;;
esac
exec /usr/local/bin/myos-open "$1"
EOF
chmod 755 "$WORK/usr/local/bin/xdg-open"

chroot "$WORK" sh -c "update-desktop-database /usr/share/applications \
    >/dev/null 2>&1" || true

echo "=== 補助コマンド ==="
# 端末の中で走らせて、終わっても窓を閉じない。
# py や sh を実行したときに出力が読めるようにするため。
cat > "$WORK/usr/local/bin/myos-term" <<'EOF'
#!/bin/sh
# myos-term <コマンド> [引数...]
# Win98 の DOS 窓の見た目で、コマンドを実行して結果を残す。
# フォントは myos-prompt と揃える。ここは apt のインストールの出力が
# 出る場所なので、CJK が描けないと日本語が全部豆腐になる。
exec xterm -title "myOS - $1" \
    -fg lightgray -bg black -cr white \
    -fa "VL Gothic:antialias=false" -fs 12 \
    -fn 9x15 -geometry 80x25 \
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
    # 上限。以前は 1024 にしていたが、それだと 8GB 積んだ機械でも 1GB で
    # 頭打ちになり、Minecraft のような LWJGL のゲームが動かせない。
    # -Xmx は「上限」であって確保ではないので、大きめでも積んでいない
    # ぶんを取られることはない。1/4 の縛りは残してあるから、
    # メモリの少ない機械では今までどおり小さいままになる。
    [ "$heap" -gt 4096 ] && heap=4096
fi

exec java -Xmx${heap}m -Dsun.java2d.opengl=false "$@"
EOF

# AppImage をダブルクリックで動かす。
cat > "$WORK/usr/local/bin/myos-run-appimage" <<'EOF'
#!/bin/sh
# myOS: AppImage を動かす。実行属性が無ければ付ける。
f="$1"
[ -n "$f" ] || exit 2
[ -x "$f" ] || chmod +x "$f" 2>/dev/null
# FUSE が使えない環境 (コンテナなど) では中身を展開して動かす手もあるので、
# 失敗したらそのやり方を案内する。
"$f" "$@" 2>/tmp/appimage.err || {
    if grep -qi "fuse" /tmp/appimage.err 2>/dev/null; then
        myos-alert -warn "AppImage" \
            "This AppImage needs FUSE.\nTry running it with --appimage-extract-and-run"
    fi
}
EOF

# 書庫の中身を見るだけ。形式ごとにコマンドが違うので吸収する。
cat > "$WORK/usr/local/bin/myos-lsarchive" <<'EOF'
#!/bin/sh
f="$1"
case "$f" in
    *.zip) unzip -l "$f" ;;
    *.7z)  7z l "$f" ;;
    *)     tar tvf "$f" ;;
esac
EOF

chmod 755 "$WORK/usr/local/bin/myos-term" "$WORK/usr/local/bin/myos-term-run" \
          "$WORK/usr/local/bin/myos-java" \
          "$WORK/usr/local/bin/myos-run-appimage" \
          "$WORK/usr/local/bin/myos-lsarchive"

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

# 98 の起動時のバナーと同じ書式にしてある。名前は myOS のもの。
#
# ここは元は "Microsoft(R) Windows 98 / (C)Copyright Microsoft Corp" と
# そのまま出していた。落ちが付いているので冗談ではあるが、配る物の中で
# 他人の名前の著作権表示を出すのは、見た目を似せるのとは別の話になる。
# スタートボタンの旗をやめたのと同じ判断で、名前だけ差し替えた。
cat <<'BANNER'

myOS(R) 98
   (C)Copyright myOS Project 2026.

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

# 文字は Xft の VL Gothic で出す。
# -fn 9x15 のビットマップフォントを使っていたが、あれは CJK の字を
# 持っていないので、apt の日本語の出力が全部豆腐になっていた。
# (画面の他の場所は 1.1.2 で Xft に直したが、端末だけ残っていた)
#
# antialias=false を付けて、なるべく元の当たりの硬さを残す。
# フォントが無い機械のために -fn も残しておく。そちらに落ちても
# 英数字は読める。
exec xterm \
    -title "MS-DOS Prompt" \
    -fg lightgray -bg black -cr white \
    -fa "VL Gothic:antialias=false" -fs 12 \
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
Internet|globe|/usr/local/bin/myos-browser
Java Demo|java|/usr/local/bin/myos-java -jar /usr/local/share/myos/hello.jar
OpenGL Test|app|/usr/bin/glxgears
MS-DOS Prompt|app|/usr/local/bin/myos-prompt
Notepad|file|/usr/local/bin/myos-notepad
Settings|app|/usr/local/bin/myos-settings
Network|globe|/usr/local/bin/myos-net
Removable|folder|/usr/local/bin/myos-files /media
EOF

cat > "$WORK/etc/myos/startmenu.conf" <<'EOF'
# スタートメニュー。ラベル|アイコン|コマンド
Programs|app|/usr/local/bin/myos-files /usr/bin
Documents|folder|/usr/local/bin/myos-files ~
Settings|app|/usr/local/bin/myos-settings
Network|globe|/usr/local/bin/myos-net
Add Programs|app|/usr/local/bin/myos-getapps
Device Manager|computer|/usr/local/bin/myos-devmgr
Task Manager|app|/usr/local/bin/myos-taskman
Removable|folder|/usr/local/bin/myos-files /media
Internet|globe|/usr/local/bin/myos-browser
Notepad|file|/usr/local/bin/myos-notepad
Virus Scan|app|/usr/local/bin/myos-term /usr/local/bin/myos-scan
MS-DOS Prompt|app|/usr/local/bin/myos-prompt
Shut Down|app|/usr/local/bin/myos-shutdown
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

echo "=== 余分なものを落とす ==="
# インストールディスクを CD-R (700MB) に収めるための掃除。
# 消すのは「無くても動作が変わらないもの」に限る。
# ファームウェアやフォントには手を付けない
# (実機で画面や無線が出なくなる / 日本語が豆腐になる)。
#
# SLIM=0 を渡すと掃除しない。
if [ "${SLIM:-1}" = "1" ]; then
    # --- 1. パッケージを外す ---------------------------------------------
    # apt の一覧を消す *前* にやること。消したあとだと
    # 「そんなパッケージは知らない」と言われて purge 自体が中止される。
    #
    # 1 つずつ外す。まとめて渡すと、入っていないものが 1 つあるだけで
    # apt が全体を中止してしまい、何も消えずに終わる。
    # X11 の dev パッケージ (libx11-dev / x11proto-dev) はここで外さない。
    # 数 MB しかないうえ、外すと次のビルドで
    # 「Xlib.h はあるが X.h が無い」のような半端な状態になりやすい。
    # 作り直しが必ず通ることのほうが、数 MB より価値がある。
    #
    # ヘッダは rm で消さず、パッケージとして外す。
    # rm -rf /usr/include すると dpkg の記録だけ残り、
    # 次のビルドで apt が「もう入っている」と判断して入れ直さない。
    # 結果、gcc はあるのに X11/Xlib.h が無い状態になり、
    # 作り直すたびにコンパイルが通らなくなる。
    #
    # fonts-noto-cjk は入れるのをやめたが、ここでも外す。
    # このビルドは既存のルートに上書きしていくので、
    # 一覧から消しただけでは前回入れたものが残る。
    #
    # 89MB (ISO で 58MB) あるのに一度も使われていなかった。
    # x98.h のフォントの並びは
    #   VL Gothic -> VL PGothic -> IPAGothic -> Noto Sans CJK JP -> sans
    # で、VL Gothic (8MB) が先に当たるため Noto まで降りてこない。
    # fonts-japanese-gothic の代替も fonts-vlgothic が提供しているので、
    # 外しても日本語は出る。中国語と韓国語のページは豆腐になるが、
    # その 58MB は無線のファームウェアに回したほうがよい。
    for pkg in gcc g++ cpp build-essential libc6-dev linux-libc-dev \
               gcc-12 cpp-12 g++-12 libstdc++-12-dev libgcc-12-dev \
               geany geany-common \
               fonts-noto-cjk fonts-noto-cjk-extra \
               openjdk-17-jdk openjdk-17-jdk-headless; do
        chroot "$WORK" sh -c "apt-get purge -y $pkg >/dev/null 2>&1" || true
    done
    chroot "$WORK" sh -c "apt-get autoremove --purge -y >/dev/null 2>&1; \
                          apt-get clean" || true

    # --- 2. ファイルを消す -------------------------------------------------
    # 文書。読み物であって動作には要らない。
    #
    # ただし copyright だけは残す。あれは読み物ではなく、配布に付いて
    # 回る条件そのもの。GPL のものは「ライセンス文を一緒に配る」ことが
    # 求められるし、Debian のパッケージはその文面を copyright に置いて
    # いる。丸ごと消すと、条件を満たさないまま配ることになる。
    # 569 個で約 17MB (圧縮すれば数 MiB)。閲覧ソフトを外して空けた
    # 84MiB から見れば安い。
    #
    # myos 自身のライセンスもここに置いてあるので、巻き添えで消えない
    # ようにする (実際、足した直後にこの行で消えていた)。
    find "$WORK/usr/share/doc" -mindepth 1 \
         ! -name copyright \
         ! -path "$WORK/usr/share/doc/myos*" \
         -delete 2>/dev/null || true
    rm -rf "$WORK/usr/share/man"/* "$WORK/usr/share/info"/* 2>/dev/null || true

    # 同梱物の索引。掃除が終わった *あと* に作る。
    # 先に作ると、そのあと消したものまで載ってしまう。
    sh "$ROOTDIR/tools/gen-third-party.sh" "$WORK" \
       "$WORK/usr/share/doc/myos/THIRD-PARTY.md" 2>/dev/null || true
    echo "--- 同梱物の索引: $(grep -c '^| [a-z0-9]' \
         "$WORK/usr/share/doc/myos/THIRD-PARTY.md" 2>/dev/null || echo 0) 件 ---"

    # Noto CJK の実体。purge が効かなかった場合や、前回のビルドの残りが
    # 上書きビルドで生き延びた場合に備えて、ここでも落とす。
    rm -rf "$WORK/usr/share/fonts/opentype/noto" 2>/dev/null || true

    # apt のパッケージ一覧。apt-get update でいつでも作り直せる。
    rm -rf "$WORK/var/lib/apt/lists"/* 2>/dev/null || true
    rm -rf "$WORK/var/cache/apt/archives"/*.deb 2>/dev/null || true

    # 翻訳。日本語と英語だけ残す。
    if [ -d "$WORK/usr/share/locale" ]; then
        find "$WORK/usr/share/locale" -mindepth 1 -maxdepth 1 -type d \
             ! -name 'ja*' ! -name 'en*' ! -name 'C*' \
             -exec rm -rf {} + 2>/dev/null || true
    fi

    # ビルドのときだけ使う中間物
    rm -rf "$WORK/usr/local/src"/* 2>/dev/null || true
    rm -f  "$WORK/var/log"/*.log "$WORK/var/log"/*.gz 2>/dev/null || true

    echo "  掃除後: $(du -sh "$WORK" | cut -f1)"
fi

# --- ビルド中に使った DNS の設定を捨てる ------------------------------------
# chroot の中で apt を動かすために、ビルドマシンの
# /etc/resolv.conf をコピーしてある。そのまま出荷すると
# 「そのマシンでしか届かない DNS」がイメージに焼き込まれ、
# 動かした先では名前解決が全部失敗する。
# (GitHub のランナーで焼くと Azure の内部リゾルバが入り、
#  実機では deb.debian.org すら引けなくなる。実際そうなった)
#
# 中身は起動時に dhclient が書く。空にはせず、何が起きるのかを
# 書き残しておく。DHCP の無い網に繋いだ人がここを見て直せるように。
cat > "$WORK/etc/resolv.conf" <<'RESOLV'
# 起動のたびに dhclient が書き換えます。
# 下は DHCP が DNS を教えてくれなかったときの控えです。
# 好きなものに書き換えて構いません。
nameserver 1.1.1.1
nameserver 1.0.0.1
RESOLV

echo "=== 完成 ==="
du -sh "$WORK"

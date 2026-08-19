#!/bin/sh
# ============================================================================
# make_install_initramfs.sh  -  インストールディスク用の initramfs を作る
#
# やること:
#   1. 起動メディア (CD / USB) を探す
#   2. その中の myos.squashfs を読み取り専用でマウントする
#   3. 上に tmpfs を重ねて (overlay) 書けるようにする
#   4. そこへ switch_root して、インストーラを起動する
#
# squashfs をそのまま live の根っこにしているので、
# 「インストーラが動く環境」と「インストールされる中身」が同じものになる。
# 別々に用意すると、片方だけ古いという事故が起きる。
#
# GPU のファームウェアもここに入れる。組み込み (=y) のドライバは PCI の
# probe がルートのマウントより 1.1 秒早いので、initramfs に置かないと
# amdgpu も radeon も probe の時点で firmware を読めずに転ぶ。しかも
# 転ぶ前に VBE の画面を取り上げるので、AMD の機械では「インストーラの
# 画面すら出ない」ことになる。ここに入れておけば probe に間に合う。
# switch_root で initramfs は捨てられるので、メモリも起動後には戻る。
#
# 使い方:
#   sh tools/make_install_initramfs.sh [出力先] [ルートファイルシステム]
# ============================================================================
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/install-initramfs.cpio.gz}"
FWROOT="${2:-}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BUSYBOX="$(command -v busybox || echo /bin/busybox)"
[ -x "$BUSYBOX" ] || { echo "busybox が見つかりません" >&2; exit 1; }

echo "=== 骨組みを作る ==="
mkdir -p "$WORK"/bin "$WORK"/sbin "$WORK"/proc "$WORK"/sys "$WORK"/dev \
         "$WORK"/run "$WORK"/mnt/medium "$WORK"/mnt/ro "$WORK"/mnt/rw \
         "$WORK"/mnt/root "$WORK"/tmp

cp "$BUSYBOX" "$WORK/bin/busybox"
chmod 755 "$WORK/bin/busybox"

# busybox の別名を張る。init から使うものだけで十分。
for a in sh mount umount mkdir sleep echo cat ls losetup switch_root \
         findfs blkid dmesg mknod modprobe cp mv rm ln grep sed \
         head tail printf test poweroff; do
    ln -sf busybox "$WORK/bin/$a"
done

cat > "$WORK/init" <<'INIT'
#!/bin/busybox sh
# myOS インストールディスクの init。
# ここは initramfs の中なので、まだ本物のルートは無い。

/bin/busybox --install -s /bin 2>/dev/null

mount -t proc     proc /proc
mount -t sysfs    sys  /sys
mount -t devtmpfs dev  /dev 2>/dev/null || true

echo
echo "  myOS installer"
echo

# --- 「動いている」ことを見せる -------------------------------------------
# カーネルは loglevel を下げて黙らせてあるので、ここで何も出さないと
# ブートローダーの画面のあと画面が真っ暗になり、
# 動いているのか止まったのか分からない。
# Stage2 と同じ調子で点を . -> .. -> ... -> . と回す。
dots_n=0
tick() {
    if [ "$dots_n" -ge 3 ]; then
        # 3 つ出したら、戻って空白で潰してから戻る (Stage2 と同じ)
        printf '\b \b\b \b\b \b'
        dots_n=0
    else
        printf '.'
        dots_n=$((dots_n + 1))
    fi
}
# 点を消して次の行へ。文章を出す前に呼ぶ。
untick() {
    while [ "$dots_n" -gt 0 ]; do
        printf '\b \b'
        dots_n=$((dots_n - 1))
    done
    echo
}

printf '  Booting now'

# --- 起動メディアを探す --------------------------------------------------
# CD とは限らない (USB に焼いた場合もある) ので、
# 「myos.squashfs が入っているもの」を探すという条件で見る。
# ラベルで探すやり方もあるが、USB に dd したときにラベルが
# 変わっていることがあるので中身で判断する。
find_medium() {
    for try in 1 2 3 4 5 6 7 8 9 10; do
        for dev in /dev/sr0 /dev/sr1 /dev/sda /dev/sda1 /dev/sdb /dev/sdb1 \
                   /dev/sdc /dev/sdc1 /dev/vda /dev/vdb /dev/hda; do
            [ -b "$dev" ] || continue
            tick
            mount -t iso9660 -o ro "$dev" /mnt/medium 2>/dev/null || \
            mount -o ro "$dev" /mnt/medium 2>/dev/null || continue
            if [ -f /mnt/medium/myos.squashfs ]; then
                untick
                echo "  found the installation medium on $dev"
                return 0
            fi
            umount /mnt/medium 2>/dev/null
        done
        # USB は認識されるまで少し待つことがある
        tick
        sleep 1
    done
    untick
    return 1
}

if ! find_medium; then
    echo
    echo "  Could not find the installation medium."
    echo "  Dropping to a shell."
    exec /bin/sh
fi

# --- squashfs を重ねて書けるようにする ------------------------------------
echo "  mounting the system image"
mount -t squashfs -o ro,loop /mnt/medium/myos.squashfs /mnt/ro || {
    echo "  Could not mount myos.squashfs"; exec /bin/sh; }

mount -t tmpfs -o size=512m tmpfs /mnt/rw
mkdir -p /mnt/rw/upper /mnt/rw/work

mount -t overlay overlay \
      -o lowerdir=/mnt/ro,upperdir=/mnt/rw/upper,workdir=/mnt/rw/work \
      /mnt/root || { echo "  overlay failed"; exec /bin/sh; }

# インストーラが読めるよう、メディアを新しい根っこの下へ移す。
#
# 置き場所は /run の下ではなく /myos-medium にする。
# switch_root の先で PID 1 が /run に tmpfs を被せるので、
# /run/myos に置くとそこで見えなくなってしまう。
# (インストーラからは /run/myos で見えるように、PID 1 側で
#  この /myos-medium を bind し直している)
mkdir -p /mnt/root/myos-medium
mount --move /mnt/medium /mnt/root/myos-medium 2>/dev/null || \
    mount --bind /mnt/medium /mnt/root/myos-medium

# 切り替え先に init が無いとカーネルパニックになる。
# 何が起きたか分からないまま止まるのが一番困るので、
# 無ければシェルを出して調べられるようにしておく。
if [ ! -x /mnt/root/myos-install-init ]; then
    echo
    echo "  /myos-install-init is missing from the system image."
    echo "  The medium booted correctly, but there is nothing to run."
    echo "  Dropping to a shell inside the initramfs."
    echo
    exec /bin/sh
fi

echo "  starting the installer"
echo

exec switch_root /mnt/root /myos-install-init
INIT
chmod 755 "$WORK/init"

# --- GPU のファームウェア -------------------------------------------------
# myos-mkfwinit と同じ理屈で 1 つずつ zstd に潰して置く。
# ルートファイルシステムを渡されなかったときは、この機械の /lib/firmware
# から拾う。どちらも無ければファーム無しで作る (今までと同じ動き)。
FWSRC=""
for cand in "$FWROOT/lib/firmware" "$FWROOT/usr/lib/firmware" /lib/firmware; do
    [ -n "$cand" ] || continue
    case "$cand" in /lib/firmware) [ -n "$FWROOT" ] && continue ;; esac
    [ -d "$cand" ] && { FWSRC="$cand"; break; }
done

if [ -n "$FWSRC" ] && command -v zstd >/dev/null 2>&1; then
    echo "=== GPU のファームウェアを入れる ($FWSRC) ==="
    mkdir -p "$WORK/lib/firmware"
    for d in amdgpu radeon nvidia i915; do
        [ -d "$FWSRC/$d" ] || continue
        cp -a "$FWSRC/$d" "$WORK/lib/firmware/$d"
    done
    # 先にリンクの行き先を直してから潰す。逆にすると、リンクがリンクを
    # 指している場合 (nvidia/gp104 -> gp102 -> gm200) に取りこぼす。
    find "$WORK/lib/firmware" -type l | while IFS= read -r l; do
        t="$(readlink "$l")"
        d="$(dirname "$l")"
        [ -d "$d/$t" ] && continue
        ln -sfn "$t.zst" "$l.zst"
        rm -f "$l"
    done
    find "$WORK/lib/firmware" -type f -print0 |
        xargs -0 -r -P "$(nproc 2>/dev/null || echo 1)" -n 8 zstd -q --rm -19
    dangling="$(find "$WORK/lib/firmware" -xtype l | wc -l)"
    [ "$dangling" -eq 0 ] || { echo "行き先の無いリンクが $dangling 本" >&2; exit 1; }
    echo "  $(du -sh "$WORK/lib/firmware" | cut -f1)"
else
    echo "=== GPU のファームウェアは入れない (見つからない) ==="
fi

echo "=== cpio に固める ==="
( cd "$WORK" && find . | cpio -o -H newc --quiet ) | gzip -9 > "$OUT"

echo "=== 完成 ==="
ls -lh "$OUT"

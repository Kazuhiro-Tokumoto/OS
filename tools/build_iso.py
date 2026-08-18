#!/usr/bin/env python3
# ============================================================================
# build_iso.py  -  CD から起動できるインストールディスク (ISO) を作る
#
# 構成:
#   /boot/boot.img          Stage1 + Stage2 (El Torito のブートイメージ)
#   /boot/payload.bin       ペイロードテーブル + カーネル + initramfs
#   /myos.squashfs          インストールする中身 (圧縮したルート)
#
# 起動の流れ:
#   BIOS が boot.img を 0x7C00 に読み込む (ノーエミュレーション起動)
#     -> Stage1 は Stage2 が既に載っているのを見て読み込みを省く
#       -> Stage2 が INT 13h AH=4Bh で「CD から起動した」と判定する
#         -> payload.bin を 2048 バイトセクタで読む
#           -> Linux カーネル + initramfs
#             -> initramfs が CD を探して squashfs を live で起動
#               -> インストーラ
#
# LBA の解決:
#   Stage2 は payload.bin が ISO のどこにあるかを知らないと読めない。
#   ISO を作ってからでないと位置が決まらないので 2 回作る。
#     1 回目 … 仮の内容で作り、payload.bin と boot.img の LBA を調べる
#     2 回目 … その LBA を埋め込んで作り直す
#   ファイルの大きさは変わらないので、2 回目でも位置はずれない。
#   (ずれていないことは最後に確かめている)
#
# 使い方:
#   python3 tools/build_iso.py --kernel <bzImage> --initrd <cpio.gz> \
#                              --payload build/myos.squashfs
# ============================================================================
import argparse
import struct
import subprocess
import sys
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "boot"
BUILD = ROOT / "build"

SECTOR = 512
CD_SECTOR = 2048
ALIGN = CD_SECTOR // SECTOR              # 4

PTBL_MAGIC = b"MYOSPLD2"
# ペイロードテーブル内のコマンドラインの位置。
# Stage2 の PT_CMDLINE と build_image.py の CMDLINE_OFF と
# 必ず同じ値でなければならない。ここがずれると、
# コマンドラインが空のままカーネルが起動し、
# console= が効かずシリアルに何も出ない (原因が見えない)。
CMDLINE_OFF = 0x28
CMDLINE_MAX = SECTOR - CMDLINE_OFF

# BIOS に読ませる大きさ。Stage1 (1) + Stage2 (17) セクタ。
BOOT_LOAD_SECTORS = 18


def die(msg):
    print(f"[ISO] エラー: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        die(f"{cmd[0]} が失敗しました\n{r.stderr[-2000:]}")
    return r.stdout


def nsect(n):
    return (n + SECTOR - 1) // SECTOR


def align_up(lba):
    return (lba + ALIGN - 1) // ALIGN * ALIGN


# --- ブートイメージ --------------------------------------------------------
def build_boot_img(out):
    """Stage1 + Stage2 を 1 本にする。BIOS はこれを丸ごと 0x7C00 に置く。"""
    stage1 = SRC / "stage1.asm"
    stage2 = SRC / "stage2_linux.asm"

    b1 = BUILD / "stage1.bin"
    b2 = BUILD / "stage2.bin"
    run(["nasm", "-f", "bin", "-I", str(SRC) + "/", "-o", str(b1), str(stage1)])
    run(["nasm", "-f", "bin", "-I", str(SRC) + "/", "-o", str(b2), str(stage2)])

    d1 = b1.read_bytes()
    d2 = b2.read_bytes()
    if len(d1) != SECTOR:
        die(f"stage1 が 512 バイトではありません ({len(d1)})")
    if len(d2) > (BOOT_LOAD_SECTORS - 1) * SECTOR:
        die(f"stage2 が大きすぎます ({len(d2)} > "
            f"{(BOOT_LOAD_SECTORS - 1) * SECTOR})")

    img = bytearray(BOOT_LOAD_SECTORS * SECTOR)
    img[0:len(d1)] = d1
    img[SECTOR:SECTOR + len(d2)] = d2
    out.write_bytes(bytes(img))
    return len(d2)


def patch_cdpt(path, lba_2048):
    """ブートイメージの中の 'CDPT' を探して、その後ろに LBA を書く。

    位置を決め打ちにすると Stage2 を直すたびにずれるので、
    目印を探して書く形にしている。
    """
    data = bytearray(path.read_bytes())
    i = data.find(b"CDPT")
    if i < 0:
        die("ブートイメージに 'CDPT' の目印が見つかりません")
    if data.find(b"CDPT", i + 1) >= 0:
        die("'CDPT' が 2 箇所以上あります")
    struct.pack_into("<I", data, i + 4, lba_2048)
    path.write_bytes(bytes(data))


# --- ペイロード ------------------------------------------------------------
def build_payload(kernel, initrd, cmdline, base_lba_512):
    """ペイロードテーブル + カーネル + initramfs を 1 本にする。

    base_lba_512 は、この塊が CD 上のどこに置かれるか (512 バイト単位)。
    テーブルの中の LBA は Stage2 がそのまま使う絶対値なので、
    ここで下駄を履かせておく必要がある。
    """
    kdata = Path(kernel).read_bytes()
    if kdata[0x202:0x206] != b"HdrS":
        die("bzImage に 'HdrS' がありません")
    setup_sects = kdata[0x1F1] or 4
    setup_len = (setup_sects + 1) * SECTOR
    k_setup, k_body = kdata[:setup_len], kdata[setup_len:]

    idata = Path(initrd).read_bytes() if initrd else b""

    # テーブルは 1 セクタ。中身はディスク版と同じ形。
    parts = []
    lba = align_up(1)                     # テーブルの次から
    def place(blob):
        nonlocal lba
        lba = align_up(lba)
        start, count = lba, nsect(len(blob))
        lba += count
        parts.append((start, blob))
        return start, count

    s_lba, s_cnt = place(k_setup)
    b_lba, b_cnt = place(k_body)
    i_lba, i_cnt = (place(idata) if idata else (0, 0))

    cmd = cmdline.encode("ascii", "replace")
    if len(cmd) >= CMDLINE_MAX:
        die("コマンドラインが長すぎます")

    # テーブルの並びは Stage2 の PT_* と 1 対 1 に対応している。
    #   0x08 setup LBA / 0x0C setup セクタ数
    #   0x10 body  LBA / 0x14 body  セクタ数 / 0x18 body  バイト数
    #   0x1C initrd LBA / 0x20 initrd セクタ数 / 0x24 initrd バイト数
    #
    # バイト数の欄を 0 にしてはいけない。Stage2 は initrd の
    # バイト数を boot_params.ramdisk_size にそのまま入れるので、
    # 0 だとカーネルは「initramfs は無い」と判断してルートを探しに行き、
    # "Unable to mount root fs" で止まる。
    # セクタ数だけ合っていても駄目、というのが分かりにくいところ。
    t = bytearray(SECTOR)
    t[0:8] = PTBL_MAGIC
    struct.pack_into("<IIIIIIII", t, 0x08,
                     base_lba_512 + s_lba, s_cnt,
                     base_lba_512 + b_lba, b_cnt, len(k_body),
                     (base_lba_512 + i_lba) if idata else 0, i_cnt,
                     len(idata))
    t[CMDLINE_OFF:CMDLINE_OFF + len(cmd)] = cmd

    out = bytearray(lba * SECTOR)
    out[0:SECTOR] = t
    for start, blob in parts:
        out[start * SECTOR:start * SECTOR + len(blob)] = blob
    return bytes(out)


# --- ISO -------------------------------------------------------------------
def make_iso(isodir, out, label):
    run(["xorriso", "-as", "mkisofs",
         "-o", str(out),
         "-V", label,
         "-J", "-r",
         "-b", "boot/boot.img",
         "-no-emul-boot",
         "-boot-load-size", str(BOOT_LOAD_SECTORS),
         "-quiet",
         str(isodir)])


def find_lba(iso, path_in_iso):
    """ISO の中のファイルが何番目の 2048 バイトセクタから始まるかを調べる。

    xorriso に聞く。自前で ISO9660 を読んでもいいが、
    作った本人に聞くほうが確実で短い。
    """
    out = run(["xorriso", "-indev", str(iso), "-find", path_in_iso,
               "-exec", "report_lba", "--"])
    for line in out.splitlines():
        # 例: "File data lba:  0 ,  337 ,  1 ,  2048 , '/boot/payload.bin'"
        if "lba:" in line and path_in_iso in line:
            fields = [f.strip() for f in line.split(",")]
            try:
                return int(fields[1])
            except (IndexError, ValueError):
                continue
    die(f"{path_in_iso} の位置が分かりませんでした")


def main():
    ap = argparse.ArgumentParser(
        description="CD から起動できる myOS のインストールディスクを作る")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--payload", required=True,
                    help="インストールする中身 (squashfs)")
    ap.add_argument("--out", default=str(BUILD / "myos-install.iso"))
    ap.add_argument("--label", default="MYOS_INSTALL")
    ap.add_argument("--cmdline",
                    default="console=tty0 console=ttyS0,115200 myos.install=1")
    args = ap.parse_args()

    out = Path(args.out)
    isodir = BUILD / "isoroot"
    if isodir.exists():
        shutil.rmtree(isodir)
    (isodir / "boot").mkdir(parents=True)

    print("[ISO] ブートイメージを作る")
    s2len = build_boot_img(isodir / "boot" / "boot.img")
    print(f"[ISO]   stage2 {s2len} バイト "
          f"(枠は {(BOOT_LOAD_SECTORS - 1) * SECTOR} バイト)")

    # 1 回目は仮の位置で作る。大きさが決まればいい。
    print("[ISO] ペイロードを作る (仮)")
    (isodir / "boot" / "payload.bin").write_bytes(
        build_payload(args.kernel, args.initrd, args.cmdline, 0))

    print("[ISO] インストールする中身を置く")
    payload_src = Path(args.payload)
    if not payload_src.exists():
        die(f"{payload_src} がありません")
    # コピーではなくハードリンクで済ませる (600MB のコピーは無駄)
    link = isodir / "myos.squashfs"
    try:
        link.hardlink_to(payload_src)
    except OSError:
        shutil.copy2(payload_src, link)

    print("[ISO] 1 回目 (位置を調べるため)")
    make_iso(isodir, out, args.label)
    payload_lba = find_lba(out, "/boot/payload.bin")
    boot_lba = find_lba(out, "/boot/boot.img")
    print(f"[ISO]   payload.bin  LBA {payload_lba} (2048 バイト単位)")
    print(f"[ISO]   boot.img     LBA {boot_lba}")

    print("[ISO] 位置を埋め込んで作り直す")
    (isodir / "boot" / "payload.bin").write_bytes(
        build_payload(args.kernel, args.initrd, args.cmdline,
                      payload_lba * ALIGN))
    patch_cdpt(isodir / "boot" / "boot.img", payload_lba)
    make_iso(isodir, out, args.label)

    # 位置がずれていないことを確かめる。ずれていたら起動しない。
    p2 = find_lba(out, "/boot/payload.bin")
    b2 = find_lba(out, "/boot/boot.img")
    if p2 != payload_lba or b2 != boot_lba:
        die(f"作り直しで位置が変わりました "
            f"(payload {payload_lba}->{p2}, boot {boot_lba}->{b2})")
    print("[VERIFY] OK   作り直しても位置は同じ")

    # ISO の中のブートイメージに LBA がちゃんと入っているか
    data = out.read_bytes()
    off = boot_lba * CD_SECTOR
    img = data[off:off + BOOT_LOAD_SECTORS * SECTOR]
    i = img.find(b"CDPT")
    if i < 0:
        die("ISO の中のブートイメージに 'CDPT' がありません")
    got = struct.unpack_from("<I", img, i + 4)[0]
    if got != payload_lba:
        die(f"埋め込んだ LBA が違います ({got} != {payload_lba})")
    print(f"[VERIFY] OK   ブートイメージの CDPT = {got}")

    if img[510:512] != b"\x55\xAA":
        die("ブートイメージの末尾に 0x55AA がありません")
    print("[VERIFY] OK   ブートシグネチャ 0x55AA")

    # ペイロードテーブルが読める位置にあるか
    poff = payload_lba * CD_SECTOR
    if data[poff:poff + 8] != PTBL_MAGIC:
        die("ペイロードテーブルの目印が見つかりません")
    print("[VERIFY] OK   ペイロードテーブル 'MYOSPLD2'")

    size = out.stat().st_size
    print()
    print(f"[ISO] 完成: {out}  ({size:,} バイト = {size / (1 << 20):.0f} MiB)")
    cdr = 737_280_000
    if size <= cdr:
        print(f"[ISO] CD-R (700MB = {cdr:,} バイト) に収まる "
              f"(余裕 {(cdr - size) / (1 << 20):.0f} MiB)")
    else:
        print(f"[ISO] CD-R には収まらない ({(size - cdr) / (1 << 20):.0f} MiB 超過)")


if __name__ == "__main__":
    main()

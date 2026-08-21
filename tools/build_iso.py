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
# CD からも USB からも起動できる (いわゆる isohybrid):
#   ISO9660 は先頭 32KB を「システム領域」として規格上まるごと空けている。
#   xorriso もそこには何も書かない。だからここに、ディスクとして起動する
#   ための入口を置ける。CD 側の仕掛けとは一切重ならない。
#
#     LBA 0-17   Stage1 + Stage2 (/boot/boot.img と同じ中身)
#     LBA 20     ペイロードテーブルの写し
#
#   これで Stage1 も Stage2 も直さずに済む。理由は 2 つ:
#
#   1. Stage1 は 0x7E00 に 'MYS2' があれば読み込みを飛ばす。CD では BIOS が
#      18 セクタまとめて載せてくれるので飛ばし、USB では BIOS が 1 セクタ
#      しか載せないので自分で LBA 1-17 を読む。どちらも既にある動き。
#
#   2. ペイロードテーブルの中の LBA は「ISO の先頭からの 512 バイト単位の
#      絶対値」で入っている (build_payload の base_lba_512)。CD では
#      disk_read が 4 で割って 2048 バイト単位に直し、ディスクではそのまま
#      使う。つまり同じ 1 枚のテーブルが両方で正しい。
#
#   Stage2 が「テーブルはどこか」を CD 用 (cd_ptbl_lba) とディスク用
#   (PTBL_LBA = 20) で切り替えるので、両方に置いておけば必ず当たる。
#
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
PT_FLAGS    = 0x1F8                   # stage2_linux.asm の PT_FLAGS と一致必須
PT_FLAG_INSTALLER = 0x01

# ISO9660 のシステム領域。規格で先頭 32KB が丸ごと空けてある。
# ここに Stage1+Stage2 とペイロードテーブルの写しを置いて、
# USB メモリからも起動できるようにする (isohybrid)。
SYSAREA_SIZE = 32768
# ディスクとして起動したとき Stage2 が読みに行くセクタ。
# stage2_linux.asm の PTBL_LBA と必ず同じ値。
SYSAREA_PTBL_LBA = 20
# コマンドラインの終わりは 0x1F0 まで。
# セクタ末尾 (0x1F0-) には画面の希望と媒体の印を置いてあるので、
# そこまで伸ばせるようにしておくと長いコマンドラインで踏み潰す。
CMDLINE_MAX = 0x1F0 - CMDLINE_OFF

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

    # この媒体は「インストール用」だという印。
    # Stage2 はこれが立っているとき、他のディスクにインストール済みの
    # myOS が無いか探し、あればそちらを起動する。
    #
    # インストールが終わったあと媒体を抜いてもらうのが本筋だが、
    # live 環境は媒体の中の squashfs を root にして動いているため、
    # 動いている間はドライブがふさがっていて排出できない。
    # レガシー BIOS には起動順を変える口も無い。
    # 「抜き忘れても普通に起動する」で面倒を見る。
    t[PT_FLAGS] = PT_FLAG_INSTALLER

    out = bytearray(lba * SECTOR)
    out[0:SECTOR] = t
    for start, blob in parts:
        out[start * SECTOR:start * SECTOR + len(blob)] = blob
    return bytes(out)


# isohybrid が使っているジオメトリ。ディスクの実際の形とは関係なく、
# 「CHS の欄をどう埋めるか」の取り決めでしかない。
#
# 255x63 ではなくこの値なのは、実際に世に出ている hybrid ISO が
# そうしているから。Alpine と Debian の ISO の先頭 512 バイトを読んで
# 終了 CHS を逆算したら、どちらも head の上限が 63、sector が 32 で、
# この形に一致した。実機で起動している書き方に合わせるのが確実。
HYBRID_HEADS = 64
HYBRID_SPT = 32
# パーティションの種別も同じ理由で 0x00 にする。0x00 は本来「空き」の
# 意味だが、isohybrid はここを 0x00 にしていて、それで実機が起動して
# いる。Linux は ISO9660 のほうを読むので、この欄は見ていない。
HYBRID_PART_TYPE = 0x00


def chs_bytes(lba, heads=HYBRID_HEADS, spt=HYBRID_SPT):
    """LBA を CHS の 3 バイトに直す。表せない大きさなら 0xFE 0xFF 0xFF。

    LBA だけ埋めて CHS を適当な値にしてはいけない。BIOS の中には
    エントリの整合性を見て、食い違っていると「壊れたテーブル」と
    判断して起動候補から外すものがある。
    """
    c = lba // (heads * spt)
    h = (lba // spt) % heads
    s = lba % spt + 1                     # セクタ番号は 1 から数える
    if c > 1023:
        return b"\xFE\xFF\xFF"
    return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])


def write_partition_table(mbr, parts):
    """MBR にパーティションエントリを書く。

    BIOS 的にはパーティションテーブルが無くても起動できる実装が多いが、
    無いと「起動できるディスク」と見なさない BIOS もあるので付けておく。
    Stage1 のコードは 0x1BE より手前に収まっているので上書きにならない。
    """
    for i, (start, count, ptype, boot) in enumerate(parts[:4]):
        e = bytearray(16)
        e[0] = 0x80 if boot else 0x00
        e[1:4] = chs_bytes(start)
        e[4] = ptype
        e[5:8] = chs_bytes(start + count - 1)
        struct.pack_into("<II", e, 8, start, count)
        off = 0x1BE + i * 16
        mbr[off:off + 16] = e


def make_hybrid(iso_path, boot_img, ptbl):
    """出来上がった ISO に、ディスクとしての起動口を足す。

    ISO9660 のシステム領域 (先頭 32KB) は規格上まるごと空きで、
    xorriso も触らない。そこへ Stage1 + Stage2 とペイロードテーブルの
    写しを置くと、USB メモリに dd しただけで起動するようになる。

    CD 側は El Torito で /boot/boot.img を読むので、こちらとは
    まったく別経路。だから CD 起動には一切影響しない。
    """
    data = bytearray(iso_path.read_bytes())

    end = SYSAREA_PTBL_LBA * SECTOR + SECTOR      # 使うのはここまで
    if end > SYSAREA_SIZE:
        die(f"システム領域 {SYSAREA_SIZE} バイトに収まりません ({end})")

    # 本当に空いているか確かめてから書く。ここが将来 xorriso に使われる
    # ようになったら、黙って壊すのではなく気づけるようにしておく。
    if any(data[:SYSAREA_SIZE]):
        die("ISO のシステム領域が空ではありません "
            "(xorriso が何か書いた? 壊す前に止めます)")

    if len(boot_img) != BOOT_LOAD_SECTORS * SECTOR:
        die(f"ブートイメージの大きさが違います ({len(boot_img)})")
    if ptbl[:8] != PTBL_MAGIC:
        die("ペイロードテーブルの目印が違います")

    mbr = bytearray(boot_img)

    # ISO 全体を 1 つのパーティションに見せる。中身は ISO9660 なので
    # 種別は 0x83 (Linux) にしておく。BIOS はここを起動可否の判断に
    # 使うだけで、中身の解釈はしない。
    total = (len(data) + SECTOR - 1) // SECTOR
    write_partition_table(mbr, [(0, total, HYBRID_PART_TYPE, True)])

    data[0:len(mbr)] = mbr
    off = SYSAREA_PTBL_LBA * SECTOR
    data[off:off + SECTOR] = ptbl
    iso_path.write_bytes(bytes(data))


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
    # console= は「最後に書いたものが /dev/console になる」。
    # インストーラは画面とキーボードで操作するので tty0 を最後に置く。
    # 逆にするとインストーラの表示がシリアルへ出てしまい、
    # 画面にはカーネルログしか出ないのにキーだけ画面側、という
    # 噛み合わない状態になる (カーネルのログは両方に出るので気付きにくい)。
    #
    # loglevel=3 はカーネルのログでインストーラの画面が
    # 上書きされるのを防ぐため。
    #
    # nomodeset は使わない。あれは KMS を丸ごと切るので、
    # 実機に GPU があっても使われず、描画が全て CPU に落ちる
    # (Minecraft のような 3D は実用にならない)。
    #
    # VirtualBox の既定 (VMSVGA) で画面が真っ黒になる原因は
    # vmwgfx ただ 1 つ。それがブートローダーの用意した画面を奪って
    # 何も映さない。だから KMS 全体ではなく vmwgfx だけを止める。
    #
    # 名前はビルド済みカーネルの System.map で確認したもの:
    #   t vmw_pci_driver_init
    #   d __initcall__kmod_vmwgfx__553_1695_vmw_pci_driver_init6
    # 組み込み (=y) なので modprobe.blacklist は効かない。initcall で外す。
    #
    # これで
    #   実機          i915 / amdgpu / nouveau が動き、GPU 支援が効く
    #   VirtualBox    vmwgfx が出てこないので simpledrm が画面を持つ
    ap.add_argument("--cmdline",
                    default="console=ttyS0,115200 console=tty0 loglevel=3 "
                            "initcall_blacklist=vmw_pci_driver_init myos.install=1")
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

    # --- ここから: USB からも起動できるようにする -------------------------
    print("[ISO] システム領域にディスク用の起動口を置く")
    make_hybrid(out, img, data[poff:poff + SECTOR])

    data = out.read_bytes()
    if data[510:512] != b"\x55\xAA":
        die("先頭セクタに 0x55AA がありません")
    print("[VERIFY] OK   先頭セクタ 0x55AA (ディスクとして起動できる)")
    if data[SECTOR + 3:SECTOR + 7] != b"MYS2":
        die("LBA 1 に Stage2 の目印 'MYS2' がありません")
    print("[VERIFY] OK   LBA 1 に Stage2")
    hoff = SYSAREA_PTBL_LBA * SECTOR
    if data[hoff:hoff + 8] != PTBL_MAGIC:
        die(f"LBA {SYSAREA_PTBL_LBA} にペイロードテーブルがありません")
    if data[hoff:hoff + SECTOR] != data[poff:poff + SECTOR]:
        die("写したペイロードテーブルが元と違います")
    print(f"[VERIFY] OK   LBA {SYSAREA_PTBL_LBA} のテーブルは元と同一")
    if data[0x1BE] != 0x80:
        die("パーティションが起動可能になっていません")
    if data[0x1BF:0x1C2] != b"\x00\x01\x00":
        die(f"開始 CHS が LBA 0 と合っていません ({data[0x1BF:0x1C2].hex()})")
    print("[VERIFY] OK   パーティションテーブル "
          f"(起動可能 / 種別 0x{data[0x1C2]:02X} / 開始 CHS 00 01 00)")
    # CD 側を壊していないこと。ISO9660 の目印は 0x8001 の 'CD001'。
    if data[0x8001:0x8006] != b"CD001":
        die("ISO9660 の目印を壊しました")
    print("[VERIFY] OK   ISO9660 'CD001' は無事")

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

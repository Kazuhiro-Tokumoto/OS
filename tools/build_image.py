#!/usr/bin/env python3
# ============================================================================
# build_image.py  -  myOS のディスクイメージを 1 本作る
#
# 起動に必要なものは全部この 1 個のイメージに入る。
# ブートローダーとカーネルとルートファイルシステムを別のメディアに分けない。
# そのまま USB メモリに dd すれば起動するし、後段で ISO に包むこともできる。
#
#   build/myos.img
#     LBA 0            stage1 (MBR + パーティションテーブル)
#     LBA 1  - 17      stage2_linux
#     LBA 20           ペイロードテーブル ("MYOSPLD2")
#     LBA 64 -         bzImage の setup 部
#     LBA    -         bzImage の本体 (プロテクトモード用)
#     LBA    -         initramfs (使う場合)
#     LBA ROOT -       ext4 のルートファイルシステム (パーティション2)
#
# ペイロードの配置は全て 2048 バイト境界 (512 バイト LBA の 4 の倍数) に
# 揃えてある。CD から起動するとき INT 13h が 2048 バイト単位でしか
# 読めないため、最初からそれに合う置き方にしておく。
#
# bzImage は setup 部と本体を別々の位置に置く。1 つの塊のまま置くと、
# 本体の開始位置が setup_sects 次第で境界からずれてしまうため。
#
# 使い方:
#   # ブートローダーだけのデモ (カーネル無し)
#   python3 tools/build_image.py --stage2 demo
#
#   # Linux を起動する (initramfs を root にする)
#   python3 tools/build_image.py --kernel path/to/bzImage \
#                                --initrd build/initramfs.cpio.gz
#
#   # Linux + ext4 のルートファイルシステム (Firefox 入り)
#   python3 tools/build_image.py --kernel path/to/bzImage \
#                                --rootfs build/rootfs.ext4
# ============================================================================
import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "boot"
BUILD = ROOT / "build"

SECTOR = 512
CD_SECTOR = 2048
ALIGN = CD_SECTOR // SECTOR          # ペイロードの LBA 境界 (= 4)

# Stage1 が読み込むセクタ数。src/boot/stage1.asm の STAGE2_SECTORS と一致必須
STAGE2_MAX_SECTORS = 17

PTBL_LBA = 20                        # stage2_linux.asm の PTBL_LBA と一致必須
PAYLOAD_START_LBA = 64
PTBL_MAGIC = b"MYOSPLD2"
CMDLINE_OFF = 0x28                   # ペイロードテーブル内のコマンドライン位置
CMDLINE_MAX = SECTOR - CMDLINE_OFF

# ルートファイルシステムは 1MB 境界から始める (よくある慣習)
ROOT_ALIGN = 2048

STAGE2_VARIANTS = {
    "linux": "stage2_linux.asm",     # 既定。Linux を起動する
    "demo":  "stage2.asm",           # フェーズ1 + 2-A のデモ
    "v2":    "stage2_v2_color.asm",  # 引き継ぎ資料 2-3節 の 47 バイト版
}

_ok = True


def die(msg):
    print(f"[BUILD] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def check(passed, name, extra=""):
    global _ok
    detail = f"  ({extra})" if extra else ""
    print(f"[VERIFY] {'OK  ' if passed else 'FAIL'} {name}{detail}")
    _ok = _ok and passed


def assemble(asm_name):
    if shutil.which("nasm") is None:
        die("nasm が見つかりません。`apt-get install nasm` を実行してください。")
    src = SRC / asm_name
    if not src.exists():
        die(f"ソースがありません: {src}")
    BUILD.mkdir(parents=True, exist_ok=True)
    out = BUILD / f"{src.stem}.bin"
    lst = BUILD / f"{src.stem}.lst"
    r = subprocess.run(["nasm", "-f", "bin", "-I", str(SRC) + "/",
                        str(src), "-o", str(out), "-l", str(lst)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr, file=sys.stderr)
        die(f"nasm がアセンブルに失敗しました: {asm_name}")
    data = out.read_bytes()
    print(f"[BUILD] assembled {asm_name:<22} -> {out.name:<22} {len(data):5d} bytes")
    return data


def make_stage1(code):
    """Stage1 を 512 バイトのブートセクタに整形する。

    nasm 側でパディングとシグネチャは済んでいるが、資料 5章(1) の教訓どおり
    最終的な 512 バイトはここでも必ず検証する。
    """
    if len(code) > SECTOR:
        die(f"Stage1 が {len(code)} バイトあり 512 バイトに収まりません")
    if len(code) == SECTOR:
        if code[510:512] != b"\x55\xAA":
            die("Stage1 の末尾が 55 AA になっていません")
        return bytearray(code)
    if len(code) > 510:
        die("Stage1 のコードが 510 バイトを超えています")
    return bytearray(code + bytes(510 - len(code)) + b"\x55\xAA")


def nsect(nbytes):
    return (nbytes + SECTOR - 1) // SECTOR


def align_up(v, a):
    return (v + a - 1) // a * a


def split_bzimage(path):
    """bzImage を setup 部と本体に分ける。

    setup 部 = 先頭 (setup_sects + 1) * 512 バイト。
    setup_sects は offset 0x1F1 の 1 バイト。0 のときは 4 とみなす(古い規約)。
    """
    data = path.read_bytes()
    if data[0x1FE:0x200] != b"\x55\xAA":
        die(f"{path} の 0x1FE が 0xAA55 ではありません。bzImage ではなさそうです。")
    if data[0x202:0x206] != b"HdrS":
        die(f"{path} に 'HdrS' マジックがありません。Boot Protocol 非対応です。")
    setup_sects = data[0x1F1] or 4
    split = (setup_sects + 1) * SECTOR
    return data[:split], data[split:], setup_sects


class Layout:
    """イメージに置くものを順番に積んでいくだけの小さな道具。"""

    def __init__(self, start_lba):
        self.lba = start_lba
        self.items = []          # (lba, bytes, label)

    def place(self, blob, label, align=ALIGN):
        if not blob:
            return 0, 0
        self.lba = align_up(self.lba, align)
        lba = self.lba
        self.items.append((lba, blob, label))
        self.lba += nsect(len(blob))
        return lba, nsect(len(blob))


def build_payload_table(k_setup, k_body, initrd, cmdline):
    cmd = cmdline.encode("ascii", "replace")
    if len(cmd) >= CMDLINE_MAX:
        die(f"コマンドラインが長すぎます ({len(cmd)} >= {CMDLINE_MAX})")
    t = bytearray(SECTOR)
    t[0:8] = PTBL_MAGIC
    struct.pack_into("<IIIIIIII", t, 0x08,
                     k_setup[0], k_setup[1],
                     k_body[0], k_body[1], k_body[2],
                     initrd[0], initrd[1], initrd[2])
    t[CMDLINE_OFF:CMDLINE_OFF + len(cmd)] = cmd
    return bytes(t)


def write_partition_table(stage1, parts):
    """MBR にパーティションエントリを書く。

    BIOS 的にはパーティションテーブルが無くても起動できる実装が多いが、
    無いと拒む BIOS もあるので付けておく。Stage1 のコードは 0x1BE より
    手前に収まっているので上書きの心配はない。
    """
    for i, (start, count, ptype, boot) in enumerate(parts[:4]):
        e = bytearray(16)
        e[0] = 0x80 if boot else 0x00
        e[1:4] = b"\x00\x02\x00"        # 開始 CHS (LBA を使うので便宜的な値)
        e[4] = ptype
        e[5:8] = b"\xFE\xFF\xFF"        # 終了 CHS (同上)
        struct.pack_into("<II", e, 8, start, count)
        off = 0x1BE + i * 16
        stage1[off:off + 16] = e


def main():
    ap = argparse.ArgumentParser(description="myOS のディスクイメージを 1 本作る")
    ap.add_argument("--stage2", choices=sorted(STAGE2_VARIANTS), default="linux")
    ap.add_argument("--kernel", help="bzImage のパス")
    ap.add_argument("--initrd", help="initramfs (cpio) のパス")
    ap.add_argument("--rootfs", help="ext4 のルートファイルシステムイメージ")
    ap.add_argument("--cmdline", default=None, help="カーネルコマンドライン")
    ap.add_argument("--out", default=str(BUILD / "myos.img"))
    ap.add_argument("--pad", type=int, default=8,
                    help="末尾に足す余白 (MB)")
    args = ap.parse_args()

    stage1 = make_stage1(assemble("stage1.asm"))
    stage2 = assemble(STAGE2_VARIANTS[args.stage2])

    if len(stage2) > STAGE2_MAX_SECTORS * SECTOR:
        die(f"Stage2 が {len(stage2)} バイトあり "
            f"{STAGE2_MAX_SECTORS} セクタ ({STAGE2_MAX_SECTORS * SECTOR} バイト) "
            f"に収まりません。stage1.asm の STAGE2_SECTORS と "
            f"build_image.py の STAGE2_MAX_SECTORS を両方増やしてください。")

    layout = Layout(PAYLOAD_START_LBA)
    k_setup = k_body = (0, 0, 0)
    initrd_info = (0, 0, 0)
    cmdline = args.cmdline

    if args.kernel:
        kpath = Path(args.kernel)
        if not kpath.exists():
            die(f"カーネルがありません: {kpath}")
        setup_blob, body_blob, setup_sects = split_bzimage(kpath)
        lba, n = layout.place(setup_blob, "kernel setup")
        k_setup = (lba, n)
        lba, n = layout.place(body_blob, "kernel body")
        k_body = (lba, n, len(body_blob))
        print(f"[BUILD] bzImage: setup_sects={setup_sects}, "
              f"setup {len(setup_blob):,}B / body {len(body_blob):,}B")

    if args.initrd:
        ipath = Path(args.initrd)
        if not ipath.exists():
            die(f"initramfs がありません: {ipath}")
        blob = ipath.read_bytes()
        lba, n = layout.place(blob, "initramfs")
        initrd_info = (lba, n, len(blob))

    # --- ルートファイルシステムの位置を決める ---
    rootfs_path = Path(args.rootfs) if args.rootfs else None
    if rootfs_path is not None and not rootfs_path.exists():
        die(f"ルートファイルシステムがありません: {rootfs_path}")

    boot_end_lba = layout.lba
    root_lba = align_up(boot_end_lba, ROOT_ALIGN) if rootfs_path else 0
    root_sects = nsect(rootfs_path.stat().st_size) if rootfs_path else 0

    # --- コマンドラインの既定値を決める ---
    if cmdline is None:
        # nomodeset は画面を DRM ドライバに取られないため。
        # .ko を全て =y にしてあるので vmwgfx なども入っており、
        # 仮想環境によっては起動しているのに画面だけ真っ黒になる。
        base = "console=tty0 console=ttyS0,115200 nomodeset"
        if rootfs_path:
            cmdline = f"{base} root=/dev/sda2 rootfstype=ext4 rw init=/myos-init"
        elif args.initrd:
            cmdline = f"{base} rdinit=/init"
        else:
            cmdline = base

    ptbl = build_payload_table(k_setup, k_body, initrd_info, cmdline)

    # --- パーティションテーブル ---
    total_sects = (root_lba + root_sects) if rootfs_path else boot_end_lba
    total_sects += args.pad * 1024 * 1024 // SECTOR
    total_sects = align_up(total_sects, ROOT_ALIGN)

    parts = [(1, (root_lba or total_sects) - 1, 0x83, True)]
    if rootfs_path:
        parts.append((root_lba, root_sects, 0x83, False))
    write_partition_table(stage1, parts)

    # --- 書き出し (2GB 級になるので seek しながら流し込む) ---
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "wb") as f:
        f.write(bytes(stage1))
        f.seek(SECTOR)
        f.write(stage2)
        f.seek(PTBL_LBA * SECTOR)
        f.write(ptbl)
        for lba, blob, label in layout.items:
            f.seek(lba * SECTOR)
            f.write(blob)
        if rootfs_path:
            f.seek(root_lba * SECTOR)
            with open(rootfs_path, "rb") as rf:
                shutil.copyfileobj(rf, f, 1024 * 1024)
        f.truncate(total_sects * SECTOR)

    verify(out, stage1, stage2, ptbl, layout, rootfs_path, root_lba)

    print()
    print(f"[BUILD] wrote {out}  ({out.stat().st_size:,} bytes "
          f"= {total_sects} sectors)")
    print(f"[BUILD]   LBA {0:<8} stage1 (MBR + partition table)")
    print(f"[BUILD]   LBA {1:<8} {STAGE2_VARIANTS[args.stage2]} ({len(stage2)} bytes)")
    print(f"[BUILD]   LBA {PTBL_LBA:<8} payload table")
    for lba, blob, label in layout.items:
        print(f"[BUILD]   LBA {lba:<8} {label} ({len(blob):,} bytes)")
    if rootfs_path:
        print(f"[BUILD]   LBA {root_lba:<8} ext4 rootfs "
              f"({rootfs_path.stat().st_size:,} bytes)  -> /dev/sda2")
    print(f"[BUILD]   cmdline: {cmdline!r}")


def verify(path, stage1, stage2, ptbl, layout, rootfs_path, root_lba):
    with open(path, "rb") as f:
        head = f.read(SECTOR)
        check(head[0x1FE:0x200] == b"\x55\xAA",
              "boot signature 0x1FE-0x1FF == 55 AA", head[0x1FE:0x200].hex().upper())
        check(head == bytes(stage1), "stage1 occupies sector 0")
        f.seek(SECTOR)
        check(f.read(len(stage2)) == stage2, "stage2 placed at LBA 1")
        # Stage1 が「もう載っている」判定に使う目印
        f.seek(SECTOR + 3)
        magic = f.read(4)
        if stage2[:3] != b"\x00\x00\x00":
            check(magic == b"MYS2" or STAGE2_VARIANTS_HAS_NO_MAGIC(stage2),
                  "stage2 magic 'MYS2' at LBA 1 offset 3", magic.decode("latin1"))
        f.seek(PTBL_LBA * SECTOR)
        check(f.read(8) == PTBL_MAGIC, f"payload table magic at LBA {PTBL_LBA}")
        for lba, blob, label in layout.items:
            check(lba % ALIGN == 0, f"{label} LBA {lba} is 2048-byte aligned")
            f.seek(lba * SECTOR)
            check(f.read(len(blob)) == blob, f"{label} placed at LBA {lba}")
        if rootfs_path:
            f.seek(root_lba * SECTOR + 0x438)
            check(f.read(2) == b"\x53\xEF",
                  f"ext4 magic 0xEF53 at LBA {root_lba} (partition 2)")
    if not _ok:
        die("イメージの検証に失敗しました")


def STAGE2_VARIANTS_HAS_NO_MAGIC(stage2):
    """デモ用の Stage2 には 'MYS2' の目印を付けていないので、その場合は許す。"""
    return b"MYS2" not in stage2[:16]


if __name__ == "__main__":
    main()

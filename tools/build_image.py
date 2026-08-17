#!/usr/bin/env python3
# ============================================================================
# build_image.py  -  myOS ディスクイメージのビルドスクリプト
#
# 引き継ぎ資料 3章の Python 手順を 1 本にまとめ、フェーズ2-B (Linux 起動) 用の
# ハードディスクイメージ生成も扱えるようにしたもの。
#
#   1. src/boot/*.asm を nasm でアセンブル
#   2. Stage1 を 512 バイトに整形 (0 埋め + 55 AA ブートシグネチャ)
#   3. Stage1 + Stage2 (+ ペイロード) を結合してイメージを作る
#   4. 検証 (サイズ / シグネチャ / 配置 / はみ出しチェック)
#
# 2 種類のイメージを作れる:
#
#   フロッピー (既定)  1.44MB。Stage2 のデモ用。
#     python3 tools/build_image.py
#     python3 tools/build_image.py --stage2 v2
#
#   ハードディスク     Linux カーネルを載せる用。bzImage は 1.44MB に
#                      収まらないのでこちら。
#     python3 tools/build_image.py --kernel path/to/bzImage \
#                                  --initrd path/to/initramfs.cpio.gz \
#                                  --cmdline "console=ttyS0,115200 console=tty0"
#
# ハードディスクイメージのレイアウト:
#   LBA 0        Stage1 (MBR)。0x1BE からパーティションテーブルも書く
#   LBA 1-17     Stage2
#   LBA 18       ペイロードテーブル ("MYOSPLD1")
#   LBA 64-      bzImage
#   LBA ...      initramfs
# ============================================================================
import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "boot"
BUILD = ROOT / "build"

SECTOR_SIZE = 512
FLOPPY_SIZE = 1474560   # 1.44MB フロッピーの標準サイズ

# Stage1 が読み込むセクタ数。src/boot/stage1.asm の STAGE2_SECTORS と必ず一致
# させること (ここを増やすなら向こうも直す)。
STAGE2_MAX_SECTORS = 17

# ハードディスクイメージのレイアウト (LBA)
PTBL_LBA = 18           # ペイロードテーブル。stage2_linux.asm の PTBL_LBA と一致
PAYLOAD_START_LBA = 64  # ここから先に bzImage / initramfs を置く
PTBL_MAGIC = b"MYOSPLD1"
CMDLINE_MAX = 256

STAGE2_VARIANTS = {
    "main":  "stage2.asm",              # フェーズ1 + フェーズ2-A のデモ
    "v2":    "stage2_v2_color.asm",     # 引き継ぎ資料 2-3節 の 47 バイト版
    "linux": "stage2_linux.asm",        # フェーズ2-B: Linux Boot Protocol
}


def die(msg: str) -> None:
    print(f"[BUILD] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def assemble(asm_name: str) -> bytes:
    """nasm でフラットバイナリにアセンブルし、リスティングも残す。"""
    if shutil.which("nasm") is None:
        die("nasm が見つかりません。`apt-get install nasm` を実行してください。")

    src = SRC / asm_name
    if not src.exists():
        die(f"ソースがありません: {src}")

    stem = src.stem
    out = BUILD / f"{stem}.bin"
    lst = BUILD / f"{stem}.lst"
    BUILD.mkdir(parents=True, exist_ok=True)

    # -I で inc/ を include パスに通す
    cmd = ["nasm", "-f", "bin", "-I", str(SRC) + "/",
           str(src), "-o", str(out), "-l", str(lst)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        die(f"nasm がアセンブルに失敗しました: {asm_name}")

    data = out.read_bytes()
    print(f"[BUILD] assembled {asm_name:<24} -> {out.name:<24} {len(data):5d} bytes"
          f"  (listing: build/{lst.name})")
    return data


def make_stage1(code: bytes) -> bytearray:
    """Stage1 を 512 バイトのブートセクタに整形する。

    nasm は `times 510-($-$$) db 0` を正しく処理できるので本来ここでの
    パディングは不要だが、資料 5章(1) の教訓どおり「最終的な 512 バイトは
    ビルドスクリプト側でも必ず検証する」方針を守っている。
    """
    if len(code) > SECTOR_SIZE:
        die(f"Stage1 が {len(code)} バイトあり 512 バイトに収まりません")

    if len(code) == SECTOR_SIZE:
        if code[510:512] != b"\x55\xAA":
            die("Stage1 の末尾が 55 AA になっていません")
        return bytearray(code)

    if len(code) > 510:
        die("Stage1 のコードが 510 バイトを超えています (シグネチャが入りません)")
    return bytearray(code + bytes(510 - len(code)) + b"\x55\xAA")


def sectors_for(nbytes: int) -> int:
    return (nbytes + SECTOR_SIZE - 1) // SECTOR_SIZE


def build_payload_table(kernel_lba, kernel_sectors, kernel_size,
                        initrd_lba, initrd_sectors, initrd_size,
                        cmdline: str) -> bytes:
    """stage2_linux.asm が読むペイロードテーブル 1 セクタを作る。"""
    cmd = cmdline.encode("ascii", "replace")
    if len(cmd) >= CMDLINE_MAX:
        die(f"コマンドラインが長すぎます ({len(cmd)} >= {CMDLINE_MAX})")

    tbl = bytearray(SECTOR_SIZE)
    tbl[0x00:0x08] = PTBL_MAGIC
    struct.pack_into("<IIIIII", tbl, 0x08,
                     kernel_lba, kernel_sectors, kernel_size,
                     initrd_lba, initrd_sectors, initrd_size)
    tbl[0x20:0x20 + len(cmd)] = cmd
    return bytes(tbl)


def write_partition_table(stage1: bytearray, total_sectors: int) -> None:
    """MBR にパーティションエントリを 1 つ書く。

    BIOS 的にはパーティションテーブルが無くても「スーパーフロッピー」として
    起動できる実装が多いが、無いと起動を拒む BIOS もあるので付けておく。
    Stage1 のコードは 0x1BE より手前に収まっているので上書きの心配はない。
    """
    part = bytearray(16)
    part[0] = 0x80                      # ブータブルフラグ
    part[1:4] = b"\x00\x02\x00"         # 開始 CHS (head 0, sect 2, cyl 0)
    part[4] = 0x83                      # タイプ: Linux
    part[5:8] = b"\xFE\xFF\xFF"         # 終了 CHS (LBA を使うので便宜的な値)
    struct.pack_into("<II", part, 8, 1, total_sectors - 1)  # 開始 LBA / セクタ数
    stage1[0x1BE:0x1CE] = part


def build_floppy(stage2_variant: str, dump: bool) -> Path:
    stage1 = make_stage1(assemble("stage1.asm"))
    stage2 = assemble(STAGE2_VARIANTS[stage2_variant])

    max_bytes = STAGE2_MAX_SECTORS * SECTOR_SIZE
    if len(stage2) > max_bytes:
        die(f"Stage2 が {len(stage2)} バイトあり、Stage1 が読み込む "
            f"{STAGE2_MAX_SECTORS} セクタ ({max_bytes} バイト) を超えています。"
            f" stage1.asm の STAGE2_SECTORS と build_image.py の "
            f"STAGE2_MAX_SECTORS を両方増やしてください。")

    image = bytearray(FLOPPY_SIZE)
    image[0:SECTOR_SIZE] = stage1
    image[SECTOR_SIZE:SECTOR_SIZE + len(stage2)] = stage2

    out = BUILD / "disk.img"
    out.write_bytes(bytes(image))

    verify_common(out, stage1, stage2)
    check(len(image) == FLOPPY_SIZE, "image size == 1,474,560", f"{len(image)}")
    check(set(image[SECTOR_SIZE + len(stage2):]) <= {0},
          "tail after stage2 is zero-filled", "")
    finish()

    print(f"[BUILD] wrote {out}  ({len(image)} bytes, floppy, "
          f"stage2 variant = {stage2_variant})")

    if dump:
        print()
        hexdump(bytes(image[:SECTOR_SIZE]), "Stage1 (sector 1 / offset 0x0000)", 0)
        print()
        n = min(len(stage2), 256)
        hexdump(bytes(image[SECTOR_SIZE:SECTOR_SIZE + n]),
                "Stage2 (sector 2 / offset 0x0200)", SECTOR_SIZE)
    return out


def build_hdd(kernel: Path, initrd: Path, cmdline: str, dump: bool) -> Path:
    stage1 = make_stage1(assemble("stage1.asm"))
    stage2 = assemble(STAGE2_VARIANTS["linux"])

    max_bytes = STAGE2_MAX_SECTORS * SECTOR_SIZE
    if len(stage2) > max_bytes:
        die(f"Stage2 が {len(stage2)} バイトあり "
            f"{STAGE2_MAX_SECTORS} セクタに収まりません")

    kdata = kernel.read_bytes()
    ksect = sectors_for(len(kdata))
    klba = PAYLOAD_START_LBA

    if initrd is not None:
        idata = initrd.read_bytes()
        isect = sectors_for(len(idata))
        ilba = klba + ksect
        # 4KB 境界に揃えておく (必須ではないが読みやすい)
        ilba = (ilba + 7) & ~7
    else:
        idata, isect, ilba = b"", 0, 0

    end_lba = (ilba + isect) if isect else (klba + ksect)
    total_sectors = end_lba + 2048          # 末尾に少し余裕を持たせる
    total_sectors = (total_sectors + 2047) & ~2047
    image = bytearray(total_sectors * SECTOR_SIZE)

    write_partition_table(stage1, total_sectors)

    image[0:SECTOR_SIZE] = stage1
    image[SECTOR_SIZE:SECTOR_SIZE + len(stage2)] = stage2

    ptbl = build_payload_table(klba, ksect, len(kdata),
                              ilba, isect, len(idata), cmdline)
    image[PTBL_LBA * SECTOR_SIZE:(PTBL_LBA + 1) * SECTOR_SIZE] = ptbl

    image[klba * SECTOR_SIZE:klba * SECTOR_SIZE + len(kdata)] = kdata
    if isect:
        image[ilba * SECTOR_SIZE:ilba * SECTOR_SIZE + len(idata)] = idata

    out = BUILD / "disk_hdd.img"
    out.write_bytes(bytes(image))

    verify_common(out, stage1, stage2)
    data = out.read_bytes()
    check(data[PTBL_LBA * SECTOR_SIZE:PTBL_LBA * SECTOR_SIZE + 8] == PTBL_MAGIC,
          f"payload table magic at LBA {PTBL_LBA}", "")
    check(data[klba * SECTOR_SIZE + 0x1FE:klba * SECTOR_SIZE + 0x200] == b"\x55\xAA",
          "kernel image has 0xAA55 at its offset 0x1FE", "")
    check(data[klba * SECTOR_SIZE + 0x202:klba * SECTOR_SIZE + 0x206] == b"HdrS",
          "kernel image has 'HdrS' magic at its offset 0x202", "")
    check(len(data) % SECTOR_SIZE == 0, "image size is a whole number of sectors", "")
    finish()

    print()
    print(f"[BUILD] wrote {out}  ({len(image):,} bytes = {total_sectors} sectors)")
    print(f"[BUILD]   LBA {0:<6} stage1 (MBR + partition table)")
    print(f"[BUILD]   LBA {1:<6} stage2_linux ({len(stage2)} bytes)")
    print(f"[BUILD]   LBA {PTBL_LBA:<6} payload table")
    print(f"[BUILD]   LBA {klba:<6} bzImage      ({len(kdata):,} bytes, {ksect} sectors)")
    if isect:
        print(f"[BUILD]   LBA {ilba:<6} initramfs    ({len(idata):,} bytes, {isect} sectors)")
    print(f"[BUILD]   cmdline: {cmdline!r}")

    if dump:
        print()
        hexdump(ptbl[:64], f"payload table (LBA {PTBL_LBA})", 0)
        print()
        hexdump(kdata[0x1F0:0x270], "bzImage setup header", 0x1F0)
    return out


# --- 検証 ------------------------------------------------------------------
_all_ok = True


def check(passed: bool, name: str, extra: str) -> None:
    global _all_ok
    mark = "OK  " if passed else "FAIL"
    detail = f"  ({extra})" if extra else ""
    print(f"[VERIFY] {mark} {name}{detail}")
    _all_ok = _all_ok and passed


def finish() -> None:
    if not _all_ok:
        die("イメージの検証に失敗しました")


def verify_common(path: Path, stage1: bytes, stage2: bytes) -> None:
    """資料 3章ステップ3 の必須検証。"""
    data = path.read_bytes()
    check(data[0x1FE:0x200] == b"\x55\xAA",
          "boot signature 0x1FE-0x1FF == 55 AA", data[0x1FE:0x200].hex().upper())
    check(data[:SECTOR_SIZE] == stage1, "stage1 occupies sector 1", "")
    check(data[SECTOR_SIZE:SECTOR_SIZE + len(stage2)] == stage2,
          "stage2 placed at offset 0x200", "")


def hexdump(data: bytes, title: str, base: int) -> None:
    print(f"--- {title} ---")
    prev = None
    skipping = False
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        if chunk == prev and set(chunk) == {0}:
            if not skipping:
                print("*")
                skipping = True
            continue
        skipping = False
        prev = chunk
        hexpart = " ".join(f"{b:02X}" for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"{base + i:04X}: {hexpart:<47}  {asciipart}")


def main() -> None:
    ap = argparse.ArgumentParser(description="myOS のディスクイメージをビルドする")
    ap.add_argument("--stage2", choices=sorted(STAGE2_VARIANTS), default="main",
                    help="フロッピーイメージに載せる Stage2 (default: main)")
    ap.add_argument("--kernel", help="bzImage のパス。指定すると HDD イメージを作る")
    ap.add_argument("--initrd", help="initramfs (cpio) のパス")
    ap.add_argument("--cmdline", default="console=ttyS0,115200 console=tty0",
                    help="カーネルコマンドライン")
    ap.add_argument("--dump", action="store_true", help="HEX ダンプも表示する")
    args = ap.parse_args()

    if args.kernel:
        kernel = Path(args.kernel)
        if not kernel.exists():
            die(f"カーネルがありません: {kernel}")
        initrd = Path(args.initrd) if args.initrd else None
        if initrd is not None and not initrd.exists():
            die(f"initramfs がありません: {initrd}")
        build_hdd(kernel, initrd, args.cmdline, args.dump)
    else:
        build_floppy(args.stage2, args.dump)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# ============================================================================
# make_fake_kernel.py  -  ローダー検証用の「偽 bzImage」を作る
#
# src/test/fake_kernel.asm (32bit の生バイナリ) の前に、bzImage 互換の
# setup ヘッダを付けて 1 ファイルにする。ローダーから見ると本物と同じに見える。
#
# 本物の bzImage が無くても stage2_linux.asm を検証できるようにするのが目的。
# 失敗したときに「ローダーが悪いのか、カーネルの都合なのか」を切り分けられる。
#
# 使い方:
#   python3 tools/make_fake_kernel.py
#   → build/fake_bzImage
# ============================================================================
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"

SETUP_SECTS = 4                     # setup 部分のセクタ数
HDR_END = 0x268                     # setup ヘッダの終端
CODE32_START = 0x00100000


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)

    src = ROOT / "src" / "test" / "fake_kernel.asm"
    body_path = BUILD / "fake_kernel_body.bin"
    cmd = ["nasm", "-f", "bin", str(src), "-o", str(body_path),
           "-l", str(BUILD / "fake_kernel.lst")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr, file=sys.stderr)
        return 1
    body = body_path.read_bytes()

    # --- setup 部分 (ヘッダ込みで (SETUP_SECTS + 1) * 512 バイト) ---
    setup = bytearray((SETUP_SECTS + 1) * 512)

    setup[0x1F1] = SETUP_SECTS
    struct.pack_into("<H", setup, 0x1FE, 0xAA55)        # boot_flag

    # 0x200 の 'EB xx' の xx が「ヘッダ終端 - 0x202」を表す。
    # ローダーはここからヘッダ長を求めるので、正しく埋めておく。
    setup[0x200] = 0xEB
    setup[0x201] = HDR_END - 0x202

    setup[0x202:0x206] = b"HdrS"
    struct.pack_into("<H", setup, 0x206, 0x020F)        # Boot Protocol 2.15
    setup[0x210] = 0x00                                 # type_of_loader
    setup[0x211] = 0x01                                 # loadflags: LOADED_HIGH
    struct.pack_into("<I", setup, 0x214, CODE32_START)  # code32_start
    struct.pack_into("<I", setup, 0x22C, 0x7FFFFFFF)    # initrd_addr_max
    struct.pack_into("<I", setup, 0x230, 0x00200000)    # kernel_alignment
    setup[0x234] = 1                                    # relocatable_kernel
    struct.pack_into("<I", setup, 0x238, 2047)          # cmdline_size

    # syssize は本体サイズ / 16 (paragraph 単位)
    struct.pack_into("<I", setup, 0x1F4, (len(body) + 15) // 16)

    out = BUILD / "fake_bzImage"
    out.write_bytes(bytes(setup) + body)

    print(f"[FAKE] fake_kernel body : {len(body):,} bytes")
    print(f"[FAKE] setup part       : {len(setup):,} bytes "
          f"(setup_sects = {SETUP_SECTS})")
    print(f"[FAKE] wrote {out} ({out.stat().st_size:,} bytes)")
    print()
    print("次はこれをイメージに載せて起動する:")
    print(f"  python3 tools/build_image.py --kernel {out} \\")
    print("      --initrd build/initramfs.cpio.gz --cmdline 'myos test cmdline'")
    print("  python3 tools/run_qemu.py --image build/disk_hdd.img --media hdd")
    return 0


if __name__ == "__main__":
    sys.exit(main())

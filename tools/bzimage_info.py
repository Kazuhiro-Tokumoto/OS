#!/usr/bin/env python3
# ============================================================================
# bzimage_info.py  -  Linux bzImage の setup ヘッダを解析して表示する
#
# フェーズ2-B (Linux Boot Protocol 対応) の準備ツール。
# 引き継ぎ資料 8章-5「bzImage のヘッダ部分を 16進ダンプで確認しながら進める」
# に対応する。
#
# 参照: Linux カーネルソース Documentation/arch/x86/boot.rst
#       (古いカーネルでは Documentation/x86/boot.txt)
#
# 使い方:
#   python3 tools/bzimage_info.py /path/to/bzImage
#   python3 tools/bzimage_info.py /path/to/bzImage --hexdump
#
# 何を見ればよいか:
#   - offset 0x1F1 setup_sects : setup コードのセクタ数。
#       0 のときは 4 として扱う (古い規約)。
#       プロテクトモード用カーネル本体は
#         ファイル先頭から (setup_sects + 1) * 512 バイト目から始まる。
#   - offset 0x202 "HdrS"      : ヘッダのマジック。これが無ければ
#                                Boot Protocol 非対応の古い形式。
#   - offset 0x206 version     : Boot Protocol のバージョン (0x020C など)
#   - offset 0x211 loadflags   : bit0 LOADED_HIGH (1 なら本体を 0x100000 へ)
#   - offset 0x210 type_of_loader : 自作ローダーなら 0xFF を書く
#   - offset 0x214 code32_start : 32bit エントリポイント (通常 0x100000)
# ============================================================================
import argparse
import sys
from pathlib import Path

# (オフセット, サイズ, 名前, 説明)
HEADER_FIELDS = [
    (0x1F1, 1, "setup_sects",        "setup のセクタ数 (0 は 4 とみなす)"),
    (0x1F2, 2, "root_flags",         "ルートを読み取り専用でマウントするか"),
    (0x1F4, 4, "syssize",            "本体サイズ / 16 (paragraph 単位)"),
    (0x1F8, 2, "ram_size",           "廃止済みフィールド"),
    (0x1FA, 2, "vid_mode",           "ビデオモード指定"),
    (0x1FC, 2, "root_dev",           "廃止済み: ルートデバイス番号"),
    (0x1FE, 2, "boot_flag",          "0xAA55 でなければならない"),
    (0x200, 2, "jump",               "短いジャンプ命令 (0x??EB)"),
    (0x202, 4, "header",             "マジック 'HdrS' (0x53726448)"),
    (0x206, 2, "version",            "Boot Protocol バージョン"),
    (0x208, 4, "realmode_swtch",     "リアルモード切替フック"),
    (0x20C, 2, "start_sys_seg",      "廃止済み"),
    (0x20E, 2, "kernel_version",     "カーネル版文字列へのオフセット(+0x200)"),
    (0x210, 1, "type_of_loader",     "ブートローダー識別子 (自作は 0xFF)"),
    (0x211, 1, "loadflags",          "bit0=LOADED_HIGH bit5=QUIET bit7=HEAP_USED"),
    (0x212, 2, "setup_move_size",    "廃止済み"),
    (0x214, 4, "code32_start",       "32bit エントリポイント (通常 0x100000)"),
    (0x218, 4, "ramdisk_image",      "initramfs のロード先物理アドレス"),
    (0x21C, 4, "ramdisk_size",       "initramfs のサイズ"),
    (0x220, 4, "bootsect_kludge",    "廃止済み"),
    (0x224, 2, "heap_end_ptr",       "setup のヒープ終端"),
    (0x226, 1, "ext_loader_ver",     "ローダーのバージョン拡張"),
    (0x227, 1, "ext_loader_type",    "ローダー種別の拡張"),
    (0x228, 4, "cmd_line_ptr",       "カーネルコマンドラインの物理アドレス"),
    (0x22C, 4, "initrd_addr_max",    "initrd を置ける最大アドレス"),
    (0x230, 4, "kernel_alignment",   "本体の要求アラインメント"),
    (0x234, 1, "relocatable_kernel", "1 なら任意アドレスに置ける"),
    (0x235, 1, "min_alignment",      "log2 の最小アラインメント"),
    (0x236, 2, "xloadflags",         "64bit/EFI 関連のフラグ"),
    (0x238, 4, "cmdline_size",       "コマンドラインの最大長"),
    (0x23C, 4, "hardware_subarch",   "サブアーキテクチャ"),
    (0x240, 8, "hardware_subarch_data", "サブアーキ固有データ"),
    (0x248, 4, "payload_offset",     "圧縮ペイロードのオフセット"),
    (0x24C, 4, "payload_length",     "圧縮ペイロードの長さ"),
    (0x250, 8, "setup_data",         "setup_data チェインの先頭"),
    (0x258, 8, "pref_address",       "推奨ロードアドレス"),
    (0x260, 4, "init_size",          "展開に必要な総メモリ量"),
    (0x264, 4, "handover_offset",    "EFI ハンドオーバのオフセット"),
]

LOADFLAG_BITS = [
    (0, "LOADED_HIGH", "本体を 0x100000 (1MB) に置く"),
    (1, "KASLR_FLAG", "KASLR 有効"),
    (5, "QUIET_FLAG", "起動メッセージを抑制"),
    (6, "KEEP_SEGMENTS", "セグメント再設定を省略 (廃止)"),
    (7, "CAN_USE_HEAP", "heap_end_ptr が有効"),
]


def read_field(data: bytes, off: int, size: int) -> int:
    chunk = data[off:off + size]
    if len(chunk) < size:
        return 0
    return int.from_bytes(chunk, "little")


def hexdump(data: bytes, base: int, length: int) -> None:
    for i in range(0, length, 16):
        chunk = data[base + i:base + i + 16]
        if not chunk:
            break
        hexpart = " ".join(f"{b:02X}" for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"{base + i:04X}: {hexpart:<47}  {asciipart}")


def main() -> int:
    ap = argparse.ArgumentParser(description="bzImage の setup ヘッダを解析する")
    ap.add_argument("bzimage")
    ap.add_argument("--hexdump", action="store_true",
                    help="0x1F0-0x270 の生ダンプも表示する")
    args = ap.parse_args()

    path = Path(args.bzimage)
    if not path.exists():
        print(f"ERROR: {path} がありません", file=sys.stderr)
        return 1

    data = path.read_bytes()
    print(f"file      : {path}")
    print(f"file size : {len(data):,} bytes")
    print()

    if read_field(data, 0x1FE, 2) != 0xAA55:
        print("警告: offset 0x1FE が 0xAA55 ではありません。"
              "bzImage ではない可能性があります。")

    magic = data[0x202:0x206]
    if magic != b"HdrS":
        print(f"警告: offset 0x202 のマジックが {magic!r} で 'HdrS' ではありません。")
        print("      Linux Boot Protocol 非対応の古い形式かもしれません。")
        print()

    version = read_field(data, 0x206, 2)

    print("--- setup ヘッダ ---")
    for off, size, name, desc in HEADER_FIELDS:
        if off + size > len(data):
            continue
        # ヘッダは protocol version ごとに伸びてきたので、
        # 実際のヘッダ末尾 (0x201 + ヘッダ内 0x0201 のサイズ) を超えたら打ち切る
        value = read_field(data, off, size)
        width = size * 2
        print(f"  0x{off:03X}  {name:<22} = 0x{value:0{width}X}  ({value})")
        print(f"         {'':<22}   {desc}")

    print()
    print("--- 解析結果 ---")
    setup_sects = read_field(data, 0x1F1, 1) or 4
    kernel_offset = (setup_sects + 1) * 512
    syssize = read_field(data, 0x1F4, 4)
    loadflags = read_field(data, 0x211, 1)
    code32_start = read_field(data, 0x214, 4)

    print(f"  Boot Protocol version : {version >> 8}.{version & 0xFF:02d} "
          f"(0x{version:04X})")
    print(f"  setup_sects           : {setup_sects}")
    print("  → プロテクトモード用カーネル本体のファイル内オフセット")
    print(f"      = (setup_sects + 1) * 512 = {kernel_offset} "
          f"(0x{kernel_offset:X})")
    print(f"  → 本体のサイズ (syssize * 16) = {syssize * 16:,} bytes")
    print(f"  → ファイル実サイズとの差分     = "
          f"{len(data) - kernel_offset:,} bytes")
    print(f"  32bit エントリポイント : 0x{code32_start:08X}")
    print()
    print(f"  loadflags = 0x{loadflags:02X}")
    for bit, name, desc in LOADFLAG_BITS:
        mark = "x" if loadflags & (1 << bit) else " "
        print(f"    [{mark}] bit{bit} {name:<14} {desc}")

    print()
    print("--- 自作ブートローダー側でやること ---")
    print(f"  1. ファイル先頭 {kernel_offset} バイト以降 "
          f"({syssize * 16:,} バイト) を物理 0x{code32_start:08X} へ読み込む")
    print("  2. boot_params (ゼロページ) を作る:")
    print("       - bzImage の 0x01F1〜0x0268 をゼロページの同じオフセットへコピー")
    print("       - type_of_loader (0x210) に 0xFF を書く")
    print("       - loadflags (0x211) の bit0 (LOADED_HIGH) を立てる")
    print("       - cmd_line_ptr (0x228) にコマンドライン文字列の物理アドレス")
    print("       - INT 15h E820h のメモリマップをゼロページ 0x2D0 以降へ、")
    print("         エントリ数を 0x1E8 (e820_entries) へ")
    print("       - initramfs を使うなら ramdisk_image(0x218)/ramdisk_size(0x21C)")
    print(f"  3. レジスタを整えて 0x{code32_start:08X} へジャンプ")
    print("       - ESI = ゼロページの物理アドレス")
    print("       - EBP / EDI / EBX = 0")
    print("       - CS = __BOOT_CS (セレクタ 0x10), DS/ES/SS = __BOOT_DS (0x18)")
    print("       - プロテクトモード / ページング無効 / 割り込み禁止")
    print("     ※ セレクタ番号が 0x10 / 0x18 で固定されている点に注意。")
    print("        教科書的な 0x08 / 0x10 の配置ではカーネルに渡せない。")

    if args.hexdump:
        print()
        print("--- 生ダンプ 0x1F0 - 0x270 ---")
        hexdump(data, 0x1F0, 0x80)

    return 0


if __name__ == "__main__":
    sys.exit(main())

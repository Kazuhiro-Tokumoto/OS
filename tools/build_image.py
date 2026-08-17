#!/usr/bin/env python3
# ============================================================================
# build_image.py  -  myOS フロッピーイメージ (disk.img) ビルドスクリプト
#
# 引き継ぎ資料 3章の Python 手順を 1 本にまとめたもの。
#
#   1. src/boot/*.asm を nasm でアセンブル
#   2. Stage1 を 512 バイトに整形 (0 埋め + 55 AA ブートシグネチャ)
#   3. Stage1 + Stage2 を結合し 1,474,560 バイト (1.44MB FD) に 0 埋め
#   4. 検証 (サイズ / シグネチャ / Stage2 配置 / Stage1 のはみ出しチェック)
#
# 使い方:
#   python3 tools/build_image.py                  … 通常の Stage2 でビルド
#   python3 tools/build_image.py --stage2 v2      … 引き継ぎ資料の Stage2 v2
#                                                    (色付き HELLO, WORLD!)
#   python3 tools/build_image.py --dump           … 先頭のHEXダンプも表示
# ============================================================================
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "boot"
BUILD = ROOT / "build"

FLOPPY_SIZE = 1474560   # 1.44MB フロッピーの標準サイズ
SECTOR_SIZE = 512
# Stage1 が読み込むセクタ数。src/boot/stage1.asm の STAGE2_SECTORS と必ず一致
# させること (ここを増やすなら向こうも直す)。
STAGE2_MAX_SECTORS = 17

STAGE2_VARIANTS = {
    "main": "stage2.asm",              # フェーズ1 + フェーズ2-A の本体
    "v2":   "stage2_v2_color.asm",     # 引き継ぎ資料 2-3節 の 47 バイト版
}


def die(msg: str) -> "None":
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

    cmd = ["nasm", "-f", "bin", str(src), "-o", str(out), "-l", str(lst)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        die(f"nasm がアセンブルに失敗しました: {asm_name}")

    data = out.read_bytes()
    print(f"[BUILD] assembled {asm_name:<24} -> {out.name:<24} {len(data):5d} bytes"
          f"  (listing: build/{lst.name})")
    return data


def make_stage1(code: bytes) -> bytes:
    """Stage1 を 512 バイトのブートセクタに整形する。

    nasm は `times 510-($-$$) db 0` を正しく処理できるので本来ここでの
    パディングは不要だが、資料 5章(1) の教訓どおり「最終的な 512 バイトは
    ビルドスクリプト側でも必ず検証する」方針を守っている。
    """
    if len(code) > SECTOR_SIZE:
        die(f"Stage1 が {len(code)} バイトあり 512 バイトに収まりません")

    if len(code) == SECTOR_SIZE:
        # nasm 側で既にパディング + シグネチャ済み
        if code[510:512] != b"\x55\xAA":
            die("Stage1 の末尾が 55 AA になっていません")
        return code

    # 念のため: nasm 側でパディングしていない場合はここで付ける
    if len(code) > 510:
        die("Stage1 のコードが 510 バイトを超えています (シグネチャが入りません)")
    return code + bytes(510 - len(code)) + b"\x55\xAA"


def build(stage2_variant: str, dump: bool) -> Path:
    stage1_code = assemble("stage1.asm")
    stage2 = assemble(STAGE2_VARIANTS[stage2_variant])

    stage1 = make_stage1(stage1_code)

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

    verify(out, stage1, stage2)
    print(f"[BUILD] wrote {out}  ({len(image)} bytes, stage2 variant = {stage2_variant})")

    if dump:
        print()
        hexdump(bytes(image[:SECTOR_SIZE]), "Stage1 (sector 1 / offset 0x0000)", 0)
        print()
        n = min(len(stage2), 256)
        hexdump(bytes(image[SECTOR_SIZE:SECTOR_SIZE + n]),
                "Stage2 (sector 2 / offset 0x0200)", SECTOR_SIZE)

    return out


def verify(path: Path, stage1: bytes, stage2: bytes) -> None:
    """資料 3章ステップ3 の必須検証。"""
    data = path.read_bytes()
    checks = []

    checks.append(("image size == 1,474,560", len(data) == FLOPPY_SIZE, f"{len(data)}"))
    checks.append(("boot signature 0x1FE-0x1FF == 55 AA",
                   data[0x1FE:0x200] == b"\x55\xAA",
                   data[0x1FE:0x200].hex().upper()))
    checks.append(("stage1 occupies sector 1",
                   data[:SECTOR_SIZE] == stage1, ""))
    checks.append(("stage2 placed at offset 0x200",
                   data[SECTOR_SIZE:SECTOR_SIZE + len(stage2)] == stage2, ""))
    checks.append(("tail after stage2 is zero-filled",
                   set(data[SECTOR_SIZE + len(stage2):]) <= {0}, ""))

    ok = True
    for name, passed, extra in checks:
        mark = "OK  " if passed else "FAIL"
        detail = f"  ({extra})" if extra else ""
        print(f"[VERIFY] {mark} {name}{detail}")
        ok = ok and passed
    if not ok:
        die("イメージの検証に失敗しました")


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
    ap = argparse.ArgumentParser(description="myOS フロッピーイメージをビルドする")
    ap.add_argument("--stage2", choices=sorted(STAGE2_VARIANTS), default="main",
                    help="使用する Stage2 (default: main)")
    ap.add_argument("--dump", action="store_true", help="HEX ダンプも表示する")
    args = ap.parse_args()
    build(args.stage2, args.dump)


if __name__ == "__main__":
    main()

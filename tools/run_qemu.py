#!/usr/bin/env python3
# ============================================================================
# run_qemu.py  -  disk.img を QEMU で起動して結果を自動検証するツール
#
# VirtualBox は GUI が要る & 画面を目視するしかないため、CI や
# ヘッドレス環境では検証しづらい。このツールは QEMU のモニタ機能を使って
#
#   - テキスト VRAM (物理 0xB8000 から 4000 バイト) をそのまま吸い出し、
#     「画面に何が出ているか」を文字コード + 属性バイトのレベルで確認する
#   - 画面のスクリーンショット (PPM → PNG) を撮る
#   - キーボード入力を送り込む (sendkey)
#
# ことで、機械語レベルでの検証を自動化する。
# VirtualBox での最終確認を置き換えるものではなく、その前段のふるいとして使う。
#
# 使い方:
#   python3 tools/run_qemu.py                       … 2秒起動して画面を取得
#   python3 tools/run_qemu.py --keys "hi,esc"       … キー入力を送ってから取得
#   python3 tools/run_qemu.py --wait 4 --png out.png
# ============================================================================
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"

VRAM_ADDR = 0xB8000
VRAM_LEN = 80 * 25 * 2

# CGA/VGA の 16 色パレット (R, G, B)
CGA_PALETTE = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0xAA), (0x00, 0xAA, 0x00), (0x00, 0xAA, 0xAA),
    (0xAA, 0x00, 0x00), (0xAA, 0x00, 0xAA), (0xAA, 0x55, 0x00), (0xAA, 0xAA, 0xAA),
    (0x55, 0x55, 0x55), (0x55, 0x55, 0xFF), (0x55, 0xFF, 0x55), (0x55, 0xFF, 0xFF),
    (0xFF, 0x55, 0x55), (0xFF, 0x55, 0xFF), (0xFF, 0xFF, 0x55), (0xFF, 0xFF, 0xFF),
]
COLOR_NAMES = [
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "lightgray",
    "darkgray", "lightblue", "lightgreen", "lightcyan", "lightred",
    "lightmagenta", "yellow", "white",
]


ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class QemuMonitor:
    """QEMU の human monitor に UNIX ソケット越しに喋るだけの薄いラッパ。

    注意: モニタはコマンドを 1 文字ずつエコーバックし、ANSI エスケープも
    混ぜてくるので、応答を読むときは必ずエスケープを剥がすこと。
    """

    def __init__(self, path: str):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + 10
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("QEMU モニタに接続できませんでした")
                time.sleep(0.1)
        self.sock.settimeout(10)
        self._read_until_prompt()

    def _read_until_prompt(self) -> str:
        buf = b""
        deadline = time.time() + 10
        while b"(qemu)" not in buf:
            if time.time() > deadline:
                break
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
        return ANSI_RE.sub("", buf.decode("utf-8", "replace"))

    def cmd(self, line: str) -> str:
        self.sock.sendall((line + "\n").encode())
        out = self._read_until_prompt()
        # エコーバック部分を落とし、実際の応答だけを見られるようにする
        idx = out.rfind(line)
        if idx >= 0:
            out = out[idx + len(line):]
        return out.strip()

    def close(self) -> None:
        try:
            self.sock.sendall(b"quit\n")
        except OSError:
            pass
        self.sock.close()


def decode_vram(raw: bytes):
    """テキスト VRAM を (文字, 属性) の行リストに変換する。"""
    rows = []
    for r in range(25):
        cells = []
        for c in range(80):
            off = (r * 80 + c) * 2
            ch = raw[off]
            attr = raw[off + 1]
            cells.append((ch, attr))
        rows.append(cells)
    return rows


def cp437_char(code: int) -> str:
    if code in (0x00, 0x20):
        return " "
    if 32 <= code < 127:
        return chr(code)
    try:
        return bytes([code]).decode("cp437")
    except UnicodeDecodeError:
        return "."


def render_text(rows) -> str:
    out = []
    for cells in rows:
        line = "".join(cp437_char(ch) for ch, _ in cells).rstrip()
        out.append(line)
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out)


def attribute_report(rows) -> str:
    """行ごとに「実際に使われている属性バイト」を集計する。
    色が意図どおり付いているかを目視ではなく数値で確認するため。"""
    lines = []
    for i, cells in enumerate(rows):
        used = {}
        for ch, attr in cells:
            if ch in (0x00, 0x20) and (attr >> 4) == 0:
                continue    # 黒背景の空白は「未使用」として無視
            used[attr] = used.get(attr, 0) + 1
        if not used:
            continue
        parts = []
        for attr, count in sorted(used.items(), key=lambda kv: -kv[1]):
            bg = COLOR_NAMES[(attr >> 4) & 0x0F]
            fg = COLOR_NAMES[attr & 0x0F]
            parts.append(f"0x{attr:02X}(bg={bg},fg={fg})x{count}")
        lines.append(f"  row {i:2d}: " + "  ".join(parts))
    return "\n".join(lines)


def ppm_to_png(ppm_path: Path, png_path: Path) -> bool:
    """QEMU の screendump が吐く P6 PPM を、外部ライブラリ無しで PNG に変換。"""
    data = ppm_path.read_bytes()
    if not data.startswith(b"P6"):
        return False

    # ヘッダ (P6, width, height, maxval) をコメントを飛ばしつつ読む
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    width, height, _maxval = fields
    pixels = data[pos:pos + width * height * 3]

    raw = bytearray()
    for y in range(height):
        raw.append(0)   # PNG のフィルタタイプ: None
        raw += pixels[y * width * 3:(y + 1) * width * 3]

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    png_path.write_bytes(png)
    return True


def parse_keys(spec: str):
    """"hi,esc" のような指定を QEMU の sendkey 引数列に変換する。"""
    special = {
        "esc": "esc", "ret": "ret", "enter": "ret", "spc": "spc",
        "space": "spc", "bs": "backspace", "backspace": "backspace",
        "tab": "tab",
    }
    keys = []
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        low = token.lower()
        if low in special:
            keys.append(special[low])
            continue
        for ch in token:
            if ch == " ":
                keys.append("spc")
            elif ch.isalnum():
                keys.append(ch.lower())
            else:
                keys.append(ch)
    return keys


def main() -> int:
    ap = argparse.ArgumentParser(description="disk.img を QEMU で起動し画面を検証する")
    ap.add_argument("--image", default=str(BUILD / "disk.img"))
    ap.add_argument("--wait", type=float, default=2.0,
                    help="キー送信前の待ち時間(秒)")
    ap.add_argument("--keys", default="",
                    help='送信するキー。例: "hello,ret,esc"')
    ap.add_argument("--post-wait", type=float, default=1.5,
                    help="キー送信後、画面取得までの待ち時間(秒)")
    ap.add_argument("--png", default=str(BUILD / "screen.png"),
                    help="スクリーンショットの出力先 PNG")
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--media", choices=["floppy", "hdd"], default="floppy",
                    help="イメージをフロッピーとして繋ぐか、ハードディスクとして繋ぐか")
    ap.add_argument("--mem", default="128", help="QEMU に渡すメモリ量 (MB)")
    ap.add_argument("--serial", default="",
                    help="シリアル出力の保存先。Linux の起動ログを取るのに使う")
    ap.add_argument("--mouse", default="",
                    help='マウス操作。; 区切りで並べる。"dx:dy" で相対移動、'
                         '"click" で左クリック、"wait" で少し待つ。'
                         '例: "-475:369;click"  (スタートボタンを押す)')
    args = ap.parse_args()

    image = Path(args.image)
    if not image.exists():
        print(f"ERROR: {image} がありません。先に build_image.py を実行してください。",
              file=sys.stderr)
        return 1

    tmpdir = Path(tempfile.mkdtemp(prefix="myos-qemu-"))
    sock_path = tmpdir / "monitor.sock"
    vram_path = tmpdir / "vram.bin"
    ppm_path = tmpdir / "screen.ppm"
    # 重要: pmemsave / screendump のファイル名に絶対パスを渡してはいけない。
    # モニタの引数は式として評価されるため、先頭の '/' が除算演算子と解釈され
    # "invalid char 't' in expression" のようなエラーになる。
    # QEMU の作業ディレクトリを tmpdir にして、相対ファイル名で渡す。
    image = image.resolve()

    if args.media == "floppy":
        drive = f"file={image},format=raw,if=floppy,index=0"
        boot = "order=a"
    else:
        drive = f"file={image},format=raw,if=ide,index=0,media=disk"
        boot = "order=c"

    cmd = [
        args.qemu,
        "-drive", drive,
        "-boot", boot,
        "-m", str(args.mem),
        "-display", "none",
        "-no-reboot",
        "-monitor", f"unix:{sock_path},server,nowait",
    ]
    serial_path = None
    if args.serial:
        serial_path = Path(args.serial).resolve()
        serial_path.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["-serial", f"file:{serial_path}"]
    print("[QEMU] " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            cwd=str(tmpdir))

    try:
        mon = QemuMonitor(str(sock_path))

        time.sleep(args.wait)

        if args.keys:
            keys = parse_keys(args.keys)
            print(f"[QEMU] sendkey: {keys}")
            for k in keys:
                mon.cmd(f"sendkey {k}")
                time.sleep(0.06)
            time.sleep(args.post_wait)

        if args.mouse:
            for step in args.mouse.split(";"):
                step = step.strip()
                if not step:
                    continue
                low = step.lower()
                if low == "click":
                    mon.cmd("mouse_button 1")
                    time.sleep(0.15)
                    mon.cmd("mouse_button 0")
                elif low == "down":
                    mon.cmd("mouse_button 1")
                elif low == "up":
                    mon.cmd("mouse_button 0")
                elif low == "wait":
                    time.sleep(0.5)
                else:
                    dx, _, dy = step.partition(":")
                    mon.cmd(f"mouse_move {int(dx)} {int(dy)}")
                time.sleep(0.15)
            time.sleep(args.post_wait)

        # --- テキスト VRAM をそのまま吸い出す (相対ファイル名で渡すこと) ---
        out = mon.cmd(f"pmemsave 0x{VRAM_ADDR:X} {VRAM_LEN} {vram_path.name}")
        if "rror" in out or "invalid" in out:
            print(f"[QEMU] pmemsave 失敗: {out}", file=sys.stderr)
        # --- スクリーンショット ---
        mon.cmd(f"screendump {ppm_path.name}")

        # CPU の状態も見ておく (プロテクトモードに入れたかの確認に使う)
        cpu_info = mon.cmd("info registers")

        mon.close()
    finally:
        time.sleep(0.3)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    if not vram_path.exists():
        print("ERROR: VRAM を取得できませんでした", file=sys.stderr)
        err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
        if err:
            print(err, file=sys.stderr)
        return 1

    raw = vram_path.read_bytes()
    rows = decode_vram(raw)

    print()
    print("=" * 78)
    print(" 画面テキスト (物理 0xB8000 のテキスト VRAM を直接デコード)")
    print("=" * 78)
    print(render_text(rows))
    print("=" * 78)
    print(" 属性バイト (色) の集計")
    print("=" * 78)
    print(attribute_report(rows))

    m = re.search(r"^CR0=([0-9a-fA-F]+)", cpu_info, re.MULTILINE)
    if m:
        cr0 = int(m.group(1), 16)
        print("=" * 78)
        print(f" CR0 = 0x{cr0:08X}   PE(bit0) = {cr0 & 1}"
              f"  -> {'プロテクトモード' if cr0 & 1 else 'リアルモード'}")

    if ppm_path.exists():
        png = Path(args.png)
        png.parent.mkdir(parents=True, exist_ok=True)
        if ppm_to_png(ppm_path, png):
            print("=" * 78)
            print(f" スクリーンショット: {png}")

    if serial_path is not None and serial_path.exists():
        text = serial_path.read_text("utf-8", "replace")
        print("=" * 78)
        print(f" シリアル出力 ({serial_path}, {len(text)} bytes)")
        print("=" * 78)
        print(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())

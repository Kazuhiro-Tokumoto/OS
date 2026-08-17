# myOS

Windows 98 もどきの見た目・操作感を目指した自作 OS。

自作するのは **ブートローダー** と **GUI 層** の 2 つに絞り、
カーネルは Linux カーネルをそのまま採用する（`.ko` は全て `=y` にした
モノリシックな `bzImage` 1 個で完結させる）。
自作ブートローダーから Linux Boot Protocol の 32bit Entry で直接起動することを目指す。
ユーザーランドも動的リンカーや実行ファイル形式は自作せず、
標準 ELF と既存の glibc / musl を流用する。

## 対象環境

| 項目 | 方針 |
| --- | --- |
| CPU | Core 2 Duo 世代（2006年〜）以降。それ以降の新しい CPU でも動作必須 |
| ファームウェア | レガシー BIOS 前提（UEFI は対象外） |
| BIOS 拡張機能 | 使う場合は必ず対応チェック → 非対応時は枯れた方式へフォールバック |
| Long Mode | ブートローダー側では対応不要（Linux カーネル側が担う） |

## ディレクトリ構成

```
src/boot/     ブートローダーのソース (nasm)
  stage1.asm            Stage1 / MBR (512 バイト)
  stage2.asm            Stage2 本体 (フェーズ1 + フェーズ2-A)
  stage2_v2_color.asm   引き継ぎ資料 2-3節 の色付き HELLO, WORLD! (47 バイト)
tools/        ビルド・検証スクリプト (Python)
  build_image.py        disk.img のビルドと検証
  run_qemu.py           QEMU で起動し、VRAM を吸い出して自動検証
  bzimage_info.py       bzImage の setup ヘッダ解析 (フェーズ2-B 準備)
docs/         設計メモ・進捗
build/        生成物 (git 管理外)
```

## ビルドと実行

```bash
# 必要なもの
apt-get install -y nasm qemu-system-x86   # python3 は標準

make            # build/disk.img を作る
make run        # QEMU で起動して画面を自動検証
make run-keys   # キー入力を送ってから検証
make v2         # 引き継ぎ資料の Stage2 v2 (色付き HELLO, WORLD!) でビルド
make disasm     # 逆アセンブルして機械語を確認
```

VirtualBox で確認する場合は `build/disk.img` をフロッピーとして割り当て、
起動順序でフロッピーを最上位にする。詳しくは `docs/testing.md`。

## 検証方法について

このプロジェクトでは「画面を目視する」代わりに、
**テキスト VRAM (物理 0xB8000) を QEMU から直接吸い出して、
文字コードと属性バイトのレベルで検証する** 方式を取っている
(`tools/run_qemu.py`)。色が意図どおりに付いているかも
属性バイトの集計で数値として確認できる。

```
$ make run-keys
==============================================================================
 画面テキスト (物理 0xB8000 のテキスト VRAM を直接デコード)
==============================================================================
 myOS Stage2  -  Phase1: screen/keyboard   Phase2-A: protected mode

Boot drive (from BIOS DL) : 0x00
Memory map                : INT 15h E820h / usable 127 MB
...
==============================================================================
 CR0 = 0x00000011   PE(bit0) = 1  -> プロテクトモード
```

## 進捗

| フェーズ | 内容 | 状態 |
| --- | --- | --- |
| 0 | ブートローダー (Stage1 + Stage2 二段構成) | 完了 |
| 1 | 画面クリア / カーソル制御 / 色 / キーボード入力 | 完了 |
| 2-A | A20 ゲート / GDT / プロテクトモード移行 | 完了 |
| 2-B | Linux Boot Protocol 対応 | 未着手（`bzImage` のビルドが前提） |
| 3〜 | 以降は Linux カーネルに任せる範囲が多い。GUI 層 (5, 6) が自作の主戦場 | 未着手 |

詳しくは `docs/progress.md` を参照。

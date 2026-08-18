# myOS

Windows 98 もどきの見た目・操作感を目指した自作 OS。

自作するのは **ブートローダー** と **GUI 層** の 2 つに絞り、
カーネルは Linux カーネルをそのまま採用する。
`.ko` は全て `=y` にしたモノリシックな `bzImage` 1 個で完結させてあり、
初回起動時にドライバを入れる手間は無い（`=m` は 0 個）。
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
  stage2.asm            フェーズ1 + フェーズ2-A のデモ
  stage2_linux.asm      フェーズ2-B: Linux Boot Protocol ローダー
  stage2_v2_color.asm   引き継ぎ資料 2-3節 の色付き HELLO, WORLD! (47 バイト)
  inc/                  共通ルーチン
    video.inc           テキスト画面出力 (VRAM 直書き + カーソル制御)
    a20.inc             A20 ゲート (3方式フォールバック + 実地検証)
    memmap.inc          メモリマップ取得 (E820h → E801h → 88h)
    gdt.inc             GDT (Linux の __BOOT_CS/__BOOT_DS 配置)
    disk.inc            ディスク読み込み (LBA/CHS) とアンリアルモード
src/gui/      Win98 もどきの GUI
  x98.h                 描画部品 (立体枠 / タイトルバー / アイコン / 入力欄)
  myos_wm.c             ウィンドウマネージャ + デスクトップ + タスクバー
  myos_files.c          ファイルマネージャ
  myos_notepad.c        メモ帳
  myos_image.c          画像ビューア
  myos_settings.c       設定 (画面のプロパティ)
  win98.c               /dev/fb0 に直接描く版 (X なしのデモ)
src/init/     initramfs 用の最小 init (libc 非依存)
src/test/     fake_kernel.asm … ローダー検証用の偽 bzImage
tools/        ビルド・検証スクリプト
  build_image.py        ディスクイメージのビルドと検証
  run_qemu.py           QEMU で起動し、VRAM を吸い出して自動検証
  bzimage_info.py       bzImage の setup ヘッダ解析
  build_kernel.sh       Linux カーネルのビルド
  build_rootfs.sh       Debian + Xorg + Firefox のルートを作る
  make_rootfs_img.sh    それを ext4 イメージに固める
  make_initramfs.sh     initramfs (cpio) の作成
  make_fake_kernel.py   偽 bzImage の作成
docs/         設計メモ・進捗
  kernel.md             カーネルの構成 (モノリシック、ファームウェア)
  image-layout.md       ディスクイメージの構成
  desktop.md            デスクトップ環境 (WM / アプリ / USB)
  install-size.md       容量の見積もり
build/        生成物 (git 管理外)
```

## ビルドと実行

```bash
# 必要なもの
apt-get install -y nasm qemu-system-x86 gcc cpio   # python3 は標準
```

### 本番イメージ

```bash
make kernel     # Linux カーネルをビルド (数十分)
make rootfs     # Debian + Xorg + Firefox のルートを作る (数十分、要ネット)
make image      # build/myos.img を作る
make run        # QEMU で起動して画面とシリアルログを取る
```

`build/myos.img` はそのまま USB メモリに書けば起動する。

```bash
sudo dd if=build/myos.img of=/dev/sdX bs=4M status=progress conv=fsync
```

### ブートローダー単体の検証

```bash
make run-fake   # カーネル不要。偽カーネルでローダーだけを数秒で検証
make run-demo   # フェーズ1 + 2-A のデモ (画面・キーボード・プロテクトモード)
make disasm     # 逆アセンブルして機械語を確認
```

VirtualBox で確認する場合は `build/myos.img` をハードディスクとして
割り当てる。詳しくは `docs/testing.md`。

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
| 2-B | Linux Boot Protocol 対応 | **完了**（ユーザーランド到達を確認） |
| 3, 4 | メモリ管理 / 割り込み | Linux カーネルが担当 |
| 5 | VGA グラフィックス（VBE モード設定 + 描画） | **完了** |
| 6 | マウス・ウィンドウシステム | **着手**（WM / デスクトップ / ファイルマネージャ） |
| 10 | アプリケーション実行（標準 ELF + 既存 glibc） | Firefox / Java が動作 |
| 7〜9, 11 | マルチタスク / メモリ保護 / FS / ドライバ | Linux カーネルが担当 |

フェーズ2-B のゴール「自作ブートローダーから Linux カーネルの起動ログが出る」は
達成済み。実際には initramfs の `/init` が動くところまで到達している。

```
  myOS: userland reached.

  Booted by a hand-written bootloader:
    stage1 (MBR, 512 bytes)
      -> stage2_linux (Linux 32-bit boot protocol)
        -> Linux kernel
          -> this /init (PID 1, no libc)
```

さらに GUI 層として、1024x768x32 のフレームバッファに
Windows 98 もどきのデスクトップ（デスクトップアイコン / ウィンドウ /
タスクバー / マウスカーソル）を描くところまで進んでいる。
X11 もツールキットも使わず `/dev/fb0` へ直接書いている。

デスクトップにはアイコンが並び、ダブルクリックでアプリが起動する。
自作の GUI アプリとして**ファイルマネージャ / メモ帳 / 画像ビューア /
設定 / MS-DOS プロンプト**が載っていて、
そこに Firefox・Java 17・OpenGL が加わる。
時計は BIOS (RTC) 由来のシステム時刻。
USB のキーボード / マウス / メモリが使え、USB メモリは
`/media/<デバイス名>` に自動マウントされる。

**見た目は Windows 98、操作は今の Windows に寄せてある。**
`Alt+Tab` でウィンドウを巡回、`Alt+F4` で閉じる、Windows キーで
スタートメニュー、タイトルバーのダブルクリックで最大化。
`Win+E` でファイルマネージャ、`Win+D` でデスクトップの表示、
`Win+←` / `Win+→` で画面の左右半分にスナップ。
ファイルマネージャでは `Ctrl+C` / `Ctrl+X` / `Ctrl+V` でコピーと移動、
`F2` でリネーム、`Delete` で削除、右クリックでコンテキストメニュー。
コマンドプロンプトは中身こそ Linux のシェルだが、
`dir` / `cls` / `copy` / `del` / `type` といった DOS の名前で叩けて、
プロンプトも `C:\path\to\here>` の形で出る。

**このOSは 64bit (x86_64) です。**
16bit リアルモードで始まるのはブートローダーだけで、
Linux Boot Protocol の 32bit エントリでカーネルに渡したあと、
long mode への移行はカーネルが行う。

ディスクイメージは **4.03 GiB**（カーネル 19.4 MiB + ext4 ルート 4 GiB）、
ルートの実使用は **1.6 GiB**。圧縮すると 497 MiB なので、
インストール用の ISO は **530 MiB 前後**で CD-R 1 枚に収まる見込み。
詳しくは `docs/install-size.md`。

詳しくは `docs/progress.md` / `docs/gui.md` / `docs/desktop.md` /
`docs/image-layout.md` / `docs/install-size.md` を参照。

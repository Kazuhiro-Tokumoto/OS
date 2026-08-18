# デスクトップ環境

```
自作ブートローダー
  -> Linux カーネル
    -> Xorg
      -> myos-wm      … ウィンドウマネージャ + デスクトップ + タスクバー
        -> myos-files … ファイルマネージャ
        -> Firefox / Java / xterm など
```

見た目の部品はすべて `src/gui/x98.h` に集約してある。
Win98 らしさの肝である 2 重の立体枠 (`x98_bevel`)、
横グラデーションのタイトルバー、アイコンの図形はここにある。
ウィンドウマネージャとファイルマネージャが同じものを使うので、
色や枠の作法が勝手にずれていくことがない。

## デスクトップのリンク

`/etc/myos/desktop.conf` に 1 行足すとアイコンが増える。

```
# ラベル|アイコン|コマンド
My Computer|computer|/usr/local/bin/myos-files /
Firefox|globe|/usr/bin/firefox-esr
Java Demo|java|java -jar /usr/local/share/myos/hello.jar
```

アイコン名は `computer` / `folder` / `file` / `app` / `globe` / `java`。
図形はコードで描いているので画像ファイルは要らない。

- **シングルクリック**で選択（Win98 と同じく反転した帯が付く）
- **ダブルクリック**でコマンドを起動

ファイルマネージャで項目を**右クリック**すると、その項目が
デスクトップのリンクとして `desktop.conf` に追記される。
追記後 `myos-wm` に `SIGUSR1` を送って読み直させているので、
再起動しなくてもすぐアイコンが増える。

## ファイルマネージャ (`src/gui/myos_files.c`)

- フォルダが先、次にファイル、それぞれ名前順
- ダブルクリックで開く（振り分けは「ファイル操作」の節の表を参照）
- 上へ / ホーム / 更新
- ホイールでスクロール、上下キーと Enter でも操作できる
- コピー / 移動 / リネーム / 削除と右クリックメニュー（後述）

## ウィンドウの作法 (Z オーダーとキーボード)

「基本的に Windows 11 の操作ができてほしい」という要望に合わせて、
見た目は Win98 のまま、操作だけ今どきの Windows に寄せてある。

### Z オーダー

以前は「クリックしたら `XRaiseWindow` する」だけで、
誰が前面にいるかをウィンドウマネージャ自身が把握していなかった。
今は前面から順に並んだ配列を持っている。

```c
Client *zlist[MAX_CLIENTS];   /* [0] が最前面 */
int     n_z;
Client *focused;
```

`raise_client()` は zlist の先頭に move-to-front して
フォーカスを移し、**それまでフォーカスされていた窓の枠も描き直す**。
アクティブなタイトルバーは紺→水色のグラデーション、
非アクティブは灰色 (`0x808080` → `0xB5B5B5`) になる。
Win98 と同じく、どれが手前かがタイトルバーの色で分かる。

タスクバーのボタンも zlist の順ではなく生成順に並べてあるので、
窓を前後させてもボタンが踊らない。

### キーボード

| キー | 動作 |
| --- | --- |
| `Alt+Tab` | 次のウィンドウへ (zlist を巡回して最前面に出す) |
| `Alt+F4` | アクティブなウィンドウを閉じる (`WM_DELETE_WINDOW`) |
| `Windows` キー | スタートメニューの開閉 |

`XGrabKey` は修飾キーの組み合わせごとに登録しないと効かないので、
`NumLock` (`Mod2Mask`) と `CapsLock` (`LockMask`) の 4 通りで登録している。
ここを忘れると「NumLock が点いていると Alt+Tab が効かない」になる。

### マウス

| 操作 | 動作 |
| --- | --- |
| タイトルバーをドラッグ | 移動 |
| タイトルバーをダブルクリック | 最大化 / 元に戻す |
| 右端 / 下端 / 右下のつまみをドラッグ | リサイズ |
| 枠のどこかをクリック | 最前面 + フォーカス |

## メモ帳 (`src/gui/myos_notepad.c`)

Win98 の Notepad 相当。等幅フォント `9x15` で描く複数行エディタ。

- File メニュー: New / Save / Exit
- `Ctrl+S` 保存 / `Ctrl+N` 新規 / `Ctrl+Q` 終了
- 矢印 / `Home` / `End` / `PageUp` / `PageDown` / `Tab`
- 引数にファイル名を渡すとそれを開く（ファイルマネージャから使う）

行は `char *lines[MAX_LINES]` で持っていて、必要な分だけ確保する。

## 画像ビューア (`src/gui/myos_image.c`)

PNG / JPEG / GIF / BMP を表示する。デコーダは自作せず
**ImageMagick の `convert` に PPM へ変換させて、それを読む**。

```c
execlp("convert", "convert", path, "-depth", "8", "ppm:-", (char *)NULL);
```

- `+` / `-` 拡大縮小、`0` 等倍、`F` 画面に合わせる
- ホイールでも拡大縮小、矢印でスクロール
- 下のステータスバーにファイル名 / 元の大きさ / 倍率

### ハマった点: 16bit の PPM

最初は緑と紫の縞模様が出た。ImageMagick が入力によっては
**1 チャンネル 16bit の PPM (`P6 320 240 65535`)** を吐くためで、
8bit 前提で読むと 1 ピクセルずつずれていく。
`-depth 8` を付けたうえで、`maxval > 255` の PPM は受け取らないようにした。

## ファイル操作 (コピー / 移動 / リネーム)

ファイルマネージャに Win98 相当のファイル操作を入れた。

| 操作 | キー | 備考 |
| --- | --- | --- |
| コピー | `Ctrl+C` | パスを覚えるだけ (`clip_path`) |
| 切り取り | `Ctrl+X` | 貼り付け時に `mv` になる |
| 貼り付け | `Ctrl+V` | `cp -a` または `mv -f` |
| リネーム | `F2` | ツールバー上でその場編集 |
| 削除 | `Delete` | ツールバーに確認が出る |
| 更新 | `F5` | |

右クリックでコンテキストメニュー
(Open / Copy / Cut / Paste / Rename / Delete / Add to Desktop)。

文字入力が要るのはリネームだけなので、`x98.h` に小さな
テキスト入力ウィジェットを足した。メモ帳のような本格的な
エディタとは別物で、1 行分のバッファとカーソル位置しか持たない。

```c
typedef struct { char buf[512]; int len; int cur; } X98Edit;
```

外部コマンドの呼び出しは全て `execlp` で、**シェルを経由しない**。
`system("cp -a " ...)` にすると空白や引用符の入ったファイル名で壊れる。

### 開くときの振り分け

| 種類 | 動作 |
| --- | --- |
| ディレクトリ | そこへ移動 |
| 画像 (png/jpg/gif/bmp) | `myos-image` |
| テキスト (txt/log/conf/sh...) | `myos-notepad` |
| `.jar` | `java -jar` |
| 実行可能 | そのまま実行 |
| それ以外 | `firefox-esr` |

## MS-DOS プロンプト

`xterm` を Win98 風の配色と等幅フォントで出しているだけだが、
`/etc/myos/dosrc` を `--rcfile` で読ませて DOS のコマンド名を通している。

```sh
alias dir='ls -la'    alias cls='clear'   alias copy='cp -i'
alias move='mv -i'    alias del='rm -i'   alias ren='mv'
alias md='mkdir'      alias rd='rmdir'    alias type='cat'
alias ver='uname -a'  alias mem='free -h' alias edit='myos-notepad'
```

プロンプトは `C:\path\to\here>` の形にしている。
中身は普通の Linux のパスで、表示のときに `/` を `\` に置換しているだけ。

```sh
dos_pwd() { printf 'C:'; printf '%s' "$PWD" | tr '/' '\\'; }
PS1='$(dos_pwd)> '
```

### ハマった点: 文字化け

起動時のバナーに日本語を入れたら化けた。
`xterm` に指定している `9x15` はビットマップフォントで、
**日本語のグリフを持っていない**。
バナーは ASCII だけで書くようにした。

（`fixed` 系の 2 バイトフォントを併用すれば日本語も出せるが、
Win98 の DOS 窓らしさを優先して英語のままにしてある）

## 設定アプリ (`src/gui/myos_settings.c`)

Win98 の「画面のプロパティ」相当。配色を選んで `/etc/myos/theme.conf` に
書き、ウィンドウマネージャに `SIGUSR1` を送って即座に反映させる。

- 配色プリセット 6 種（Windows Standard / Desert / Eggplant /
  Rainy Day / Slate / Rose）
- デスクトップの色だけを 16 色から選ぶ
- ダブルクリックの速さ（Slow / Normal / Fast）
- OK / Apply / Cancel

**このアプリ自身も `x98.h` の色で描いているので、選んだ配色が
そのまま自分の見た目に反映される。** 生きたプレビューになっている。

### theme.conf

```
desktop     = 008080
face        = C0C0C0
title1      = 000080
title2      = 1084D0
titletext   = FFFFFF
text        = 000000
select      = 000080
dblclick_ms = 700
```

ハイライトと影の色は面色 (`face`) から機械的に導いている。
面色を変えれば立体枠もそれらしく付いてくるので、
設定項目を増やさずに済んでいる。

## 時計

**RTC (CMOS) から取れている。** ブートローダー側で何かする必要はない。

x86 では Linux カーネルが起動のごく早い段階で CMOS の RTC を読み、
システム時刻に入れている。起動ログにもそのまま出る。

```
[    1.110204] PM: RTC time: 00:18:56, date: 2026-08-18
```

なのでウィンドウマネージャは `time()` と `localtime()` を呼ぶだけでよい。
以前は文字列を決め打ちしていただけだった。

X のイベントだけを待っていると時計が更新されないので、
`select()` で 1 秒ごとに起きて、分が変わったときだけタスクバーを描き直している。

## ハマった点: X に入力デバイスが見えない

マウスもキーボードも一切効かない、という状態になった。
画面は出ているし、カーネルはデバイスを認識している。

```
[    3.384375] input: AT Translated Set 2 keyboard as ...
[    3.837253] input: ImExPS/2 Generic Explorer Mouse as ...
```

原因は **udev を起動していなかったこと**。
X の `AutoAddDevices` は udev 経由でデバイスを見つけるので、
udev がいないと入力デバイスが 1 つも登録されない。
systemd は使わない方針だが、`systemd-udevd` 単体なら普通に動くので、
`/myos-init` で起動している。

```sh
/lib/systemd/systemd-udevd --daemon
udevadm trigger --action=add
udevadm settle --timeout=10
```

## ハマった点: ポインタの加速

QEMU に `mouse_move` を送って自動検証していると座標が合わない。
X が既定でポインタ加速をかけるため、送った移動量と実際の移動量がずれる。

`xorg.conf` の `InputClass` で加速を切って 1:1 にした。
Win98 の操作感にも近くなる。

```
Option "AccelProfile" "flat"
Option "AccelSpeed"   "0"
```

あわせて、QEMU の PS/2 マウスは 1 パケットで送れる移動量が ±255 までなので、
`tools/run_qemu.py` は大きな移動を 100 ピクセルずつに割って送るようにした。

## USB

**ドライバは全部カーネルに組み込み済み。** 追加で何かする必要はなかった。

| 用途 | 設定 |
| --- | --- |
| USB 3.0 / 2.0 / 1.1 | `USB_XHCI_HCD` / `USB_EHCI_HCD` / `USB_OHCI_HCD` / `USB_UHCI_HCD` |
| キーボード・マウス | `USB_HID` + `HID_GENERIC` |
| USB メモリ | `USB_STORAGE` + `SCSI` |
| ファイルシステム | `VFAT_FS`（FAT32）/ `EXFAT_FS` / `NTFS3_FS` |

exFAT と NTFS は今どきの USB メモリ向けに後から足した。

### 自動マウント

`/usr/local/bin/myos-automount` が常駐して 2 秒ごとに見張り、
`/sys/block/*/removable` が 1 のものだけを `/media/<デバイス名>` に
マウントする。**内蔵ディスクを勝手に触らないため**にこの条件を入れている。
抜かれたら `/media` の下を片付ける。

udev のルールでも書けるが、ポーリングのほうが挙動が読みやすい。

デスクトップの "Removable Media" アイコンから `/media` を開ける。

### 動作確認

```bash
python3 tools/run_qemu.py --image build/myos.img --media hdd \
    --usb-hid --usb-storage build/usbstick.img
```

起動ログでこう出れば通っている。

```
usb 1-1: Product: QEMU USB Keyboard
input: QEMU QEMU USB Keyboard as /devices/.../input/input4
usb 1-2: Product: QEMU USB Mouse
hid-generic 0003:0627:0001.0002: input,hidraw1: USB HID v0.01 Mouse ...
usb-storage 2-3:1.0: USB Mass Storage device detected
sd 2:0:0:0: [sdb] Attached SCSI removable disk
[automount] mounted /dev/sdb on /media/sdb
```

## Java

`openjdk-17-jre` を入れてある（JDK ではなく JRE。軽いほうを選んだ）。

デモアプリ `src/java/MyOsHello.java` は Swing の窓を 1 枚出すだけのもの。
**Java のバイトコードは可搬なので、ホスト側でコンパイルした `.jar` を
置くだけでよい。rootfs に JDK を入れずに済む。**

### ハマった点: クラスファイルのバージョン

ホストの JDK が Java 21、rootfs の JRE が Java 17 だったため、
そのままコンパイルすると起動時に落ちた。

```
UnsupportedClassVersionError: MyOsHello has been compiled by a more recent
version of the Java Runtime (class file version 65.0), this version of the
Java Runtime only recognizes class file versions up to 61.0
```

`javac --release 17` を必ず付けること（Makefile の `JAVA_RELEASE`）。

## OpenGL

`libgl1-mesa-dri` を入れてあり、GPU が無い環境では Mesa の
ソフトウェアラスタライザ (llvmpipe) が使われる。
デスクトップの "OpenGL Test" (`glxgears`) で確認できる。

Minecraft のような LWJGL を使うアプリは、これに加えて
`libXcursor` / `libXrandr` / `libXxf86vm` / `libXi` を要求するので、
それらも入れてある。

### Minecraft について

**動かすための土台は揃っているが、Minecraft 本体は同梱できない。**
proprietary であり、Microsoft アカウントでの認証も要るため。

現実的な話として、GPU の無い QEMU では描画がソフトウェア処理になるので、
動いてもフレームレートは実用に耐えない。
実機で GPU を使うなら、その GPU の DRM ドライバをカーネルに組み込む
必要がある（今は QEMU 用の `bochs-drm` などを入れてある）。

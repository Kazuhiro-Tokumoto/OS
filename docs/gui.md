# GUI 層 (フェーズ5・6)

## どこに作るか

カーネルを自作せず Linux を採用した以上、**GUI はカーネル内ではなく
ユーザーランドのプログラムとして作る**のが自然な形になる。

```
自作ブートローダー (stage2_linux)
   VBE でグラフィックモードを設定し、screen_info に書いて渡す
      ↓
Linux カーネル
   screen_info を見てフレームバッファを /dev/fb0 として見せる
      ↓
src/gui/win98.c
   /dev/fb0 に直接ピクセルを書く。X11 もツールキットも使わない
```

「自作の主戦場はブートローダーと GUI 層」という方針どおり、
この 2 つは自分で書き、間のカーネルは Linux に任せている。

## ブートローダー側: VBE でモードを決める

`src/boot/inc/vbe.inc`。

Linux の framebuffer ドライバは「ブートローダーがモードを設定し、
その情報を `boot_params.screen_info` に書いておく」ことを前提にしている。
`screen_info.orig_video_isVGA = VIDEO_TYPE_VLFB (0x23)` がその合図。

1. `INT 10h AX=4F00h` で VbeInfoBlock を取得（`'VESA'` シグネチャを確認）
2. `VideoModePtr` の指すモード番号のリストを頭から舐める
3. 各モードについて `AX=4F01h` で ModeInfoBlock を取り、
   - ModeAttributes bit0（対応）/ bit4（グラフィック）/ bit7（LFB あり）
   - MemoryModel == 6（ダイレクトカラー）
   - 希望の解像度・bpp と一致するか
   を確認する
4. 見つかったら `AX=4F02h`、`BX = モード番号 | 0x4000`（bit14 = LFB を使う）で切り替え

希望の解像度は 1024x768x32 → 800x600x32 → 640x480x32 の順に試し、
それぞれ 24bpp のフォールバックも用意している。
**VBE が無ければテキストモードのまま続行する**（互換性方針どおり、
新しめの機能に依存しきらない）。

モード切り替えは「カーネルへジャンプする直前」に行う。
切り替えた瞬間から `0xB8000` へのテキスト出力は使えなくなるので、
表示したいことはそれより前に全部済ませておく必要がある。

## カーネル側の設定

`tools/build_kernel.sh` が設定している。

| 設定 | 役割 |
| --- | --- |
| `CONFIG_FB` / `CONFIG_FB_DEVICE` | フレームバッファ本体と `/dev/fb0` |
| `CONFIG_SYSFB_SIMPLEFB` | screen_info から `simple-framebuffer` デバイスを登録 |
| `CONFIG_FB_SIMPLE` | その `simple-framebuffer` に結び付くドライバ |
| `CONFIG_FB_VESA` | 旧来の vesafb 経路 |
| `CONFIG_FRAMEBUFFER_CONSOLE` | カーネルログをフレームバッファに出す |
| `CONFIG_INPUT_MOUSEDEV` | `/dev/input/mice`（PS/2 互換の 3 バイトパケット） |
| `CONFIG_MOUSE_PS2` / `CONFIG_SERIO_I8042` | PS/2 マウス本体 |

### ハマった点: デバイスは出来るのにドライバが無い

`CONFIG_SYSFB_SIMPLEFB=y` だけを入れると、
`simple-framebuffer` プラットフォームデバイスは登録されるが、
それにバインドするドライバ（`CONFIG_FB_SIMPLE`）が無いので
`/dev/fb0` が生えない。

症状は起動ログの

```
Console: colour dummy device 80x25
```

これが出たら「screen_info は正しく渡っている（だから VGA テキストを
諦めている）が、フレームバッファのドライバがいない」という意味。

### ハマった点: initramfs にデバイスノードが無い

`/dev/fb0` も `/dev/input/mice` も、initramfs の中に自分で
`mknod` しておく必要がある（devtmpfs が来る前に使いたいため）。
`tools/make_initramfs.sh` が作っている。

| ノード | major/minor |
| --- | --- |
| `/dev/console` | 5, 1 |
| `/dev/fb0` | 29, 0 |
| `/dev/input/mice` | 13, 63 |
| `/dev/input/eventN` | 13, 64+N |

### ハマった点: init の出力先

`console=` を複数並べると、`/dev/console` は**最後に書いたもの**を指す。
GUI が画面を占有するので、`console=tty0 console=ttyS0,115200` の順にして
init の出力はシリアル側に出るようにしてある。

## GUI プログラム

`src/gui/win98.c`。libc を使わず syscall を直接叩く静的バイナリ。

```
open("/dev/fb0")
  → ioctl(FBIOGET_VSCREENINFO) で解像度と bpp
  → ioctl(FBIOGET_FSCREENINFO) で pitch (line_length) とサイズ
  → mmap してそこへ直接書く
```

### 描いているもの

- ティール（`0x008080`）のデスクトップ
- デスクトップアイコン 3 つ（白抜き + 影のラベル付き）
- ウィンドウ 1 枚
  - 横グラデーションのタイトルバー（濃紺 → 明るい青）
  - 最小化 / 最大化 / 閉じるボタン
  - メニューバー（File / Edit / View / Help）
  - 凹み枠の白いクライアント領域
- タスクバー
  - スタートボタン（4 色の旗つき）
  - 押された状態のタスクボタン
  - 凹み枠の時計

### Win98 らしさの肝は「立体枠」

`bevel()` が全部やっている。外側 1px と内側 1px の 2 重になっていて、

- 盛り上がり: 外側の左上 = 白、右下 = 黒 / 内側の左上 = 面色、右下 = 濃灰
- 凹み: その逆

これを押しボタン・タスクバー・クライアント領域・時計と使い回すだけで、
かなりそれらしい見た目になる。

### フォント

`src/gui/font8x8.h`（8x8 ビットマップ、ASCII 0x20-0x7E）。
`tools/make_font.py` が Linux カーネルの `lib/fonts/font_8x8.c` から生成する。
生成物をコミットしてあるので、普段はスクリプトを走らせなくてよい。
元データがカーネル由来なので、このヘッダは GPL-2.0。

### マウス

`/dev/input/mice` は PS/2 互換の 3 バイトパケットを返すので扱いが楽。

```
pkt[0] : ボタンとフラグ
pkt[1] : X の移動量 (符号付き)
pkt[2] : Y の移動量 (符号付き、上が正)
```

画面座標は下が正なので Y は符号を反転する。
カーソルは描く前に下の絵を退避し、動かすときに書き戻している
（`cursor_show` / `cursor_hide`）。

## 検証

```bash
make run-gui
```

`tools/run_qemu.py --mouse="-200:0;0:150"` でマウスの移動を送り込める。
カーソルが追従し、通った跡が残らない（背景がちゃんと復元されている）ことを
スクリーンショットで確認できる。

シェルの都合で、負の値を渡すときは `--mouse=...` と `=` で繋ぐこと。

## これから

- [ ] スタートメニューを出す（クリック判定 → メニュー描画）
- [ ] ウィンドウの移動（タイトルバーのドラッグ）
- [ ] Z オーダーと複数ウィンドウ
- [ ] キーボード入力（`/dev/input/eventN`）
- [ ] ダブルバッファ（今は直接描いているのでちらつく）
- [ ] 描画をライブラリに切り出して、アプリとウィンドウマネージャを分ける

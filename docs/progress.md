# 進捗

## フェーズ0: ブートローダー — 完了

- [x] Stage1 (MBR, 512 バイト)
- [x] Stage2 のロードとジャンプ
- [x] 二段ブート構成の disk.img ビルド

引き継ぎ資料の時点から変えた点:

- ブートドライブ番号を `00h` 決め打ちにせず、**BIOS が `DL` に入れて渡してくる値**を
  保存して使うようにした（USB-FDD エミュレーション等への対応）
- ディスクリセットを挟んだ 5 回リトライを追加（実機のモータ回転待ち対策）
- 読み込むセクタ数を 1 → 17 に拡大（Stage2 が 512 バイトに収まらなくなったため）
- エラー時に `E` 1 文字ではなく `STAGE1: DISK ERROR` を表示
- ブラウザ IDE ではなく nasm を使うようにした。
  資料 5章(1)(3)(4) にあるブラウザ IDE 固有の不具合
  （`TIMES` が効かない、`ES:[DI]` を誤コンパイルする、
  文字リテラルや `$` や far jump が使えない）はすべて解消している。
  ただし「生成バイト列を必ず検算する」方針は維持している。

## フェーズ1: 画面表示の強化 — 完了

- [x] 文字に色を付ける（引き継ぎ資料 Stage2 v2 の検証も完了。`docs/testing.md` 参照）
- [x] 画面クリア処理（`cls`: VRAM を `rep stosw` で一括塗りつぶし）
- [x] カーソル位置の制御（ソフトカーソル `cur_row`/`cur_col` +
      `INT 10h AH=02h` でハードウェアカーソルを同期）
- [x] キーボード入力の受付（`INT 16h AH=00h`）と画面へのエコー
      - Enter で改行してプロンプト再表示、BackSpace で 1 文字削除
      - 拡張キー（矢印など、`AL=0` で返るもの）は無視
      - 最終行まで来たらスクロール（0 行目のタイトルバーは固定）

## フェーズ2-A: プロテクトモード移行 — 完了

- [x] メモリマップ取得（`E820h` → `E801h` → `AH=88h` のフォールバック）
- [x] A20 ゲート有効化（BIOS → 8042 → ポート `0x92` の 3 方式 + 実地検証）
- [x] GDT の作成（フラットモデル）
- [x] `CR0.PE` を立ててプロテクトモードへ
- [x] 32bit コードへの移行、セグメント再設定

QEMU での確認結果:

```
Boot drive (from BIOS DL) : 0x00
Memory map                : INT 15h E820h / usable 127 MB
A20 gate                  : ENABLED
Entering protected mode...
** PROTECTED MODE OK (32bit) - next: Linux Boot Protocol **

CR0 = 0x00000011   PE(bit0) = 1  -> プロテクトモード
```

## フェーズ2-B: Linux Boot Protocol — 完了

**自作ブートローダーから Linux カーネルが起動し、ユーザーランドまで到達した。**

- [x] Linux カーネルのビルド（`tools/build_kernel.sh`、6.12.9 / defconfig）
- [x] bzImage ヘッダ解析（`setup_sects`, `HdrS` マジック、プロトコル版）
- [x] カーネル本体を `0x100000` へ読み込む（アンリアルモード + LBA 拡張読み込み）
- [x] `boot_params`（ゼロページ）の構築
- [x] `type_of_loader` / `loadflags` / `screen_info` などの設定
- [x] E820 メモリマップをゼロページへ（24 バイト → 20 バイトに詰め直し）
- [x] 32bit エントリポイントへジャンプ
- [x] 簡易 initramfs（cpio）の用意とアドレス設定

実装は `src/boot/stage2_linux.asm`。詳細は
`docs/phase2b-linux-boot-protocol.md` を参照。

```
$ make run-linux
myOS Stage2  -  Phase2-B: Linux Boot Protocol loader

Disk        : INT 13h extensions (LBA) available / geometry 63 sect/track, 16
Memory map  : INT 15h E820h / 6 entries / usable 511 MB
A20 gate    : OK
Unreal mode : OK
Payload tbl : OK
bzImage hdr : 'HdrS' found / boot protocol 2.15 / setup_sects 39
Kernel      : loading to 0x100000 ...(略)...OK
initramfs   : loading to 0x4000000 .OK
boot_params : building zero page at 0x90000 OK
Jumping to kernel entry (ESI=boot_params, EBX=EBP=EDI=0)...
```

その後カーネルのログが流れ、最後に initramfs の `/init` が動く。

```
[    0.000000] Linux version 6.12.9 ...
[    0.000000] Command line: console=ttyS0,115200 console=tty0 ... rdinit=/init
[    2.221666] Unpacking initramfs...
[    3.808349] Run /init as init process

  myOS: userland reached.

  Booted by a hand-written bootloader:
    stage1 (MBR, 512 bytes)
      -> stage2_linux (Linux 32-bit boot protocol)
        -> Linux kernel
          -> this /init (PID 1, no libc)
```

### 残っている宿題

- [ ] カーネルを本来の方針どおり「`.ko` を全て `=y`」のモノリシック構成にする
      （`MONOLITHIC=1 sh tools/build_kernel.sh` で試せるようにはしてある。
      現状の検証は defconfig で行った）
- [ ] initramfs を busybox など実用的な中身にする
      （今は動作確認用の `/init` が 1 本あるだけ）
- [ ] CHS 経路（EDD 非対応の古い BIOS）での 13MB 読み込みは 1 セクタずつに
      なるため実機だと相当遅い。トラック単位のまとめ読みを入れたい

## フェーズ5: VGA グラフィックス — 着手

- [x] ブートローダーで VBE のグラフィックモードを設定する（`inc/vbe.inc`）
      1024x768x32 を優先し、無ければ 800x600 → 640x480 と落とす。
      VBE 自体が無ければテキストモードのまま続行する
- [x] `boot_params.screen_info` に LFB の情報を書いてカーネルへ渡す
- [x] カーネルに framebuffer を有効化させ `/dev/fb0` を得る
- [x] ユーザーランドから `/dev/fb0` に直接描画（`src/gui/win98.c`）
- [x] 8x8 ビットマップフォントによる文字描画
- [x] ダブルバッファ（裏バッファに描いてから一括転送）
- [ ] 図形描画の整理（線・矩形・クリッピング）
- [ ] 差分更新（今は毎フレーム全画面を描き直している）

## フェーズ6: マウス・ウィンドウシステム — 着手

- [x] Win98 風の立体枠（`bevel`）と、それを使ったボタン・枠
- [x] デスクトップ / アイコン / ウィンドウ / タスクバーの描画
- [x] `/dev/input/mice` からマウスを読み、カーソルを追従させる
- [x] クリック判定とスタートメニューの開閉
- [x] ウィンドウの移動（タイトルバーのドラッグ）
- [x] X11 のウィンドウマネージャ化（`src/gui/myos_wm.c`）
      Firefox に Win98 のタイトルバーが付き、タスクバーに並ぶところまで
- [x] デスクトップのアイコンとダブルクリック起動
      （`/etc/myos/desktop.conf` で定義。図形はコードで描くので画像不要）
- [x] ファイルマネージャ（`src/gui/myos_files.c`）
      フォルダ移動 / `.jar` は `java -jar` / 実行可能はそのまま実行
- [x] 右クリックでデスクトップにリンクを追加（`SIGUSR1` で即反映）
- [x] 時計を RTC の実時刻にする
- [x] スタートメニューの項目を設定ファイル化し、選ぶと起動するようにした
- [x] ウィンドウのリサイズ（右端 / 下端 / 右下のつまみ）
- [x] 設定アプリ（配色プリセット / デスクトップ色 / ダブルクリック速度）
- [x] ファイル操作（新規フォルダ / 削除、確認付き）
- [ ] リネーム（文字入力の仕組みが要る）
- [ ] コピー / 移動
- [ ] Z オーダーの明示的な管理（今はクリックで最前面に出すだけ）

詳細は `docs/gui.md` と `docs/desktop.md`。

## ユーザーランド

- [x] Debian bookworm の最小構成を ext4 のルートとして同じディスクに載せる
- [x] Xorg（modesetting ドライバ）
- [x] Firefox ESR
- [x] Java 17 (JRE) + Swing のデモアプリ
- [x] OpenGL（Mesa のソフトウェアラスタライザ、`glxgears` で確認）
- [x] xterm（MS-DOS Prompt として）
- [x] USB キーボード / マウス / メモリ（ドライバは全部カーネル組み込み済み）
- [x] リムーバブルメディアの自動マウント（`/media/<デバイス名>`）
- [x] exFAT / NTFS（今どきの USB メモリ向けに追加）
- [ ] Minecraft は同梱できない（proprietary + 要アカウント）。
      土台は揃っているが、GPU 無しでは実用速度にならない

## フェーズ3 以降

`.ko` を全て `=y` にした Linux カーネルを採用する方針のため、
フェーズ3（メモリ管理）・4（割り込み）・7（マルチタスク）・8（メモリ保護）・
9（ファイルシステム）・11（デバイスドライバ）は基本的にカーネルが担う。

自作の主戦場は **フェーズ5（VGA グラフィックス）と
フェーズ6（マウス・ウィンドウシステム）** の GUI 層になる。

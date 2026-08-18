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
- ダブルクリックで開く
  - ディレクトリ → そこへ移動
  - `.jar` → `java -jar` で実行
  - 実行可能 → そのまま実行
  - それ以外 → Firefox に渡す
- 上へ / ホーム / 更新
- ホイールでスクロール、上下キーと Enter でも操作できる

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

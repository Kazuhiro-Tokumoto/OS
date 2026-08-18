# アプリとセキュリティ

## 一般の Linux アプリを入れる

「Minecraft のような普通の Linux アプリを落としてきて、入れて、GUI で動かす」
が通るようにしてある。Linux での配布のされ方は主に 4 通りで、
どれもファイルマネージャからダブルクリックで扱える。

| 形 | ダブルクリックしたときの動き |
| --- | --- |
| `.deb` | `myos-install` が端末で走り、apt に依存ごと入れさせる |
| `.AppImage` | 実行属性を付けてそのまま起動 |
| `.tar.gz` / `.zip` / `.7z` | 「開く」で中身一覧、「実行」で `~/Applications` に展開 |
| `.sh` / `.run` / `.bin` | インストーラとして端末で実行 |
| `.jar` | `myos-java -jar` |

`myos-install` は**入れる前に必ず ClamAV に通す**。
感染していたらそこで止める。

### 入れたアプリがスタートメニューに出る

Linux のアプリは `/usr/share/applications` に `.desktop` を置いていく。
`myos-refresh-menu` がそれを読んで、myOS のスタートメニューの形に直す。

```
Name=       -> ラベル
Exec=       -> コマンド (%f %u などは落とす)
NoDisplay / Terminal のものは出さない
```

ログオンのたびに走るので、入れたアプリは次に入り直せば出てくる。
`myos-install` は最後に自分で呼ぶので、その場でも出る。
`~/Applications` に置いた AppImage も拾う。

### GUI が動くために入れてあるもの

配布物は「これくらいは入っているだろう」という前提で作られているので、
最小構成のままだと起動時に共有ライブラリが無いと言って落ちる。
よく要求されるものを先に入れてある。

```
GTK3 / GTK2 / Cairo / Pango / gdk-pixbuf / at-spi2
NSS / NSPR            … Electron 系がほぼ必ず要求する
ALSA / libXss / libgbm / libdrm / CUPS
libfuse2 / fuse3      … AppImage 用
DejaVu / Liberation / Noto CJK … 文字が四角になるのを防ぐ
```

## Java

**JDK を入れてある。** JRE だけだと `.java` をコンパイルできず、
「Java が入っているのに Java が書けない」という妙な状態になる。

| 拡張子 | 開く | 実行 |
| --- | --- | --- |
| `.java` | メモ帳 | `myos-javac` でコンパイルして実行 |
| `.jar` | `myos-java -jar` | 端末で `myos-java -jar` |
| `.class` | `myos-java` | |

`myos-java` はヒープの上限をメモリ量から決める（1/4、192〜1024MB）。
`/etc/myos/java.conf` か設定アプリの System タブで固定もできる。

## コードエディタ

**Geany** を入れてある。デスクトップとスタートメニューの "Code Editor"。
シンタックスハイライト、プロジェクト、ビルド／実行ボタンが付いた
普通のエディタで、10MB 程度と軽い。

VS Code は**同梱していない**。Microsoft が配っているビルドは
再配布を許さないライセンスなので、公開する ISO に入れられない。
（MIT ライセンスの VSCodium なら入れられるが、Debian のリポジトリには
無いのでビルド時にネットから取ってくることになる。今は入れていない）

自分で入れるなら `.deb` を落としてダブルクリックすれば入る。

## ファイアウォール

**ufw** を入れて、起動時に有効にしてある。

```
入ってくるもの : 全部断る (deny incoming)
出ていくもの   : 許す     (allow outgoing)
```

家庭の PC としてはこれが素直で、これだけでだいぶ違う。
`myos-init` が `ufw --force enable` を叩くので、初回から効いている。

カーネル側にも netfilter を入れてある。defconfig には芯しか無く、
ufw が既定で入れるルールは `-m limit` や `-m addrtype` を使うので、
**これらが無いと `ufw enable` がルールを入れられずに黙って穴が開く**。
`tools/build_kernel.sh` で 13 個の `xt` マッチを有効にしている。

## ウイルス対策 (ClamAV)

### 定義ファイルは焼き込んである

イメージを作るときに `freshclam` を走らせて定義を入れてある。
初回起動時にネットが無くても検査できる。約 250MB 増えるが、
「繋がるまで無防備」よりはいい。

### いつ動くか

`/etc/cron.d/myos-clamav`:

| いつ | 何を |
| --- | --- |
| 毎日 3:12 | `freshclam` で定義を更新 |
| 毎日 12:30 | `/home` `/media` `/tmp` を検査 |
| 起動 5 分後 | `/home` `/media` を検査 |

加えて **ダウンロードのたび**に検査する常駐がいる。

### ダウンロードの監視 (`myos-scan-daemon`)

`~/Downloads` / `~/Desktop` / `/media` を `inotifywait` で見張る。

```sh
inotifywait -m -r -q -e close_write -e moved_to --format '%w%f' $dirs
```

見るのは **書き込みが終わった合図 (`close_write`) と `moved_to`** だけ。
ダウンロード中の半端なファイルを検査しても意味が無いため。
`.part` や `.crdownload` も相手にしない。

一般ユーザーで動く。見張るのは本人のダウンロード先なので root は要らない。

### 見つかったらどうするか

**削除はしない。隔離する。**

| どこから | 隔離先 |
| --- | --- |
| ダウンロード監視 | `~/.myos/quarantine` |
| 定期スキャン | `/var/lib/myos/quarantine` |

誤検知だったときに戻せなくなるのが困るので、消さずに動かすだけにしてある。
そのうえで Win98 風のダイアログ (`myos-alert`) で知らせる。

`myos-notify` は cron から呼ばれたとき用の小物。
cron は `DISPLAY` を持っていないので、動いている X を探して
デスクトップのユーザーに切り替えてから `myos-alert` を出す。

### 手で検査する

スタートメニューの "Virus Scan"、または

```sh
myos-scan            # ホームを検査
myos-scan /media     # USB メモリを検査
```

## SSH

`openssh-server` は入れてあるが、**既定では起動していない。**
置いてあるだけの機械が最初から外に口を開けているのは危ないため。

```sh
myos-ssh on       # sshd を起動し、ufw の 22/tcp を開ける
myos-ssh off      # 止めて、穴も閉じる
myos-ssh          # 今どちらか
```

`PermitRootLogin no`。root は `passwd -l` で塞いであるので、
どのみち root では入れない。

## 起動画面

カーネルを読み込んでいる間、`Booting now` のあとに点が
`.` → `..` → `...` → `.` と回る。

```asm
tick_dots:
        cmp     byte [dot_n], 3
        jb      .add
        ; putc は 0x08 を「1 つ戻って空白で潰す」として扱うので、
        ; 3 回投げれば点が消えてカーソルも元の位置に戻る
        mov     cx, 3
.erase: mov     al, 0x08
        call    putc
        loop    .erase
```

1MB 読むごとに 1 回呼ばれる。点で画面が埋まらず、
止まっていないことも分かる。

# 容量の見積もり

実測値（`build/myos.img` を作った時点のもの）。

## 1. いま出来ているディスクイメージ

| 中身 | 位置 | サイズ |
| --- | --- | --- |
| Stage1 (MBR) | LBA 0 | 512 バイト |
| Stage2 (Linux Boot Protocol ローダー) | LBA 1〜 | 4,275 バイト |
| ペイロードテーブル | LBA 20 | 512 バイト |
| カーネル setup | LBA 64 | 20,480 バイト |
| カーネル本体 (bzImage, zstd, `=m` 0 個) | LBA 104 | 20,370,432 バイト = **19.4 MiB** |
| ext4 ルート (`/dev/sda2`) | LBA 40960 | 4,294,967,296 バイト = **4.0 GiB** |
| **合計** | | **4,324,327,424 バイト = 4.03 GiB** |

自作部分（ブートローダー）は全部で **4.7 KiB**。
残りは Linux カーネルと Debian のユーザーランド。

## 2. ルートファイルシステムの中身

> **セキュリティとアプリ用ランタイムで一度 2.2 GiB まで増えたあと、
> CD-R に収めるために削って 1.7 GiB にした。**



`du -sm` の実測で **1.7 GiB**。
4 GiB のパーティションに対して、空きが 2.3 GiB ほど残る。

### 増えたもの

| 中身 | 増えた量 | なぜ |
| --- | --- | --- |
| OpenJDK 17 JRE | +75 MiB | `.jar` と `.class` を動かす |
| ClamAV の定義 | +108 MiB | ネットが無くても初回から検査できるように |
| GTK3 / NSS / フォント一式 | +200 MiB | 落としてきた GUI アプリが起動時に転ばないように |
| Geany / ufw / sshd / cron ほか | +30 MiB | |

`.cvd` は既に圧縮済みなので、**ClamAV の定義は圧縮しても縮まない**。
ISO の大きさに丸ごと乗る。

### CD-R に収めるために削ったもの

| 削ったもの | 圧縮後で減った量 | 失ったもの |
| --- | --- | --- |
| JDK → JRE | -57 MiB | `.java` のコンパイル |
| gcc + ヘッダ | -50 MiB | `.c` のその場コンパイル実行 |
| Geany | -4 MiB | コードエディタ (メモ帳はある) |
| doc / man / info | -15 MiB | コマンドの説明書 |
| apt のパッケージ一覧 | -20 MiB | `apt update` で作り直せる |
| 日英以外の翻訳 | -20 MiB | 他言語の UI 表示 |
| **合計** | **-206 MiB** | |

**残したもの**: ClamAV の定義 (オフラインでも検査できる) /
日本語フォント (無いと豆腐になる) / ファームウェア (無いと実機で
画面や無線が出ない) / Firefox / Java 実行環境 /
一般 Linux アプリ用のランタイム。

`apt` 自体は残してあるので、あとから `apt install gcc` などで足せる。
掃除は `SLIM=0 sh tools/build_rootfs.sh` で止められる。

| 中身 | サイズ | 備考 |
| --- | --- | --- |
| `usr/lib/x86_64-linux-gnu` | 368 MiB | 共有ライブラリ一式 |
| `usr/lib/firmware` | 301 MiB | 無線 LAN / GPU 用。実機対応のため |
| `usr/lib/firefox-esr` | 271 MiB | |
| `usr/lib/jvm` | 185 MiB | OpenJDK 17 JRE |
| `usr/share/locale` | 84 MiB | |
| `var/lib/apt` | 80 MiB | パッケージ一覧 |
| `usr/lib/gcc` | 79 MiB | GUI アプリを chroot 内でビルドするため |
| `usr/bin` | 72 MiB | |
| `usr/share/icons` | 38 MiB | |
| `usr/share/doc` | 35 MiB | |
| `usr/share/fonts` | 12 MiB | |
| `usr/include` | 15 MiB | |
| その他 | 残り | |

### 削れるもの

インストール後に要らないものを外すと、素の状態で **約 220 MiB** 減る。

| 対象 | 減る量 | 外していい理由 |
| --- | --- | --- |
| `usr/lib/gcc` + `usr/include` | 94 MiB | アプリのビルドはイメージ作成時に済んでいる |
| `var/lib/apt` | 80 MiB | ネットからパッケージを入れない前提なら不要 |
| `usr/share/doc` + `usr/share/man` | 43 MiB | |
| `usr/share/locale` の一部 | 〜70 MiB | 日本語と英語だけ残す場合 |

`usr/lib/firmware` の 301 MiB は残したい。
「どの PC でもそのまま動く」ためのもので、削ると
実機で無線 LAN や GPU が動かなくなる。

## 3. インストールメディア (ISO) — 実測

`tools/build_iso.py` が実際に作ったものの実測値。

| 中身 | サイズ |
| --- | --- |
| カーネル (bzImage) | 19.4 MiB |
| インストーラの initramfs (busybox + 起動時の探索) | 1.06 MiB |
| 圧縮したルート (squashfs / zstd -19, 1M ブロック) | **597 MiB** (626,286,592 バイト) |
| ISO9660 + El Torito + 自作ブートローダー | 1 MiB 前後 |
| **合計 `build/myos-install.iso`** | **618 MiB** (648,366,080 バイト) |

**CD-R 1 枚 (703 MiB = 737,280,000 バイト) に収まる。余裕は 85 MiB。**
USB メモリなら 1 GiB 以上のものに `dd` で書ける。

一度 860 MiB まで膨らんだものを、上の表のとおり削って戻した。
削る前は余裕が 3 MiB しか無く、実用にならなかった。

squashfs は tar + zstd より小さくなる。
ブロック単位で圧縮するぶん不利なはずだが、
重複するファイルを 1 つにまとめてくれるので、
結果としては 24 MiB ほど得をしている。

## 4. インストール先のディスクに必要な容量

| | 容量 | 備考 |
| --- | --- | --- |
| 最低限 | 2.5 GiB | ルート 1.7 GiB + 作業領域 |
| 推奨 | **4 GiB** | いまのイメージと同じ構成 |
| 余裕を見るなら | 8 GiB 以上 | ユーザーのファイル置き場込み |

いまはスワップを切っていないので、メモリは実装ぶんだけ使う。
2006 年前後の PC を想定すると 1〜2 GiB あたりになるが、
Firefox を動かすなら 2 GiB は欲しい。

## 5. 測り直し方

```bash
du -sm /home/user/rootfs                       # ルートの実サイズ
ls -l build/myos.img build/rootfs.ext4         # イメージ
ls -l ../kernelbuild/linux-6.12.9/arch/x86/boot/bzImage
cd /home/user/rootfs && tar -c . | zstd -8 -T0 -c | wc -c   # 圧縮後
```

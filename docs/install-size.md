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

`du -sm` の実測で **1,626 MiB**（≒ 1.59 GiB）。
4 GiB のパーティションに対して、空きが 2.4 GiB ほど残る。

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

## 3. インストールメディア (ISO) の見積もり

インストーラは「圧縮したルートを ISO に載せて、
インストール先へ展開する」形にする想定。

| 中身 | 見積もり |
| --- | --- |
| カーネル (bzImage) | 19.4 MiB |
| インストーラの initramfs (busybox + セットアップ画面) | 10 MiB 前後 |
| 圧縮したルート (squashfs / zstd) | **497 MiB**（実測: tar + zstd -8 で 521,114,033 バイト） |
| ISO9660 + El Torito のオーバーヘッド | 1 MiB 前後 |
| **合計** | **約 530 MiB** |

**CD-R 1 枚 (700 MiB) に収まる。** USB メモリなら 1 GiB 以上のもの。

上の「削れるもの」を全部やると圧縮後で **450 MiB 前後**まで下がるが、
どのみち CD-R に収まるので、無理に削る必要はない。

## 4. インストール先のディスクに必要な容量

| | 容量 | 備考 |
| --- | --- | --- |
| 最低限 | 2.5 GiB | ルート 1.6 GiB + 作業領域 |
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

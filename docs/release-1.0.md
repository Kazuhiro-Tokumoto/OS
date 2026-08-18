# myOS 1.0

Windows 98 の見た目と操作感を目指した自作 OS の、最初の公開版。

ブートローダーは自作、カーネルは Linux（`.ko` を全て `=y` にした
モノリシック構成）、GUI 層は Xlib で自作。

## 収録物

| ファイル | サイズ | 内容 |
| --- | --- | --- |
| `myos-install-1.0.iso` | 618 MiB | 起動できるインストールディスク |

CD-R 1 枚（703 MiB）に 85 MiB の余裕を残して収まる。
書き終わったら `sha256sum` を突き合わせること。

```
4de837a7f47d6b44cc9f887c1d8edd9d06deadb1779ae8be39c5e640df52b865  myos-install-1.0.iso
```

## 使い方

### CD に焼く

```
# Linux
wodim dev=/dev/sr0 -v -sao myos-install-1.0.iso

# Windows / macOS
イメージを右クリック → 「ディスクイメージの書き込み」
```

### USB メモリに書く

同じ ISO がそのまま USB でも使える。

```
sudo dd if=myos-install-1.0.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

`/dev/sdX` は書き込み先の USB メモリ。**間違えると中身が消える**ので
`lsblk` で必ず確認すること。

### 入れる

1. CD / USB から起動する
2. 青い画面でインストール先のディスクを選ぶ
   - ディスクを丸ごと消す / 空き領域に入れる のどちらかを選ぶ
   - **F8 を押すまで何も書き込まない**
3. コピーが終わると自動で再起動する
4. 再起動後にユーザー名・パスワード・キーボード配列・タイムゾーンを決める

## 必要なもの

| 項目 | 条件 |
| --- | --- |
| CPU | x86_64（Core 2 Duo 世代 / 2006年以降） |
| ファームウェア | **レガシー BIOS**（UEFI は対象外） |
| メモリ | 1 GB 以上（Firefox を使うなら 2 GB） |
| ディスク | 2.5 GB 以上（推奨 4 GB） |
| 起動メディア | CD-R、または 1 GB 以上の USB メモリ |

UEFI しかない機械では起動しない。BIOS の設定に CSM / Legacy Boot が
あれば、それを有効にする。

## 入っているもの

- デスクトップ（ウィンドウマネージャ / タスクバー / スタートメニュー）
- ファイルマネージャ、メモ帳、画像ビューア、設定、MS-DOS プロンプト
- Firefox ESR
- Java 17（JRE）
- ClamAV（定義入り。オフラインでも検査できる）
- ufw（起動時に有効。入ってくる通信は既定で断る）
- OpenSSH サーバー（既定では停止。`myos-ssh on` で開始）
- Python 3

`apt` がそのまま使えるので、Debian のパッケージを後から入れられる。

```
sudo apt update
sudo apt install gimp vlc gcc
```

## 操作

見た目は Windows 98、キー操作は今の Windows に寄せてある。

| キー | 動作 |
| --- | --- |
| `Alt+Tab` / `Alt+Shift+Tab` | ウィンドウの切り替え |
| `Alt+F4` | 閉じる |
| Windows キー | スタートメニュー |
| `Win+E` | ファイルマネージャ |
| `Win+D` | デスクトップの表示 |
| `Win+←` / `Win+→` | 画面の左右半分にスナップ |

## 分かっている制限

- **UEFI では起動しない。** レガシー BIOS 専用
- `.java` のコンパイルと `.c` のその場実行はできない
  （CD-R に収めるため javac と gcc を外した。`apt install` で戻せる）
- systemd を使っていないので、デーモンを自動起動するパッケージは
  `/myos-init` に起動行を自分で足す必要がある
- 「空き領域に入れる」を選んだ場合、MBR は myOS のものに置き換わる。
  既存 OS のデータは残るが、そのままでは起動しなくなる
- GPU が無い環境では 3D はソフトウェア処理になり、実用速度は出ない

## 中身の話

詳しくはリポジトリの `docs/` を参照。

- `docs/bootloader.md` — 自作ブートローダー
- `docs/phase2b-linux-boot-protocol.md` — Linux Boot Protocol での起動
- `docs/kernel.md` — モノリシックカーネルの構成
- `docs/desktop.md` — デスクトップと関連付け
- `docs/security.md` — 権限とセットアップ
- `docs/apps-and-security.md` — アプリ、ファイアウォール、ウイルス対策
- `docs/install-size.md` — 容量の内訳

## 作り直す

```bash
# 1. ルートファイルシステム (debootstrap から。1 回目は 30 分ほど)
sudo tools/build_rootfs.sh

# 2. カーネル (.ko を全て =y にしたモノリシック構成)
sudo tools/build_kernel.sh

# 3. インストーラの initramfs
sudo tools/make_install_initramfs.sh

# 4. ルートを固める
sudo mksquashfs /home/user/rootfs build/myos.squashfs \
     -comp zstd -Xcompression-level 19 -b 1M -noappend

# 5. ISO
sudo python3 tools/build_iso.py \
     --kernel  ../kernelbuild/linux-6.12.9/arch/x86/boot/bzImage \
     --initrd  build/install-initramfs.cpio.gz \
     --payload build/myos.squashfs
```

## 確かめたこと

QEMU 上で、CD から入れて再起動するところまで通してある。

| 段階 | 結果 |
| --- | --- |
| CD から起動 (El Torito, ノーエミュレーション) | Stage2 が CD を認識、2048 バイトセクタで読む |
| initramfs がメディアを探す | `/dev/sr0` を見つけて squashfs をマウント |
| 青いテキスト画面 | ディスク一覧 → 入れ方 → 確認 |
| パーティションとフォーマット | 128 MB の生領域 + ext4 のルート |
| GUI のコピー | 1.7 GiB を展開、進捗が進む |
| ブートローダーの書き込み | MBR / Stage2 / ペイロードテーブル / カーネル |
| 再起動して installed から起動 | `/dev/sda2` を ext4 で read-write マウント |
| 初回セットアップ | 「myOS Setup - Step 1 of 5」が出る |

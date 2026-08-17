# フェーズ2-B: Linux Boot Protocol 対応の手順

参照: Linux カーネルソース `Documentation/arch/x86/boot.rst`
（古いカーネルでは `Documentation/x86/boot.txt`）

自作ブートローダーから `bzImage` を **32bit Boot Protocol** で直接起動する。
GRUB 等は使わない。Long Mode への遷移はカーネル側が担うので、
ブートローダーは 32bit プロテクトモードまで持っていけばよい。

## ステップ0: カーネルをビルドする

まだ未着手。方針は「`.ko`（ローダブルモジュール）を全て `=y` にして
モノリシックな `bzImage` 1 個で完結させる」。

```bash
make menuconfig     # モジュールを全て組み込みに
make -j$(nproc) bzImage
```

できた `arch/x86/boot/bzImage` をこのツールに食わせてヘッダを確認する。

```bash
python3 tools/bzimage_info.py path/to/bzImage --hexdump
```

## ステップ1: ヘッダの確認

| オフセット | フィールド | 見るポイント |
| --- | --- | --- |
| `0x1F1` | `setup_sects` | setup のセクタ数。**0 のときは 4 とみなす**（古い規約） |
| `0x1F4` | `syssize` | 本体サイズ / 16（paragraph 単位） |
| `0x1FE` | `boot_flag` | `0xAA55` でなければ bzImage ではない |
| `0x202` | `header` | マジック `'HdrS'`。無ければ Boot Protocol 非対応 |
| `0x206` | `version` | Boot Protocol バージョン（`0x020C` なら 2.12） |
| `0x210` | `type_of_loader` | 自作ローダーは `0xFF` を書く |
| `0x211` | `loadflags` | bit0 `LOADED_HIGH` を立てる（本体を 1MB へ置く） |
| `0x214` | `code32_start` | 32bit エントリポイント。通常 `0x100000` |
| `0x218` | `ramdisk_image` | initramfs のロード先物理アドレス |
| `0x21C` | `ramdisk_size` | initramfs のサイズ |
| `0x228` | `cmd_line_ptr` | コマンドライン文字列の物理アドレス |

プロテクトモード用のカーネル本体は、ファイル先頭から
`(setup_sects + 1) * 512` バイト目から始まる。

## ステップ2: カーネル本体を 0x100000 へ読み込む

ここが技術的な山場。リアルモードのセグメント:オフセットでは
1MB より上を直接指せないため、次のどちらかが必要になる。

### 案A: ビッグリアルモード（アンリアルモード）

1. 一度プロテクトモードに入り、データセグメントのリミットを 4GB に設定
2. プロテクトモードから戻る（`CR0.PE` を落とす）
3. セグメントディスクリプタキャッシュにリミット 4GB が残るので、
   リアルモードのまま 32bit オフセットで 1MB 超にアクセスできる
4. この状態で `INT 13h` を使ってディスクから読み、そのまま高位へ書き込む

### 案B: 低位バッファ経由でコピー

1. リアルモードのまま `INT 13h` で低位メモリ（例: `0x10000`）へ読む
2. `INT 15h AH=87h`（拡張メモリブロック移動）で 1MB 超へコピーする
   または一時的にプロテクトモードへ入ってコピーする

**互換性方針としては案Bの方が枯れている**が、`AH=87h` は
非対応の BIOS もあるため、案A・案Bの両方を用意してフォールバックするのが安全。

### ディスク読み込み自体のフォールバック

1MB 超を扱う段階ではフロッピーではなく HDD / USB から読むことになるので、
`INT 13h AH=41h` で拡張機能の対応をチェックし、
対応していれば `AH=42h`（LBA 拡張読み込み）、
非対応なら `AH=02h`（CHS）にフォールバックする。

## ステップ3: boot_params（ゼロページ）の構築

`boot_params` は 4KB の構造体。慣例的に `0x90000` に置く。

1. まず 4KB 全体を 0 で埋める
2. bzImage の `0x01F1`〜`0x0268`（setup ヘッダ）を
   ゼロページの**同じオフセット**へそのままコピーする
3. 以下を書き換える／追記する

| ゼロページ内オフセット | フィールド | 設定値 |
| --- | --- | --- |
| `0x210` | `type_of_loader` | `0xFF`（自作ローダー） |
| `0x211` | `loadflags` | bit0 `LOADED_HIGH` を立てる。bit7 `CAN_USE_HEAP` は heap を使うなら |
| `0x224` | `heap_end_ptr` | heap を使う場合のみ |
| `0x228` | `cmd_line_ptr` | コマンドライン文字列の物理アドレス |
| `0x218` / `0x21C` | `ramdisk_image` / `ramdisk_size` | initramfs を使う場合 |
| `0x1E8` | `e820_entries` | E820 のエントリ数（1 バイト） |
| `0x2D0` | `e820_table` | E820 のエントリ本体。領域サイズ `0xA00` = 2560 バイト |

**注意**: ゼロページの E820 テーブルは 1 エントリ **20 バイト**
（base 8 + length 8 + type 4）。
領域が 2560 バイトで最大 128 エントリなので 2560 / 128 = 20 バイト、と裏が取れる。

`INT 15h E820h` を 24 バイトバッファで呼んだ場合、
ACPI 3.0 拡張属性の 4 バイトは**含めずに**詰めること。
`src/boot/stage2.asm` の `do_e820` は 24 バイト単位で取得しているので、
ゼロページへ移すときに 20 バイトずつ詰め直す必要がある。

コマンドラインの例（GUI 層のことを考えると最低限これくらい）:

```
console=tty0 root=/dev/ram0 rdinit=/init
```

## ステップ4: カーネルへジャンプ

32bit Boot Protocol の規約:

`boot.rst` の "32-bit BOOT PROTOCOL" の節より（原文）:

> At entry, the CPU must be in 32-bit protected mode with paging disabled;
> a GDT must be loaded with the descriptors for selectors `__BOOT_CS`(0x10)
> and `__BOOT_DS`(0x18); both descriptors must be 4G flat segment;
> `__BOOT_CS` must have execute/read permission, and `__BOOT_DS` must have
> read/write permission; CS must be `__BOOT_CS` and DS, ES, SS must be
> `__BOOT_DS`; interrupt must be disabled; `%esi` must hold the base address
> of the struct boot_params; `%ebp`, `%edi` and `%ebx` must be zero.

まとめると:

- プロテクトモードであること（`CR0.PE = 1`）
- **ページング無効**（`CR0.PG = 0`）
- 割り込み禁止（`cli`）、NMI も止めておく
- GDT に **`__BOOT_CS` = セレクタ `0x10`（実行/読み取り可）**と
  **`__BOOT_DS` = セレクタ `0x18`（読み書き可）**、どちらも 4GB フラット
- `CS = 0x10`、`DS` / `ES` / `SS = 0x18`
- `ESI` = boot_params（ゼロページ）の物理アドレス
- `EBP` / `EDI` / `EBX` = 0
- `code32_start`（通常 `0x100000`）へジャンプ

```
    cli
    mov     esi, 0x00090000     ; boot_params
    xor     ebp, ebp
    xor     edi, edi
    xor     ebx, ebx
    jmp     0x00100000
```

### 注意1: セレクタ番号

**セレクタ番号が `0x10` / `0x18` で固定されている**のがハマりどころ。
教科書的な GDT はコードを `0x08`、データを `0x10` に置くが、
それではカーネルに渡せない。

`src/boot/stage2.asm` の GDT は最初からこの規約に合わせてあり、
インデックス 1（セレクタ `0x08`）を空の詰め物にして
`GDT_CODE32 = 0x10` / `GDT_DATA32 = 0x18` としてある。
フェーズ2-B ではこの GDT をそのまま使える。

### 注意2: boot_params を渡すレジスタ

引き継ぎ資料には「`EAX=0x2, EBX=boot_params`」と書かれているが、
これは Multiboot の規約と混ざっている。
**Linux の 32bit Boot Protocol では `ESI` に boot_params を渡す**のが正しい。
`EBX` はむしろ 0 にしなければならない。

## ステップ5: initramfs

ルートファイルシステム代わりに簡易 initramfs（cpio アーカイブ）を用意する。

```bash
# newc 形式で固める（Linux が読めるのはこの形式）
find . | cpio -o -H newc > ../initramfs.cpio
```

ディスクから読み込んで物理メモリに置き、
そのアドレスとサイズをゼロページの `ramdisk_image` / `ramdisk_size` に書く。
置き場所は `initrd_addr_max`（`0x22C`）以下でなければならない。

## デバッグの目安

ゴールは「自作ブートローダーから Linux カーネルの起動ログ、
あるいはカーネルパニックメッセージが画面に出る」状態。

- 画面が真っ暗のまま何も出ない → ジャンプ先かゼロページが壊れている疑い
- カーネルパニックが出る → **ジャンプまでは成功している**。
  ここまで来れば大きな山は越えている。あとは root fs や
  コマンドラインの問題であることが多い
- QEMU なら `-append` を使った正規の起動と挙動を比較すると切り分けやすい

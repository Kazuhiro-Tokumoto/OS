# カーネル

自作せず Linux カーネルをそのまま採用している。
引き継ぎ資料の方針どおり **`.ko` (ローダブルモジュール) を全て `=y` にして、
`bzImage` 1 個で完結させる**。

```bash
make kernel                          # モノリシック構成 (既定)
MONOLITHIC=0 make kernel             # defconfig のまま (ビルドが速い)
```

## なぜ全部組み込むのか

初回起動時にドライバを入れる手間を無くし、**どの PC に挿しても
そのまま動く**ようにするため。モジュールにしておくと

- ルートファイルシステムをマウントする前に必要なドライバは
  initramfs に入れておかないといけない
- どのモジュールが要るかは機種ごとに違う
- 結局「動かないから調べて入れる」という作業が発生する

全部組み込んでしまえばこの手間が消える。
代わりに `bzImage` は大きくなり、ビルドにも時間がかかる。

## 何を有効にしているか

`tools/build_kernel.sh` が `defconfig` を土台にして、

1. `.config` の `=m` を全て `=y` に置換する
2. よくある PC のハードウェアを明示的に足す

`defconfig` は「そこそこ動く最小限」でしかないので、2 が要る。

| 分類 | 主なもの |
| --- | --- |
| ストレージ | NVMe / AHCI / ATA / USB / SD カード / RAID / virtio |
| ファイルシステム | ext4 / Btrfs / XFS / F2FS / FAT32 / exFAT / NTFS3 / ISO9660 / UDF / SquashFS |
| 入力 | PS/2 (Synaptics・ALPS・Elantech) / USB HID / I2C HID |
| USB | xHCI / EHCI / OHCI / UHCI / ストレージ / シリアル |
| グラフィック | i915 / amdgpu / radeon / nouveau / vmwgfx / qxl / bochs / virtio-gpu / AST / MGA |
| 有線 LAN | Intel (e1000/e1000e/igb/igc/ixgbe) / Realtek / Broadcom / Marvell / nVidia / Atheros / VIA / USB NIC |
| 無線 LAN | iwlwifi / ath9k / ath10k / rtw88 / brcm / ralink / mediatek |
| サウンド | HD Audio (Realtek・Analog・HDMI・VIA・Conexant・Cirrus・IDT) / USB Audio |
| 電源・その他 | ACPI / cpufreq / intel_pstate / amd_pstate / thermal / I2C / hwmon / RTC |

圧縮は `zstd` にしてある。全部入りだと展開前が大きくなるので、
圧縮率と展開速度のバランスがよいものを選んだ。

## ファームウェアは別問題

**ドライバを組み込んでも、ファームウェアの blob は別途要る。**
Wi-Fi、最近の GPU、一部の有線 LAN がこれに当たる。
組み込みにしたからといって不要にはならない。

`tools/build_rootfs.sh` が Debian の `non-free-firmware` から
以下を入れている。

```
firmware-linux-free  firmware-misc-nonfree  firmware-realtek
firmware-iwlwifi     firmware-atheros       firmware-brcm80211
firmware-amd-graphics  firmware-intel-sound
```

組み込みドライバはルートファイルシステムがマウントされる前に
初期化されるので、その時点では `/lib/firmware` を読めない。
そのため `FW_LOADER_USER_HELPER_FALLBACK` を有効にして、
ユーザーランドが立ち上がってから読めるようにしてある。
それでも取りこぼす場合があるので、
**Wi-Fi など一部のデバイスは起動直後には使えないことがある**。

## ブートローダー側の都合

モノリシックな `bzImage` は展開前でも数十 MB になる。
そのため `src/boot/stage2_linux.asm` を次のように調整してある。

- initramfs の置き場を 64MB から **128MB** へ移した。
  カーネル本体は 1MB から上に伸びるので、離しておかないとぶつかる
- 読み込みの進捗を表す点を、チャンクごとから **1MB ごと** に変えた。
  そうしないと画面が点で埋まる

ローダー自体は 32bit のアドレスで転送しているので、
サイズの上限はメモリ量だけ。

## KASLR は切ってある

`CONFIG_RANDOMIZE_BASE` を無効にしている。
自作ブートローダーの問題とカーネルの問題を切り分けやすくするため。
安定したら戻してよい。

#!/bin/sh
# ============================================================================
# build_kernel.sh  -  myOS が起動する Linux カーネルをビルドする
#
# 方針 (引き継ぎ資料 1章):
#   カーネルは自作せず Linux カーネルを採用する。
#   最終的には .ko (ローダブルモジュール) を全て =y にして
#   モノリシックな bzImage 1 個で完結させる。
#
# 使い方:
#   sh tools/build_kernel.sh [作業ディレクトリ] [バージョン]
#   sh tools/build_kernel.sh ~/kernelbuild 6.12.9
#
# 環境変数:
#   MONOLITHIC=1   … .config の =m を全て =y に変換してからビルドする
#                    (ビルド時間とサイズが大幅に増える。最終形はこちら)
# ============================================================================
set -e

WORKDIR="${1:-$HOME/kernelbuild}"
VERSION="${2:-6.12.9}"
SRC="$WORKDIR/linux-$VERSION"
JOBS="$(nproc)"

echo "=== ビルド依存パッケージ ==="
if command -v apt-get >/dev/null 2>&1; then
    apt-get install -y --no-install-recommends \
        build-essential bc bison flex libelf-dev libssl-dev cpio zstd >/dev/null
fi

echo "=== ソースの取得 ==="
mkdir -p "$WORKDIR"
if [ ! -d "$SRC" ]; then
    MAJOR="$(echo "$VERSION" | cut -d. -f1)"
    cd "$WORKDIR"
    [ -f "linux-$VERSION.tar.xz" ] || \
        curl -sS -O "https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-$VERSION.tar.xz"
    tar xf "linux-$VERSION.tar.xz"
fi

cd "$SRC"

echo "=== 設定 ==="
make defconfig

# 自作ブートローダーからの起動を確認しやすくするための設定。
# シリアルコンソールに出せると QEMU の -serial でログを丸ごと取れる。
./scripts/config --enable SERIAL_8250
./scripts/config --enable SERIAL_8250_CONSOLE
./scripts/config --enable EARLY_PRINTK
./scripts/config --enable VT
./scripts/config --enable VT_CONSOLE
./scripts/config --enable VGA_CONSOLE
./scripts/config --enable BLK_DEV_INITRD
./scripts/config --enable RD_GZIP
./scripts/config --enable BINFMT_ELF

# --- フェーズ5 (GUI) 用: フレームバッファと入力 ---------------------------
# 自作ブートローダーが VBE でリニアフレームバッファを設定し、
# screen_info.orig_video_isVGA = VIDEO_TYPE_VLFB (0x23) を立てて渡す。
# それを受けて vesafb が /dev/fb0 を作る。GUI はそこへ直接描く。
./scripts/config --enable FB
./scripts/config --enable FB_DEVICE          # /dev/fb0 を作るのに必要
./scripts/config --enable FB_VESA
./scripts/config --enable FB_VGA16
# SYSFB_SIMPLEFB は screen_info を見て "simple-framebuffer" プラットフォーム
# デバイスを登録するだけ。それに結び付くドライバ (FB_SIMPLE) を入れておかないと
# デバイスは出来るのに誰もバインドせず、/dev/fb0 が生えない。
# 症状は「Console: colour dummy device 80x25」と出て画面が死ぬこと。
./scripts/config --enable SYSFB_SIMPLEFB
./scripts/config --enable FB_SIMPLE
./scripts/config --enable FRAMEBUFFER_CONSOLE
./scripts/config --enable FRAMEBUFFER_CONSOLE_DETECT_PRIMARY
# マウスとキーボードを読むため
./scripts/config --enable INPUT_MOUSEDEV     # /dev/input/mice (PS/2 形式)
./scripts/config --enable INPUT_EVDEV        # /dev/input/eventN
./scripts/config --enable SERIO_I8042
./scripts/config --enable MOUSE_PS2
./scripts/config --enable KEYBOARD_ATKBD
# ブートローダー側でまだ扱えないものを外しておく
./scripts/config --disable RANDOMIZE_BASE       # KASLR。切り分けを簡単にする

# --- モノリシック構成 -------------------------------------------------------
# 引き継ぎ資料の方針どおり「.ko を全て =y にして bzImage 1 個で完結させる」。
# 初回にドライバを入れる手間を無くし、どの PC でもそのまま動かすのが狙い。
#
# MONOLITHIC=0 を渡すと従来どおり defconfig のままにできる (ビルドが速い)。
if [ "${MONOLITHIC:-1}" = "1" ]; then
    echo "=== モジュール (=m) を全て =y に変換 ==="
    sed -i 's/^\(CONFIG_[A-Z0-9_]*\)=m$/\1=y/' .config

    echo "=== よくある PC のハードウェアを広く有効化 ==="
    # defconfig は「そこそこ動く最小限」でしかないので、
    # 実機で使いそうなものを明示的に足す。
    # ここに無いものは各自で追加すること。

    # 圧縮方式。全部入りだと展開前が大きくなるので、
    # 圧縮率と展開速度のバランスがよい zstd にする。
    ./scripts/config --enable KERNEL_ZSTD

    # --- ストレージ ---
    for o in BLK_DEV_NVME NVME_CORE SATA_AHCI SATA_AHCI_PLATFORM ATA_PIIX \
             ATA_GENERIC ATA_SFF PATA_LEGACY BLK_DEV_SD BLK_DEV_SR CHR_DEV_SG \
             SCSI_LOWLEVEL MMC MMC_BLOCK MMC_SDHCI MMC_SDHCI_PCI \
             MMC_SDHCI_ACPI BLK_DEV_MD MD_RAID0 MD_RAID1 BLK_DEV_DM \
             VIRTIO_BLK VIRTIO_SCSI VIRTIO_PCI VIRTIO_NET VIRTIO_CONSOLE \
             VIRTIO_INPUT USB_UAS; do
        ./scripts/config --enable $o
    done

    # --- ファイルシステム ---
    for o in EXT4_FS BTRFS_FS XFS_FS F2FS_FS VFAT_FS EXFAT_FS NTFS3_FS \
             ISO9660_FS JOLIET UDF_FS FUSE_FS NLS_UTF8 NLS_CODEPAGE_932 \
             NLS_CODEPAGE_437 NLS_ISO8859_1 OVERLAY_FS SQUASHFS \
             SQUASHFS_XZ SQUASHFS_ZSTD; do
        ./scripts/config --enable $o
    done

    # --- 入力 ---
    for o in INPUT_EVDEV INPUT_MOUSEDEV SERIO_I8042 SERIO_SERPORT \
             KEYBOARD_ATKBD MOUSE_PS2 MOUSE_PS2_ALPS MOUSE_PS2_ELANTECH \
             MOUSE_PS2_SYNAPTICS MOUSE_SYNAPTICS_I2C INPUT_TOUCHSCREEN \
             HID_GENERIC HID_APPLE HID_LOGITECH HID_MICROSOFT HID_MULTITOUCH \
             USB_HID I2C_HID I2C_HID_ACPI; do
        ./scripts/config --enable $o
    done

    # --- USB のホストコントローラ ---
    for o in USB USB_XHCI_HCD USB_XHCI_PCI USB_EHCI_HCD USB_EHCI_PCI \
             USB_OHCI_HCD USB_OHCI_HCD_PCI USB_UHCI_HCD USB_STORAGE \
             USB_ACM USB_SERIAL USB_SERIAL_FTDI_SIO USB_SERIAL_PL2303 \
             USB_PRINTER; do
        ./scripts/config --enable $o
    done

    # --- グラフィック ---
    # amdgpu と i915 と nouveau は巨大だが、これが無いと実機で画面が出ない。
    for o in DRM DRM_KMS_HELPER DRM_FBDEV_EMULATION DRM_SIMPLEDRM \
             DRM_I915 DRM_AMDGPU DRM_RADEON DRM_NOUVEAU DRM_VMWGFX \
             DRM_QXL DRM_BOCHS DRM_VIRTIO_GPU DRM_AST DRM_MGAG200 \
             FB FB_DEVICE FB_VESA FB_EFI FB_SIMPLE \
             FRAMEBUFFER_CONSOLE FRAMEBUFFER_CONSOLE_DETECT_PRIMARY \
             BACKLIGHT_CLASS_DEVICE; do
        ./scripts/config --enable $o
    done

    # --- 有線 LAN ---
    for o in ETHERNET NET_VENDOR_INTEL E100 E1000 E1000E IGB IGC IXGBE \
             NET_VENDOR_REALTEK 8139CP 8139TOO R8169 \
             NET_VENDOR_BROADCOM TIGON3 BNX2 \
             NET_VENDOR_MARVELL SKGE SKY2 \
             NET_VENDOR_NVIDIA FORCEDETH \
             NET_VENDOR_ATHEROS ATL1 ATL1C ATL1E \
             NET_VENDOR_VIA VIA_RHINE VIA_VELOCITY \
             USB_NET_DRIVERS USB_USBNET USB_NET_AX8817X USB_NET_CDCETHER \
             USB_RTL8152; do
        ./scripts/config --enable $o
    done

    # --- 無線 LAN (ファームウェアが別途要る点に注意) ---
    for o in WLAN CFG80211 MAC80211 WLAN_VENDOR_INTEL IWLWIFI IWLMVM IWLDVM \
             WLAN_VENDOR_ATH ATH9K ATH9K_PCI ATH10K ATH10K_PCI \
             WLAN_VENDOR_REALTEK RTW88 RTW88_8822BE RTW88_8822CE RTL8XXXU \
             WLAN_VENDOR_BROADCOM B43 BRCMSMAC BRCMFMAC \
             WLAN_VENDOR_RALINK RT2800PCI RT2800USB \
             WLAN_VENDOR_MEDIATEK MT7601U; do
        ./scripts/config --enable $o
    done

    # --- サウンド ---
    for o in SOUND SND SND_PCM SND_HDA_INTEL SND_HDA_GENERIC \
             SND_HDA_CODEC_REALTEK SND_HDA_CODEC_ANALOG SND_HDA_CODEC_HDMI \
             SND_HDA_CODEC_VIA SND_HDA_CODEC_CONEXANT SND_HDA_CODEC_CIRRUS \
             SND_HDA_CODEC_SIGMATEL SND_USB_AUDIO SND_INTEL_DSP_CONFIG; do
        ./scripts/config --enable $o
    done

    # --- ファイアウォール (ufw / iptables) ---
    # defconfig には netfilter の芯しか入っていない。
    # ufw が既定で入れるルールは -m limit や -m addrtype を使うので、
    # これらが無いと "ufw enable" がルールを入れられずに黙って穴が開く。
    for o in NETFILTER_ADVANCED NF_TABLES NF_TABLES_INET NFT_CT NFT_LOG \
             NFT_LIMIT NFT_REJECT NFT_REJECT_INET NFT_COMPAT \
             NETFILTER_NETLINK NETFILTER_NETLINK_QUEUE NF_LOG_SYSLOG \
             NETFILTER_XT_MATCH_LIMIT NETFILTER_XT_MATCH_ADDRTYPE \
             NETFILTER_XT_MATCH_COMMENT NETFILTER_XT_MATCH_MULTIPORT \
             NETFILTER_XT_MATCH_RECENT NETFILTER_XT_MATCH_MAC \
             NETFILTER_XT_MATCH_IPRANGE NETFILTER_XT_MATCH_TCPMSS \
             NETFILTER_XT_MATCH_HL NETFILTER_XT_MATCH_PKTTYPE \
             NETFILTER_XT_TARGET_REJECT NETFILTER_XT_TARGET_MASQUERADE \
             NETFILTER_XT_TARGET_TCPMSS NETFILTER_XT_TARGET_NFLOG \
             IP_NF_FILTER IP_NF_MANGLE IP_NF_NAT IP_NF_TARGET_MASQUERADE \
             IP6_NF_IPTABLES IP6_NF_FILTER IP6_NF_TARGET_REJECT \
             IP6_NF_MANGLE NF_CONNTRACK_FTP NF_CONNTRACK_IRC; do
        ./scripts/config --enable $o
    done

    # --- 電源・温度・チップセット ---
    for o in ACPI ACPI_BUTTON ACPI_BATTERY ACPI_AC ACPI_THERMAL \
             CPU_FREQ CPU_FREQ_GOV_ONDEMAND CPU_FREQ_GOV_PERFORMANCE \
             X86_ACPI_CPUFREQ X86_INTEL_PSTATE X86_AMD_PSTATE \
             THERMAL THERMAL_GOV_STEP_WISE INTEL_IDLE \
             I2C I2C_I801 I2C_PIIX4 I2C_CHARDEV \
             PINCTRL_INTEL GPIOLIB HWMON SENSORS_CORETEMP SENSORS_K10TEMP \
             RTC_CLASS RTC_DRV_CMOS WATCHDOG; do
        ./scripts/config --enable $o
    done

    # --- 仮想環境の NIC ---
    # defconfig では「is not set」になっていて、=m ですらないので
    # 上の =m -> =y の変換に引っかからない。明示的に足す必要がある。
    #
    # 実際 VirtualBox の既定 (PCnet-PCI II) でネットワークが
    # 一切見えなかった。カードはあるのに動かせるドライバが無く、
    # e1000 だけが登録されていて噛み合っていなかった。
    # 「どの PC でもそのまま動く」を掲げている以上、ここは埋める。
    #
    #   PCNET32  VirtualBox の PCnet-PCI II / III (既定になることがある)
    #   VMXNET3  VMware と VirtualBox の準仮想化 NIC
    #   TULIP    DEC 21x4x。古い実機と一部の VM
    #   NE2K_PCI NE2000 互換。QEMU の -net ne2k_pci など
    for o in PCNET32 VMXNET3 TULIP DE2104X TULIP_MMIO NE2K_PCI \
             NET_VENDOR_AMD NET_VENDOR_DEC NET_VENDOR_8390 \
             HYPERV_NET VIRTIO_NET E1000 E1000E; do
        ./scripts/config --enable $o
    done

    # ファームウェアの遅延読み込み。組み込みドライバがルートより先に
    # 初期化されても、後からユーザーランド経由で読めるようにする。
    ./scripts/config --enable FW_LOADER
    ./scripts/config --enable FW_LOADER_USER_HELPER
    ./scripts/config --enable FW_LOADER_USER_HELPER_FALLBACK
fi

make olddefconfig

echo "=== ビルド (make -j$JOBS bzImage) ==="
make -j"$JOBS" bzImage

echo
echo "=== 完成 ==="
ls -l "$SRC/arch/x86/boot/bzImage"
echo "ヘッダを確認するには:"
echo "  python3 tools/bzimage_info.py $SRC/arch/x86/boot/bzImage"

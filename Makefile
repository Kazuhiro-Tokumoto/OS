# ============================================================================
# myOS - Makefile
#
# フロッピー (フェーズ1 / フェーズ2-A のデモ):
#   make            … build/disk.img をビルド
#   make v2         … 引き継ぎ資料 Stage2 v2 (色付き HELLO, WORLD!) でビルド
#   make run        … QEMU で起動して画面を自動検証
#   make run-keys   … キー入力を送り込んでから検証
#
# ハードディスク (フェーズ2-B: Linux カーネルを起動する):
#   make initramfs  … build/initramfs.cpio.gz を作る
#   make fake       … ローダー検証用の偽 bzImage を作る
#   make run-fake   … 偽カーネルでローダーを検証する (カーネル不要・数秒)
#   make kernel     … Linux カーネルをビルドする (数十分)
#   make linux      … 本物の bzImage を載せた build/disk_hdd.img を作る
#   make run-linux  … 本物の Linux を起動してシリアルログを取る
#
# その他:
#   make dump       … disk.img の HEX ダンプ付きでビルド
#   make disasm     … Stage1 / Stage2 を逆アセンブルして確認
#   make clean      … build/ を掃除
#
# 必要なもの: nasm, python3, gcc, cpio, qemu-system-x86
# ============================================================================
PYTHON  ?= python3
BUILD   := build
IMAGE   := $(BUILD)/disk.img
HDD     := $(BUILD)/disk_hdd.img

# Linux カーネルのビルド先。tools/build_kernel.sh の既定に合わせてある
KDIR    ?= $(HOME)/kernelbuild
KVER    ?= 6.12.9
BZIMAGE ?= $(KDIR)/linux-$(KVER)/arch/x86/boot/bzImage

CMDLINE ?= console=ttyS0,115200 console=tty0 earlyprintk=serial,ttyS0,115200 rdinit=/init

.PHONY: all v2 run run-keys dump disasm clean \
        initramfs fake run-fake kernel linux run-linux

all:
	$(PYTHON) tools/build_image.py

v2:
	$(PYTHON) tools/build_image.py --stage2 v2

dump:
	$(PYTHON) tools/build_image.py --dump

run: all
	$(PYTHON) tools/run_qemu.py

# フェーズ1のキーボードエコーを叩いてから ESC でプロテクトモードへ進める
run-keys: all
	$(PYTHON) tools/run_qemu.py --keys "myos,spc,test,ret,abc,bs,esc" --post-wait 2

# --- フェーズ2-B ------------------------------------------------------------

initramfs:
	sh tools/make_initramfs.sh

fake:
	$(PYTHON) tools/make_fake_kernel.py

# 本物のカーネルを用意しなくてもローダーを検証できる。
# 偽カーネルが boot_params の中身を自分で検査して画面に出す。
run-fake: initramfs fake
	$(PYTHON) tools/build_image.py --kernel $(BUILD)/fake_bzImage \
		--initrd $(BUILD)/initramfs.cpio.gz \
		--cmdline "myos fake kernel test"
	$(PYTHON) tools/run_qemu.py --image $(HDD) --media hdd --wait 5 \
		--png $(BUILD)/screen_fake.png

kernel:
	sh tools/build_kernel.sh $(KDIR) $(KVER)

linux: initramfs
	$(PYTHON) tools/build_image.py --kernel $(BZIMAGE) \
		--initrd $(BUILD)/initramfs.cpio.gz --cmdline "$(CMDLINE)"

# 注意: 64bit カーネルを動かすので qemu-system-i386 ではなく x86_64 を使う。
# qemu-system-i386 の既定 CPU は long mode 非対応で、カーネルが無反応になる。
run-linux: linux
	$(PYTHON) tools/run_qemu.py --image $(HDD) --media hdd --mem 512 \
		--qemu qemu-system-x86_64 --wait 35 \
		--serial $(BUILD)/serial.log --png $(BUILD)/screen_linux.png

# --- その他 -----------------------------------------------------------------

disasm: all
	@echo "=== Stage1 (16bit, ORG 0x7C00) ==="
	@objdump -D -b binary -m i8086 -M intel --adjust-vma=0x7C00 $(BUILD)/stage1.bin
	@echo
	@echo "=== Stage2 (16bit, ORG 0x7E00) ==="
	@objdump -D -b binary -m i8086 -M intel --adjust-vma=0x7E00 $(BUILD)/stage2.bin

clean:
	rm -rf $(BUILD)

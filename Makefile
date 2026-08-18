# ============================================================================
# myOS - Makefile
#
# 起動に必要なものは全部 build/myos.img 1 個に入る。
# ブートローダーとカーネルとルートファイルシステムを別のメディアに分けない。
# そのまま USB メモリに dd すれば起動する。
#
#   make image      … build/myos.img を作る (カーネル + ext4 ルート)
#   make run        … QEMU で起動して画面とシリアルログを取る
#
#   make kernel     … Linux カーネルをビルドする (数十分)
#   make java-demo  … Java のデモアプリ (build/hello.jar) を作る
#   make rootfs     … Debian + Xorg + Firefox + Java のルートを作る
#                     (数十分。ネットワークが要る)
#
# GUI だけを速く試す:
#   make run-wm     … Xvfb 上で WM とファイルマネージャを動かして画面を撮る
#
# ブートローダー単体の検証:
#   make demo       … フェーズ1 + 2-A のデモを載せたイメージ
#   make run-demo   … それを起動してキー入力まで検証
#   make run-fake   … 偽カーネルでローダーだけを数秒で検証 (カーネル不要)
#
# その他:
#   make disasm     … Stage1 / Stage2 を逆アセンブルして確認
#   make font       … 8x8 フォントを生成し直す (生成物はコミット済み)
#   make clean      … build/ を掃除 (rootfs のキャッシュは消さない)
#
# 必要なもの: nasm, python3, gcc, cpio, qemu-system-x86,
#             debootstrap, e2fsprogs (rootfs を作る場合)
# ============================================================================
PYTHON  ?= python3
BUILD   := build
IMAGE   := $(BUILD)/myos.img

# Linux カーネルのビルド先
# 既定はリポジトリの隣。$(HOME) だと sudo などで実行者が変わったときにずれる。
KDIR    ?= $(CURDIR)/../kernelbuild
KVER    ?= 6.12.9
BZIMAGE ?= $(KDIR)/linux-$(KVER)/arch/x86/boot/bzImage

# ルートファイルシステム
RFSDIR  ?= $(CURDIR)/../rootfs
RFSIMG  := $(BUILD)/rootfs.ext4
RFSSIZE ?= 4G

# QEMU。64bit カーネルを動かすので i386 版ではなく x86_64 版を使う。
# qemu-system-i386 の既定 CPU は long mode 非対応で、カーネルが無反応になる。
QEMU    ?= qemu-system-x86_64
MEM     ?= 2048

.PHONY: image run demo run-demo kernel rootfs rootfs-img initramfs fake \
        run-fake disasm font clean distclean java-demo run-wm

# --- 本番イメージ -----------------------------------------------------------

image: $(RFSIMG)
	$(PYTHON) tools/build_image.py --kernel $(BZIMAGE) --rootfs $(RFSIMG)

run: image
	$(PYTHON) tools/run_qemu.py --image $(IMAGE) --media hdd --mem $(MEM) \
		--qemu $(QEMU) --wait 90 \
		--serial $(BUILD)/serial.log --png $(BUILD)/screen.png

# --- 材料 -------------------------------------------------------------------

kernel:
	sh tools/build_kernel.sh $(KDIR) $(KVER)

rootfs:
	sh tools/build_rootfs.sh $(RFSDIR)

# ext4 イメージは 2GB あって作り直しに時間がかかるので、
# 無いときだけ作る。rootfs を更新したら make rootfs-img で作り直すこと。
$(RFSIMG):
	sh tools/make_rootfs_img.sh $(RFSDIR) $(RFSIMG) $(RFSSIZE)

rootfs-img:
	rm -f $(RFSIMG)
	$(MAKE) $(RFSIMG)

initramfs:
	sh tools/make_initramfs.sh

# Java のデモアプリ。バイトコードは可搬なのでホスト側でコンパイルし、
# rootfs には JRE だけ入れて JDK は入れない。
# --release を必ず指定すること。ホストの JDK が rootfs の JRE より新しいと
# クラスファイルのバージョンが上がりすぎて UnsupportedClassVersionError になる。
JAVA_RELEASE ?= 17

java-demo:
	mkdir -p $(BUILD)/javaclasses
	javac --release $(JAVA_RELEASE) -d $(BUILD)/javaclasses \
		src/java/MyOsHello.java
	jar --create --file $(BUILD)/hello.jar --main-class MyOsHello \
		-C $(BUILD)/javaclasses .

# GUI をディスクイメージ抜きで試す (Xvfb 上で動かして画面を撮る)
run-wm:
	sh tools/run_wm.sh $(BUILD)/wm.png myos-files

# --- ブートローダー単体の検証 -----------------------------------------------

demo:
	$(PYTHON) tools/build_image.py --stage2 demo

run-demo: demo
	$(PYTHON) tools/run_qemu.py --image $(IMAGE) --media hdd \
		--keys "myos,spc,test,ret,abc,bs,esc" --post-wait 2 \
		--png $(BUILD)/screen_demo.png

fake:
	$(PYTHON) tools/make_fake_kernel.py

# 本物のカーネルを用意しなくてもローダーを検証できる。
# 偽カーネルが boot_params の中身を自分で検査して画面に出す。
run-fake: initramfs fake
	$(PYTHON) tools/build_image.py --kernel $(BUILD)/fake_bzImage \
		--initrd $(BUILD)/initramfs.cpio.gz \
		--cmdline "myos fake kernel test"
	$(PYTHON) tools/run_qemu.py --image $(IMAGE) --media hdd --wait 5 \
		--png $(BUILD)/screen_fake.png

# --- その他 -----------------------------------------------------------------

disasm:
	@nasm -f bin -I src/boot/ src/boot/stage1.asm -o $(BUILD)/stage1.bin
	@nasm -f bin -I src/boot/ src/boot/stage2_linux.asm -o $(BUILD)/stage2_linux.bin
	@echo "=== Stage1 (16bit, ORG 0x7C00) ==="
	@objdump -D -b binary -m i8086 -M intel --adjust-vma=0x7C00 $(BUILD)/stage1.bin
	@echo
	@echo "=== Stage2 (16bit, ORG 0x7E00) ==="
	@objdump -D -b binary -m i8086 -M intel --adjust-vma=0x7E00 $(BUILD)/stage2_linux.bin

font:
	$(PYTHON) tools/make_font.py $(KDIR)/linux-$(KVER)/lib/fonts/font_8x8.c

clean:
	rm -rf $(BUILD)

# rootfs のキャッシュまで消す。debootstrap をやり直すことになる。
distclean: clean
	rm -rf $(RFSDIR)

# ============================================================================
# myOS - Makefile
#
#   make            … disk.img をビルド (フェーズ1 + フェーズ2-A の Stage2)
#   make v2         … 引き継ぎ資料 Stage2 v2 (色付き HELLO, WORLD!) でビルド
#   make run        … QEMU で起動して画面を自動検証
#   make run-keys   … キー入力を送り込んでから検証
#   make dump       … disk.img の HEX ダンプ付きでビルド
#   make disasm     … Stage1 / Stage2 を逆アセンブルして確認
#   make clean      … build/ を掃除
#
# 必要なもの: nasm, python3, (検証したいなら) qemu-system-i386
# ============================================================================
PYTHON  ?= python3
BUILD   := build
IMAGE   := $(BUILD)/disk.img

.PHONY: all v2 run run-keys dump disasm clean

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

disasm: all
	@echo "=== Stage1 (16bit, ORG 0x7C00) ==="
	@objdump -D -b binary -m i8086 -M intel --adjust-vma=0x7C00 $(BUILD)/stage1.bin
	@echo
	@echo "=== Stage2 (16bit, ORG 0x7E00) ==="
	@objdump -D -b binary -m i8086 -M intel --adjust-vma=0x7E00 $(BUILD)/stage2.bin

clean:
	rm -rf $(BUILD)

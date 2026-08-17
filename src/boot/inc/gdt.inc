; ============================================================================
; gdt.inc  -  GDT (グローバルディスクリプタテーブル)
;
; フラットモデル: コード/データとも base=0, limit=4GB。
;
; セレクタの配置は Linux の 32bit Boot Protocol に合わせてある。
; カーネルに制御を渡すときは
;   「__BOOT_CS(0x10) と __BOOT_DS(0x18) のディスクリプタを積んだ GDT を
;     ロードし、CS=__BOOT_CS、DS/ES/SS=__BOOT_DS にしておくこと」
; と規定されている (Documentation/arch/x86/boot.rst の 32-bit BOOT PROTOCOL)。
; 教科書的に 0x08/0x10 に置くとカーネルに渡せず組み直しになるので、
; 最初から 0x10/0x18 に置き、インデックス1 (0x08) は空けてある。
; ============================================================================

GDT_CODE32 equ 0x10             ; = Linux の __BOOT_CS
GDT_DATA32 equ 0x18             ; = Linux の __BOOT_DS

align 8
gdt_start:
        ; 0x00: ヌルディスクリプタ (必須)
        dq 0x0000000000000000

        ; 0x08: 未使用 (__BOOT_CS を 0x10 に合わせるための詰め物)
        dq 0x0000000000000000

        ; 0x10: 32bit コードセグメント (__BOOT_CS)
        dw 0xFFFF               ; limit 15:0
        dw 0x0000               ; base 15:0
        db 0x00                 ; base 23:16
        db 0x9A                 ; access: P=1 DPL=0 S=1 type=1010 (code, read)
        db 0xCF                 ; G=1 D=1 L=0 AVL=0 | limit 19:16 = F
        db 0x00                 ; base 31:24

        ; 0x18: 32bit データセグメント (__BOOT_DS)
        dw 0xFFFF
        dw 0x0000
        db 0x00
        db 0x92                 ; access: type=0010 (data, write)
        db 0xCF
        db 0x00
gdt_end:

gdt_desc:
        dw gdt_end - gdt_start - 1      ; リミット (サイズ-1)
        dd gdt_start                    ; 線形アドレス (セグメント0なのでそのまま)

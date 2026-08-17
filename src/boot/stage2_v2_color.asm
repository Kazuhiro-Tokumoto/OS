; ============================================================================
; stage2_v2_color.asm  -  Stage2 v2 (色付き文字列表示)
;
; 引き継ぎ資料 2-3節 で「手動バイトアセンブル」された 47 バイトを、
; そのまま再現するためのソース。
; nasm の出力が資料の16進ダンプと1バイトも違わないことを
; tools/verify_stage2_v2.py で検証している。
;
; VRAM (0xB8000) に直接書き込む方式。1文字 = 2バイト (文字コード, 属性)。
; 属性 0x1A = 背景:青(1) / 文字:明るい緑(A)
; ============================================================================
BITS 16
ORG 0x7E00

start:
        mov     ax, 0xB800          ; B8 00 B8   : VRAM セグメント
        mov     es, ax              ; 8E C0
        mov     di, 0               ; BF 00 00   : 画面左上 (0,0)
        mov     si, msg             ; BE 21 7E

print_loop:
        mov     al, [si]            ; 8A 04
        cmp     al, 0               ; 3C 00
        je      halt                ; 74 0E

        mov     [es:di], al         ; 26 88 05   : 文字コードを書き込み
        mov     byte [es:di+1], 0x1A; 26 C6 45 01 1A : 属性を書き込み

        add     di, 2               ; 83 C7 02   : 次の文字位置 (1文字=2バイト)
        inc     si                  ; 46
        jmp     print_loop          ; EB EC

halt:
        jmp     halt                ; EB FE

msg:    db      'HELLO, WORLD!', 0

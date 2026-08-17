; ============================================================================
; fake_kernel.asm  -  ローダー検証用の「偽カーネル」
;
; 本物の bzImage を用意しなくても stage2_linux.asm を検証できるようにするための
; スタブ。Linux の 32bit Boot Protocol で渡されたものが規約どおりかを
; 自分で検査し、結果を VRAM に直接書き出して停止する。
;
; tools/make_fake_kernel.py が、この生バイナリの前に bzImage 互換の
; setup ヘッダ (setup_sects=4 の 2560 バイト) を付けて 1 ファイルにする。
; つまりローダーから見ると本物の bzImage と同じに見える。
;
; 検査する内容 (Documentation/arch/x86/boot.rst の規約):
;   - CS/DS/ES/SS が 4GB フラットで動いていること (ここに来られた時点で OK)
;   - EBX / EBP / EDI が 0 であること
;   - ESI が boot_params を指していること ('HdrS' マジックで確認)
;   - boot_params の type_of_loader / loadflags / code32_start
;   - e820_entries が 0 でないこと
;   - cmd_line_ptr の先の文字列
;   - ramdisk_image / ramdisk_size
; ============================================================================
BITS 32
ORG 0x00100000

VRAM            equ 0x000B8000
COLS            equ 80
ATTR_OK         equ 0x0A        ; 明るい緑
ATTR_ERR        equ 0x0C        ; 明るい赤
ATTR_INFO       equ 0x0F        ; 白

entry:
        ; 渡されたレジスタを真っ先に保存する
        mov     [saved_esi], esi
        mov     [saved_ebx], ebx
        mov     [saved_ebp], ebp
        mov     [saved_edi], edi

        mov     esp, 0x0008F000

        mov     dword [row], 10

        mov     esi, msg_banner
        mov     ah, ATTR_INFO
        call    puts_line

        ; --- EBX / EBP / EDI が 0 か ---
        mov     esi, msg_regs
        call    puts_at
        mov     eax, [saved_ebx]
        or      eax, [saved_ebp]
        or      eax, [saved_edi]
        test    eax, eax
        jnz     .regs_bad
        mov     esi, str_ok
        mov     ah, ATTR_OK
        call    puts_eol
        jmp     .check_esi
.regs_bad:
        mov     esi, str_ng
        mov     ah, ATTR_ERR
        call    puts_eol

        ; --- ESI が boot_params を指しているか ---
.check_esi:
        mov     esi, msg_esi
        call    puts_at
        mov     eax, [saved_esi]
        call    puthex32
        mov     esi, msg_hdrs
        call    puts_raw
        mov     ebx, [saved_esi]
        cmp     dword [ebx + 0x202], 0x53726448  ; 'HdrS'
        jne     .esi_bad
        mov     esi, str_ok
        mov     ah, ATTR_OK
        call    puts_eol
        jmp     .check_loader
.esi_bad:
        mov     esi, str_ng
        mov     ah, ATTR_ERR
        call    puts_eol
        jmp     hang

        ; --- type_of_loader / loadflags / code32_start ---
.check_loader:
        mov     esi, msg_loader
        call    puts_at
        movzx   eax, byte [ebx + 0x210]
        call    puthex8
        mov     esi, msg_loadflags
        call    puts_raw
        movzx   eax, byte [ebx + 0x211]
        call    puthex8
        mov     esi, msg_code32
        call    puts_raw
        mov     eax, [ebx + 0x214]
        call    puthex32
        call    eol

        ; --- e820 エントリ数 ---
        mov     esi, msg_e820
        call    puts_at
        movzx   eax, byte [ebx + 0x1E8]
        call    putdec32
        mov     esi, msg_entries
        call    puts_raw
        call    eol

        ; --- e820 の先頭 2 エントリを出す ---
        movzx   ecx, byte [ebx + 0x1E8]
        cmp     ecx, 2
        jbe     .have_cnt
        mov     ecx, 2
.have_cnt:
        lea     edx, [ebx + 0x2D0]
.e820_loop:
        test    ecx, ecx
        jz      .e820_done
        push    ecx
        push    edx
        mov     esi, msg_e820_ent
        call    puts_at
        pop     edx
        push    edx
        mov     eax, [edx]              ; base 下位 32bit
        call    puthex32
        mov     esi, msg_plus
        call    puts_raw
        pop     edx
        push    edx
        mov     eax, [edx + 8]          ; length 下位 32bit
        call    puthex32
        mov     esi, msg_type
        call    puts_raw
        pop     edx
        push    edx
        mov     eax, [edx + 16]         ; type
        call    putdec32
        call    eol
        pop     edx
        add     edx, 20
        pop     ecx
        dec     ecx
        jmp     .e820_loop
.e820_done:

        ; --- ramdisk ---
        mov     esi, msg_ramdisk
        call    puts_at
        mov     eax, [ebx + 0x218]
        call    puthex32
        mov     esi, msg_size
        call    puts_raw
        mov     eax, [ebx + 0x21C]
        call    puthex32
        call    eol

        ; --- コマンドライン ---
        mov     esi, msg_cmdline
        call    puts_at
        mov     esi, [ebx + 0x228]
        mov     ah, ATTR_OK
        call    puts_eol

        mov     esi, msg_done
        mov     ah, ATTR_OK
        call    puts_line

hang:
        hlt
        jmp     hang

; ---------------------------------------------------------------------------
; 画面出力ヘルパー
;   row      … 現在の行
;   col      … 現在の桁
; ---------------------------------------------------------------------------

; calc_edi - EDI = VRAM + (row*80 + col)*2
calc_edi:
        push    eax
        push    edx
        mov     eax, [row]
        mov     edx, COLS
        mul     edx
        add     eax, [col]
        shl     eax, 1
        add     eax, VRAM
        mov     edi, eax
        pop     edx
        pop     eax
        ret

; putc - AL の文字を AH の属性で出す
putc:
        push    edi
        push    eax
        call    calc_edi
        pop     eax
        mov     [edi], ax
        inc     dword [col]
        pop     edi
        ret

; puts_raw - ESI の 0 終端文字列を、属性 AH のまま出す
puts_raw:
        push    eax
        push    esi
        mov     ah, ATTR_INFO
.loop:
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     esi
        pop     eax
        ret

; puts_at - 行頭から ESI を出す (次の出力は同じ行の続きから)
puts_at:
        mov     dword [col], 2
        call    puts_raw
        ret

; puts_eol - ESI を属性 AH で出してから改行
puts_eol:
        push    eax
        push    esi
.loop:
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     esi
        pop     eax
        call    eol
        ret

; puts_line - 行頭から ESI を属性 AH で出して改行
puts_line:
        mov     dword [col], 2
        call    puts_eol
        ret

; eol - 改行
eol:
        inc     dword [row]
        mov     dword [col], 2
        ret

; puthex8 / puthex32
puthex8:
        push    eax
        push    ecx
        mov     ecx, eax
        shr     eax, 4
        and     eax, 0x0F
        call    .nib
        mov     eax, ecx
        and     eax, 0x0F
        call    .nib
        pop     ecx
        pop     eax
        ret
.nib:
        cmp     al, 10
        jb      .dig
        add     al, 'A' - 10
        jmp     .out
.dig:
        add     al, '0'
.out:
        mov     ah, ATTR_INFO
        call    putc
        ret

puthex32:
        push    eax
        push    ecx
        mov     ecx, 8
        mov     [hexval], eax
.loop:
        mov     eax, [hexval]
        rol     eax, 4
        mov     [hexval], eax
        and     eax, 0x0F
        cmp     al, 10
        jb      .dig
        add     al, 'A' - 10
        jmp     .out
.dig:
        add     al, '0'
.out:
        mov     ah, ATTR_INFO
        call    putc
        dec     ecx
        jnz     .loop
        pop     ecx
        pop     eax
        ret

putdec32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     ecx, ecx
.div:
        xor     edx, edx
        div     ebx
        push    edx
        inc     ecx
        test    eax, eax
        jnz     .div
.out:
        pop     eax
        add     al, '0'
        mov     ah, ATTR_INFO
        call    putc
        dec     ecx
        jnz     .out
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; --- データ ----------------------------------------------------------------
msg_banner:     db '=== FAKE KERNEL ENTERED at 0x100000 (32-bit boot protocol) ===', 0
msg_regs:       db 'EBX/EBP/EDI are zero      : ', 0
msg_esi:        db 'ESI (boot_params)         : 0x', 0
msg_hdrs:       db "  magic 'HdrS' : ", 0
msg_loader:     db 'type_of_loader            : 0x', 0
msg_loadflags:  db '  loadflags : 0x', 0
msg_code32:     db '  code32_start : 0x', 0
msg_e820:       db 'e820_entries              : ', 0
msg_entries:    db ' entries', 0
msg_e820_ent:   db '  e820[]  base 0x', 0
msg_plus:       db '  len 0x', 0
msg_type:       db '  type ', 0
msg_ramdisk:    db 'ramdisk_image             : 0x', 0
msg_size:       db '  ramdisk_size : 0x', 0
msg_cmdline:    db 'cmd_line_ptr -> ', 0
msg_done:       db '=== boot_params verified. Loader works. ===', 0
str_ok:         db 'OK', 0
str_ng:         db 'MISMATCH', 0

align 4
row:            dd 0
col:            dd 2
hexval:         dd 0
saved_esi:      dd 0
saved_ebx:      dd 0
saved_ebp:      dd 0
saved_edi:      dd 0

; ============================================================================
; memmap.inc  -  BIOS からのメモリマップ取得
;
; 互換性方針に従い 3 段階でフォールバックする。
;   1) INT 15h EAX=E820h  … 詳細なメモリマップ。Linux Boot Protocol でも必須
;   2) INT 15h AX=E801h   … E820 非対応の古い BIOS 向け
;   3) INT 15h AH=88h     … 最も枯れている。64MB で頭打ちになる
;
; E820 の生エントリ (1 個 24 バイト) は E820_BUF に貯める。
; フェーズ2-B ではこれを 20 バイトずつに詰め直してゼロページへ移す。
; ============================================================================

E820_BUF        equ 0x0500      ; 0x0500-0x7BFF は BIOS 予約後の空き低位メモリ
E820_MAX        equ 32          ; 保持する最大エントリ数
E820_ENTSZ      equ 24          ; 取得時のエントリサイズ (ACPI3.0 拡張属性込み)

; ---------------------------------------------------------------------------
; detect_memory - 使える方式を順に試す
;   出力: CF=0 成功 / CF=1 全滅
;         [mem_method] 1=E820, 2=E801, 3=88h
;         [mem_mb]     使用可能メモリ量 (MB)
;         [e820_count] E820 が使えた場合のエントリ数
; ---------------------------------------------------------------------------
detect_memory:
        call    do_e820
        jc      .try_e801
        mov     byte [mem_method], 1
        clc
        ret
.try_e801:
        call    do_e801
        jc      .try_88
        mov     byte [mem_method], 2
        clc
        ret
.try_88:
        call    do_88
        jc      .failed
        mov     byte [mem_method], 3
        clc
        ret
.failed:
        mov     byte [mem_method], 0
        stc
        ret

; ---------------------------------------------------------------------------
; do_e820 - INT 15h EAX=E820h でメモリマップを取得し E820_BUF に格納
;   成功: CF=0, [e820_count] にエントリ数, [mem_mb] に type1 の合計 (MB)
;   失敗: CF=1
; ---------------------------------------------------------------------------
do_e820:
        push    ds
        pop     es
        mov     di, E820_BUF
        xor     ebx, ebx
        xor     bp, bp                  ; エントリ数カウンタ
        mov     dword [total_lo], 0
        mov     dword [total_hi], 0

        mov     edx, 0x534D4150         ; 'SMAP'
        mov     eax, 0xE820
        mov     dword [es:di + 20], 1   ; ACPI3.0 拡張属性のデフォルト値
        mov     ecx, E820_ENTSZ
        int     0x15
        jc      .fail                   ; 1 回目で CF=1 なら未対応
        cmp     eax, 0x534D4150
        jne     .fail
        test    ebx, ebx                ; EBX=0 ならエントリが 1 つも無い
        jz      .fail
        jmp     .process

.next:
        mov     edx, 0x534D4150
        mov     eax, 0xE820
        mov     dword [es:di + 20], 1
        mov     ecx, E820_ENTSZ
        int     0x15
        jc      .done                   ; CF=1 は「これで最後」の合図
        cmp     eax, 0x534D4150
        jne     .done

.process:
        jcxz    .skip                   ; 長さ 0 のエントリは無視
        cmp     cl, 20
        jbe     .no_acpi_check
        test    byte [es:di + 20], 1    ; ACPI3.0: bit0=0 なら無視すべき領域
        jz      .skip
.no_acpi_check:
        mov     eax, [es:di + 8]        ; 長さ下位32bit
        or      eax, [es:di + 12]       ; 上位も含めて 0 かどうか
        jz      .skip

        cmp     dword [es:di + 16], 1   ; type == 1 (使用可能 RAM) か
        jne     .count_only

        mov     eax, [es:di + 8]
        mov     edx, [es:di + 12]
        add     [total_lo], eax
        adc     [total_hi], edx

.count_only:
        inc     bp
        add     di, E820_ENTSZ
.skip:
        cmp     bp, E820_MAX
        jae     .done
        test    ebx, ebx                ; EBX=0 でリスト終端
        jz      .done
        jmp     .next

.done:
        test    bp, bp
        jz      .fail
        mov     [e820_count], bp
        call    total_to_mb
        clc
        ret
.fail:
        mov     word [e820_count], 0
        stc
        ret

; ---------------------------------------------------------------------------
; total_to_mb - [total_hi:total_lo] バイトを MB に直して [mem_mb] へ
;               mem_mb = (total_hi << 12) + (total_lo >> 20)
; ---------------------------------------------------------------------------
total_to_mb:
        push    eax
        push    edx
        mov     eax, [total_lo]
        shr     eax, 20
        mov     edx, [total_hi]
        shl     edx, 12
        add     eax, edx
        mov     [mem_mb], ax
        pop     edx
        pop     eax
        ret

; ---------------------------------------------------------------------------
; do_e801 - INT 15h AX=E801h
;   CX/AX = 1MB-16MB の KB 数, DX/BX = 16MB 以降の 64KB ブロック数
; ---------------------------------------------------------------------------
do_e801:
        push    bx
        push    cx
        push    dx
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0xE801
        int     0x15
        jc      .fail
        cmp     ah, 0x86                ; 未対応
        je      .fail
        cmp     ah, 0x80                ; 無効なコマンド
        je      .fail

        test    cx, cx                  ; CX/DX が 0 なら AX/BX 側を使う
        jnz     .use_cx
        mov     cx, ax
        mov     dx, bx
.use_cx:
        ; MB = 1 (先頭 1MB) + CX/1024 + DX/16
        movzx   eax, cx
        shr     eax, 10
        movzx   ebx, dx
        shr     ebx, 4
        add     eax, ebx
        inc     eax
        mov     [mem_mb], ax
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.fail:
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

; ---------------------------------------------------------------------------
; do_88 - INT 15h AH=88h (最も枯れた方式。1MB 超の KB 数だが 64MB 頭打ち)
; ---------------------------------------------------------------------------
do_88:
        push    bx
        push    cx
        push    dx
        xor     ax, ax
        mov     ah, 0x88
        int     0x15
        jc      .fail
        test    ax, ax
        jz      .fail
        shr     ax, 10                  ; KB → MB
        inc     ax                      ; 先頭 1MB を足す
        mov     [mem_mb], ax
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.fail:
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

; --- 変数 ------------------------------------------------------------------
mem_method:     db 0
e820_count:     dw 0
mem_mb:         dw 0
total_lo:       dd 0
total_hi:       dd 0

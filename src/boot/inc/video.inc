; ============================================================================
; video.inc  -  テキストモード (80x25, 16色) の画面出力ルーチン
;
; リアルモード専用。VRAM 0xB8000 へ直接書き込み、ハードウェアカーソルだけ
; INT 10h AH=02h で同期する。
; 1文字 = 2バイト (文字コード, 属性)。属性は上位4bit=背景色, 下位4bit=文字色。
;
; 提供するもの:
;   cls              画面全体を [cur_attr] で塗りつぶし、カーソルを(0,0)へ
;   putc             AL の1文字を表示 (0x0D/0x0A=改行, 0x08=BS)
;   puts             DS:SI の0終端文字列を表示
;   newline          改行
;   puthex8/16/32    16進表示
;   putdec16/32      10進表示
;   sync_hw_cursor   ソフトカーソル位置をHWカーソルへ反映
;   scroll_up        1行スクロール (0行目のタイトルバーは固定)
;
; 変数: cur_row / cur_col / cur_attr
; ============================================================================

VRAM_SEG        equ 0xB800
SCR_COLS        equ 80
SCR_ROWS        equ 25

; 属性バイト
ATTR_TITLE      equ 0x1F        ; 背景=青,  文字=白 (Win98 のタイトルバー風)
ATTR_NORMAL     equ 0x07        ; 背景=黒,  文字=灰
ATTR_OK         equ 0x0A        ; 背景=黒,  文字=明るい緑
ATTR_WARN       equ 0x0E        ; 背景=黒,  文字=黄
ATTR_ERR        equ 0x0C        ; 背景=黒,  文字=明るい赤
ATTR_INPUT      equ 0x0B        ; 背景=黒,  文字=明るいシアン

; ---------------------------------------------------------------------------
; cls - 画面全体を空白 + [cur_attr] で塗りつぶし、カーソルを (0,0) へ
; ---------------------------------------------------------------------------
cls:
        push    ax
        push    cx
        push    di
        push    es

        mov     ax, VRAM_SEG
        mov     es, ax
        xor     di, di
        mov     ah, [cur_attr]
        mov     al, ' '
        mov     cx, SCR_COLS * SCR_ROWS
        rep     stosw                   ; AX を CX 回書き込む (文字+属性)

        mov     byte [cur_row], 0
        mov     byte [cur_col], 0
        call    sync_hw_cursor

        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; draw_title_bar - 0 行目を DS:SI の文字列で Win98 のタイトルバー風に塗る
; ---------------------------------------------------------------------------
draw_title_bar:
        push    ax
        push    cx
        push    di
        push    es
        push    si

        mov     ax, VRAM_SEG
        mov     es, ax
        xor     di, di
        mov     ah, ATTR_TITLE
        mov     al, ' '
        mov     cx, SCR_COLS
        rep     stosw

        mov     di, 2                   ; 列1 から書き始める
.tb_loop:
        lodsb
        test    al, al
        jz      .tb_done
        mov     ah, ATTR_TITLE
        stosw
        jmp     .tb_loop
.tb_done:
        pop     si
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; calc_vram_di - 現在のカーソル位置から VRAM オフセットを DI に求める
;   DI = (cur_row * 80 + cur_col) * 2
; ---------------------------------------------------------------------------
calc_vram_di:
        push    ax
        push    bx
        movzx   ax, byte [cur_row]
        mov     bx, SCR_COLS
        mul     bx                      ; DX:AX = row * 80
        movzx   bx, byte [cur_col]
        add     ax, bx
        shl     ax, 1                   ; 1文字 = 2バイト
        mov     di, ax
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; putc - AL の 1 文字を現在のカーソル位置に [cur_attr] で表示し、カーソル前進
; ---------------------------------------------------------------------------
putc:
        push    ax
        push    di
        push    es
        push    bx

        cmp     al, 0x0D
        je      .cr
        cmp     al, 0x0A
        je      .lf
        cmp     al, 0x08
        je      .bs

        mov     bl, al                  ; 文字を退避
        call    calc_vram_di
        mov     ax, VRAM_SEG
        mov     es, ax
        mov     al, bl
        mov     ah, [cur_attr]
        mov     [es:di], ax

        inc     byte [cur_col]
        cmp     byte [cur_col], SCR_COLS
        jb      .done
        mov     byte [cur_col], 0
        jmp     .next_row

.cr:
        mov     byte [cur_col], 0
        jmp     .done
.lf:
        jmp     .next_row

.bs:
        cmp     byte [cur_col], 0
        je      .done
        dec     byte [cur_col]
        call    calc_vram_di
        mov     ax, VRAM_SEG
        mov     es, ax
        mov     al, ' '
        mov     ah, [cur_attr]
        mov     [es:di], ax
        jmp     .done

.next_row:
        inc     byte [cur_row]
        cmp     byte [cur_row], SCR_ROWS
        jb      .done
        call    scroll_up
        mov     byte [cur_row], SCR_ROWS - 1

.done:
        call    sync_hw_cursor
        pop     bx
        pop     es
        pop     di
        pop     ax
        ret

; ---------------------------------------------------------------------------
; scroll_up - 1〜24行目を 1 行上へスクロールする。
;             0 行目はタイトルバーとして固定表示なので動かさない。
; ---------------------------------------------------------------------------
scroll_up:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    es

        mov     ax, VRAM_SEG
        mov     es, ax
        mov     ds, ax
        mov     si, SCR_COLS * 2 * 2    ; 2 行目の先頭
        mov     di, SCR_COLS * 2        ; 1 行目の先頭
        mov     cx, SCR_COLS * (SCR_ROWS - 2)
        rep     movsw

        mov     di, SCR_COLS * (SCR_ROWS - 1) * 2
        xor     ax, ax
        mov     ds, ax                  ; 変数を読むので DS を 0 に戻す
        mov     ah, [cur_attr]
        mov     al, ' '
        mov     cx, SCR_COLS
        rep     stosw

        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; sync_hw_cursor - ソフトカーソル位置をハードウェアカーソルへ反映
;                  (INT 10h AH=02h。どの BIOS でも実装されている枯れた機能)
; ---------------------------------------------------------------------------
sync_hw_cursor:
        push    ax
        push    bx
        push    dx
        mov     ah, 0x02
        xor     bh, bh                  ; ページ 0
        mov     dh, [cur_row]
        mov     dl, [cur_col]
        int     0x10
        pop     dx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; puts - DS:SI の 0 終端文字列を表示
; ---------------------------------------------------------------------------
puts:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

; ---------------------------------------------------------------------------
; puts_attr - AH の属性に切り替えてから DS:SI を表示する
; ---------------------------------------------------------------------------
puts_attr:
        mov     [cur_attr], ah
        call    puts
        ret

; ---------------------------------------------------------------------------
; newline - 改行
; ---------------------------------------------------------------------------
newline:
        push    ax
        mov     al, 0x0D
        call    putc
        mov     al, 0x0A
        call    putc
        pop     ax
        ret

; ---------------------------------------------------------------------------
; puthex8 / puthex16 / puthex32 - 16 進表示
; ---------------------------------------------------------------------------
puthex8:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .nib
        mov     al, cl
        and     al, 0x0F
        call    .nib
        pop     cx
        pop     ax
        ret
.nib:
        cmp     al, 10
        jb      .digit
        add     al, 'A' - 10
        jmp     .out
.digit:
        add     al, '0'
.out:
        call    putc
        ret

puthex16:
        push    ax
        push    bx
        mov     bx, ax
        mov     al, bh
        call    puthex8
        mov     al, bl
        call    puthex8
        pop     bx
        pop     ax
        ret

puthex32:
        push    eax
        push    eax
        shr     eax, 16
        call    puthex16
        pop     eax
        call    puthex16
        pop     eax
        ret

; ---------------------------------------------------------------------------
; putdec16 - AX を 10 進で表示 (先頭 0 は詰める)
; ---------------------------------------------------------------------------
putdec16:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
.div_loop:
        xor     dx, dx
        div     bx                      ; DX:AX / 10 → AX=商, DX=余り
        push    dx
        inc     cx
        test    ax, ax
        jnz     .div_loop
.out_loop:
        pop     ax
        add     al, '0'
        call    putc
        loop    .out_loop
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; putdec32 - EAX を 10 進で表示
; ---------------------------------------------------------------------------
putdec32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     cx, cx
.div_loop:
        xor     edx, edx
        div     ebx                     ; EDX:EAX / 10
        push    dx
        inc     cx
        test    eax, eax
        jnz     .div_loop
.out_loop:
        pop     ax
        add     al, '0'
        call    putc
        loop    .out_loop
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; --- 変数 ------------------------------------------------------------------
cur_row:        db 0
cur_col:        db 0
cur_attr:       db ATTR_NORMAL

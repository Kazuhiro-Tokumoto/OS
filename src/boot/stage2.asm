; ============================================================================
; stage2.asm  -  myOS Stage2 (フェーズ1 + フェーズ2-A のデモ)
;
; ロード先: 0x0000:0x7E00 (Stage1 が 17 セクタ = 8704 バイト読み込む)
; 入力:     DL = BIOS が渡してきたブートドライブ番号 (Stage1 から中継)
;
; 実装内容:
;   [フェーズ1] 画面クリア / カーソル制御 / 色付き表示 / キーボード入力
;   [フェーズ2-A] メモリマップ取得 / A20 有効化 / GDT / プロテクトモード移行
;
; Linux カーネルを起動するのは stage2_linux.asm のほう。
; こちらは「ブートローダー単体としてどこまで動くか」を見せるデモとして残してある。
; ============================================================================
BITS 16
ORG 0x7E00

; Stage1 は 0x7E00 の先頭バイトへジャンプしてくる。
; %include したルーチン群が先に展開されるので、必ずここで本体へ飛ばすこと。
; (これを忘れると 0x7E00 が video.inc の cls の途中になり、いきなり暴走する)
        jmp     stage2_start

%include "inc/video.inc"
%include "inc/a20.inc"
%include "inc/memmap.inc"

; ============================================================================
stage2_start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti

        mov     [boot_drive], dl

        ; ビデオモードを 80x25 16色テキストに確定させる
        ; (BIOS によってはブート直後のモードが不定なため明示的に設定する)
        mov     ax, 0x0003
        int     0x10

        ; ------------------------------------------------------------
        ; フェーズ1: 画面クリア + タイトルバー描画
        ; ------------------------------------------------------------
        mov     byte [cur_attr], ATTR_NORMAL
        call    cls

        mov     si, msg_title
        call    draw_title_bar

        mov     byte [cur_row], 2
        mov     byte [cur_col], 0
        call    sync_hw_cursor

        mov     si, msg_boot_drive
        call    puts
        mov     al, [boot_drive]
        call    puthex8
        call    newline

        ; ------------------------------------------------------------
        ; フェーズ2: メモリマップ取得 (E820h → E801h → 88h)
        ; ------------------------------------------------------------
        mov     si, msg_memmap
        call    puts
        call    detect_memory
        call    show_mem_result
        call    newline

        ; ------------------------------------------------------------
        ; フェーズ1: キーボード入力エコー
        ; ------------------------------------------------------------
        call    newline
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_kbd_help
        call    puts
        call    newline
        mov     byte [cur_attr], ATTR_INPUT
        mov     si, msg_prompt
        call    puts

        call    keyboard_echo_loop

        ; ------------------------------------------------------------
        ; フェーズ2-A: プロテクトモードへ
        ; ------------------------------------------------------------
        call    newline
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_a20
        call    puts
        call    enable_a20
        jc      .a20_failed
        mov     byte [cur_attr], ATTR_OK
        mov     si, msg_ok
        call    puts
        call    newline
        jmp     .goto_pm

.a20_failed:
        mov     byte [cur_attr], ATTR_ERR
        mov     si, msg_fail
        call    puts
        call    newline
        mov     si, msg_a20_halt
        call    puts
        jmp     hang16

.goto_pm:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_pm
        call    puts

        call    enter_protected_mode    ; ここから戻ってこない

hang16:
        hlt
        jmp     hang16

; ---------------------------------------------------------------------------
; show_mem_result - detect_memory の結果を表示する
; ---------------------------------------------------------------------------
show_mem_result:
        cmp     byte [mem_method], 1
        je      .e820
        cmp     byte [mem_method], 2
        je      .e801
        cmp     byte [mem_method], 3
        je      .m88

        mov     byte [cur_attr], ATTR_ERR
        mov     si, msg_mem_fail
        call    puts
        ret
.e820:
        mov     byte [cur_attr], ATTR_OK
        mov     si, msg_via_e820
        jmp     .show
.e801:
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_via_e801
        jmp     .show
.m88:
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_via_88
.show:
        call    puts
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_usable
        call    puts
        mov     ax, [mem_mb]
        call    putdec16
        mov     si, msg_mb
        call    puts
        ret

; ---------------------------------------------------------------------------
; keyboard_echo_loop - INT 16h でキー入力を受け取り、画面にエコーする。
;                      ESC で抜ける。Enter は改行、BackSpace は 1 文字削除。
; ---------------------------------------------------------------------------
keyboard_echo_loop:
        push    ax
.loop:
        xor     ah, ah                  ; AH=00h: キー入力待ち
        int     0x16                    ; → AH=スキャンコード, AL=ASCII

        cmp     al, 0x1B                ; ESC
        je      .done
        test    al, al                  ; AL=0 は拡張キー (矢印など) → 無視
        jz      .loop

        cmp     al, 0x0D                ; Enter
        jne     .not_enter
        call    newline
        mov     byte [cur_attr], ATTR_INPUT
        mov     si, msg_prompt
        call    puts
        jmp     .loop
.not_enter:
        call    putc
        jmp     .loop
.done:
        pop     ax
        ret

; ---------------------------------------------------------------------------
; enter_protected_mode - GDT を積んで CR0.PE を立て、32bit コードへ移る
; ---------------------------------------------------------------------------
enter_protected_mode:
        cli

        ; NMI も止めておく (CMOS インデックスレジスタ bit7)
        in      al, 0x70
        or      al, 0x80
        out     0x70, al
        in      al, 0x71

        lgdt    [gdt_desc]

        mov     eax, cr0
        or      eax, 1                  ; PE = 1
        mov     cr0, eax

        ; パイプラインをフラッシュしつつ CS を 32bit コードセグメントに載せ替える
        jmp     GDT_CODE32:pm_entry

; ---------------------------------------------------------------------------
BITS 32
pm_entry:
        mov     ax, GDT_DATA32
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x00090000         ; 32bit 用スタック

        ; --- BIOS はもう使えないので VRAM (0xB8000) へ直接書き込む ---
        mov     esi, pm_msg
        mov     edi, 0x000B8000 + (SCR_COLS * 2) * 24   ; 最終行に表示
        mov     ah, ATTR_OK
.loop:
        lodsb
        test    al, al
        jz      .done
        mov     [edi], ax
        add     edi, 2
        jmp     .loop
.done:

pm_hang:
        hlt
        jmp     pm_hang

pm_msg: db '** PROTECTED MODE OK (32bit) - next: Linux Boot Protocol **', 0

; ---------------------------------------------------------------------------
BITS 16

%include "inc/gdt.inc"

; --- 文字列 ----------------------------------------------------------------
msg_title:      db 'myOS Stage2  -  Phase1: screen/keyboard   Phase2-A: protected mode', 0
msg_boot_drive: db 'Boot drive (from BIOS DL) : 0x', 0
msg_memmap:     db 'Memory map                : ', 0
msg_via_e820:   db 'INT 15h E820h ', 0
msg_via_e801:   db 'INT 15h E801h (E820 unsupported) ', 0
msg_via_88:     db 'INT 15h AH=88h (fallback) ', 0
msg_mem_fail:   db 'FAILED (no BIOS method worked)', 0
msg_usable:     db '/ usable ', 0
msg_mb:         db ' MB', 0
msg_kbd_help:   db 'Type anything (INT 16h echo). BackSpace/Enter work. Press ESC to continue.', 0
msg_prompt:     db 'C:\> ', 0
msg_a20:        db 'A20 gate                  : ', 0
msg_pm:         db 'Entering protected mode...', 0
msg_ok:         db 'ENABLED', 0
msg_fail:       db 'FAILED', 0
msg_a20_halt:   db 'Cannot continue without A20. Halted.', 0

; --- 変数 ------------------------------------------------------------------
boot_drive:     db 0

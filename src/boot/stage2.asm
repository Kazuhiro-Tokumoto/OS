; ============================================================================
; stage2.asm  -  myOS Stage2 本体
;
; ロード先: 0x0000:0x7E00 (Stage1 が 17 セクタ = 8704 バイト読み込む)
; 入力:     DL = BIOS が渡してきたブートドライブ番号 (Stage1 から中継)
;
; このファイルで実装している内容:
;   [フェーズ1] 画面表示の強化
;     - 画面クリア (VRAM 直接書き込み)
;     - 任意座標へのカーソル制御 (ソフトカーソル + INT 10h AH=02h でHW同期)
;     - 色付き文字表示 (属性バイト)
;     - キーボード入力の受付 (INT 16h) と画面へのエコー、スクロール処理
;   [フェーズ2-A] リアルモード → プロテクトモード移行
;     - INT 15h E820h によるメモリマップ取得
;       (非対応時は E801h → AH=88h と枯れた方式へフォールバック)
;     - A20 ゲート有効化 (BIOS 2401h → キーボードコントローラ → ポート0x92
;       の3方式フォールバック + 実際にラップアラウンドするか検証)
;     - GDT 構築 → CR0.PE=1 → 32bit コードへ far jump
;     - 32bit 側で VRAM に直接書き込み、移行成功を表示
;
; 互換性方針: 特定 BIOS 実装に依存しない。新しめの機能は必ず
;             「対応チェック → 非対応なら枯れた方式」の順で試す。
; ============================================================================
BITS 16
ORG 0x7E00

VRAM_SEG        equ 0xB800
SCR_COLS        equ 80
SCR_ROWS        equ 25
E820_BUF        equ 0x0500      ; 0x0500-0x7BFF は BIOS 予約後の空き低位メモリ
E820_MAX        equ 32          ; 保持する最大エントリ数

; 属性バイト: 上位4bit=背景色, 下位4bit=文字色
ATTR_TITLE      equ 0x1F        ; 背景=青,   文字=白 (Win98 のタイトルバー風)
ATTR_NORMAL     equ 0x07        ; 背景=黒,   文字=灰
ATTR_OK         equ 0x0A        ; 背景=黒,   文字=明るい緑
ATTR_WARN       equ 0x0E        ; 背景=黒,   文字=黄
ATTR_ERR        equ 0x0C        ; 背景=黒,   文字=明るい赤
ATTR_INPUT      equ 0x0B        ; 背景=黒,   文字=明るいシアン

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

        ; --- ビデオモード 80x25 16色テキストに確定させる ---
        ; (BIOS によってはブート直後のモードが不定なため明示的に設定する)
        mov     ax, 0x0003
        int     0x10

        ; ------------------------------------------------------------
        ; フェーズ1: 画面クリア + タイトルバー描画
        ; ------------------------------------------------------------
        mov     byte [cur_attr], ATTR_NORMAL
        call    cls

        call    draw_title_bar

        mov     byte [cur_row], 2
        mov     byte [cur_col], 0
        call    sync_hw_cursor

        mov     byte [cur_attr], ATTR_NORMAL
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

; ============================================================================
; フェーズ1: 画面表示ルーチン群
; ============================================================================

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
; draw_title_bar - 0 行目を Win98 のタイトルバー風に塗る
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

        ; タイトル文字列を (0,1) から書く
        mov     di, 2                   ; 列1 → オフセット 1*2
        mov     si, msg_title
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
;        0x0D / 0x0A は改行、0x08 はバックスペースとして扱う
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
; scroll_up - 画面を 1 行上へスクロール (1 行目以降が対象、0 行目のタイトル
;             バーは固定表示なので動かさない)
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

        ; 最終行を空白で埋める
        mov     di, SCR_COLS * (SCR_ROWS - 1) * 2
        xor     ax, ax
        mov     ds, ax
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

; ============================================================================
; フェーズ1: キーボード入力
; ============================================================================
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

; ============================================================================
; フェーズ2: メモリマップ取得
; ============================================================================
; detect_memory - E820h を試し、駄目なら E801h、それも駄目なら AH=88h。
;                 どの方式が使えたかと、利用可能メモリ量 (MB) を表示する。
; ---------------------------------------------------------------------------
detect_memory:
        pusha
        push    es

        call    do_e820
        jc      .try_e801
        mov     byte [cur_attr], ATTR_OK
        mov     si, msg_via_e820
        call    puts
        jmp     .show_size

.try_e801:
        call    do_e801
        jc      .try_88
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_via_e801
        call    puts
        jmp     .show_size

.try_88:
        call    do_88
        jc      .failed
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_via_88
        call    puts
        jmp     .show_size

.failed:
        mov     byte [cur_attr], ATTR_ERR
        mov     si, msg_mem_fail
        call    puts
        jmp     .out

.show_size:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_usable
        call    puts
        mov     ax, [mem_mb]
        call    putdec16
        mov     si, msg_mb
        call    puts

.out:
        pop     es
        popa
        ret

; ---------------------------------------------------------------------------
; do_e820 - INT 15h EAX=E820h でメモリマップを取得し E820_BUF に格納。
;           成功: CF=0, [e820_count] にエントリ数, [mem_mb] に type1 合計(MB)
;           失敗: CF=1
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
        mov     ecx, 24
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
        mov     ecx, 24
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
        add     di, 24
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
; do_e801 - INT 15h AX=E801h (最大 4GB まで報告できる古めの方式)
;           CX/AX = 1MB-16MB の KB 数, DX/BX = 16MB 以降の 64KB ブロック数
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
; do_88 - INT 15h AH=88h (最も枯れた方式。1MB 超の KB 数を返すが 64MB 頭打ち)
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

; ============================================================================
; フェーズ2-A: A20 ゲート
; ============================================================================
; enable_a20 - A20 を有効化する。成功で CF=0、全方式失敗で CF=1。
;   1) 既に有効かチェック (最近のマシンは BIOS が有効にしている)
;   2) BIOS INT 15h AX=2401h  … 最も安全。対応していれば これが通る
;   3) キーボードコントローラ (8042) 経由 … 枯れているがタイミング依存
;   4) 高速 A20 (ポート 0x92)  … 一部機種でリセットが掛かるので最後に試す
; ---------------------------------------------------------------------------
enable_a20:
        call    check_a20
        jnc     .ok                     ; 既に有効

        ; --- 方式1: BIOS ---
        mov     ax, 0x2401
        int     0x15
        call    check_a20
        jnc     .ok

        ; --- 方式2: キーボードコントローラ ---
        call    a20_via_kbc
        call    check_a20
        jnc     .ok

        ; --- 方式3: Fast A20 (ポート 0x92) ---
        call    a20_via_fast
        call    check_a20
        jnc     .ok

        stc
        ret
.ok:
        clc
        ret

; ---------------------------------------------------------------------------
; check_a20 - A20 が有効なら CF=0、無効(1MB でラップする)なら CF=1
;   0000:0500 と FFFF:0510 は、A20 が無効だと同じ物理アドレスを指す。
;   実際に書き換えて確かめる (BIOS 実装に依存しない確実な方法)。
; ---------------------------------------------------------------------------
check_a20:
        push    ax
        push    bx
        push    ds
        push    es
        push    di
        push    si

        xor     ax, ax
        mov     ds, ax
        mov     si, 0x0500
        mov     ax, 0xFFFF
        mov     es, ax
        mov     di, 0x0510

        mov     al, [ds:si]             ; 元の値を退避
        mov     bl, al
        mov     al, [es:di]
        mov     bh, al

        mov     byte [ds:si], 0x00
        mov     byte [es:di], 0xFF
        mov     al, [ds:si]             ; ラップしていれば 0xFF に化ける

        mov     [ds:si], bl             ; 元に戻す
        mov     [es:di], bh

        cmp     al, 0xFF
        je      .disabled
        clc
        jmp     .out
.disabled:
        stc
.out:
        pop     si
        pop     di
        pop     es
        pop     ds
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; a20_via_kbc - 8042 キーボードコントローラの出力ポート bit1 を立てる
; ---------------------------------------------------------------------------
a20_via_kbc:
        push    ax
        cli
        call    .wait_in
        mov     al, 0xAD                ; キーボード無効化
        out     0x64, al

        call    .wait_in
        mov     al, 0xD0                ; 出力ポート読み出し
        out     0x64, al

        call    .wait_out
        in      al, 0x60
        push    ax

        call    .wait_in
        mov     al, 0xD1                ; 出力ポート書き込み
        out     0x64, al

        call    .wait_in
        pop     ax
        or      al, 0x02                ; bit1 = A20 有効
        out     0x60, al

        call    .wait_in
        mov     al, 0xAE                ; キーボード再有効化
        out     0x64, al

        call    .wait_in
        sti
        pop     ax
        ret

; 入力バッファが空くまで待つ (status bit1 = 0)。無限ループ防止のため上限付き。
.wait_in:
        push    cx
        mov     cx, 0xFFFF
.wi_loop:
        in      al, 0x64
        test    al, 0x02
        jz      .wi_done
        loop    .wi_loop
.wi_done:
        pop     cx
        ret

; 出力バッファにデータが来るまで待つ (status bit0 = 1)
.wait_out:
        push    cx
        mov     cx, 0xFFFF
.wo_loop:
        in      al, 0x64
        test    al, 0x01
        jnz     .wo_done
        loop    .wo_loop
.wo_done:
        pop     cx
        ret

; ---------------------------------------------------------------------------
; a20_via_fast - システム制御ポート A (0x92) の bit1 を立てる
;   注意: bit0 は Fast Reset。絶対に立ててはいけないので必ずマスクする。
; ---------------------------------------------------------------------------
a20_via_fast:
        push    ax
        in      al, 0x92
        test    al, 0x02
        jnz     .done                   ; 既に立っているなら触らない
        or      al, 0x02
        and     al, 0xFE                ; bit0 (Fast Reset) を必ず落とす
        out     0x92, al
.done:
        pop     ax
        ret

; ============================================================================
; フェーズ2-A: GDT とプロテクトモード移行
; ============================================================================
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
        mov     esp, 0x00090000         ; 32bit 用スタック (低位メモリの空き)

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

; --- GDT -------------------------------------------------------------------
; フラットモデル: コード/データとも base=0, limit=4GB
;
; セレクタの配置を Linux の 32bit Boot Protocol に合わせてある。
; カーネルに制御を渡すときは
;   「__BOOT_CS(0x10) と __BOOT_DS(0x18) のディスクリプタを積んだ GDT を
;     ロードし、CS=__BOOT_CS、DS/ES/SS=__BOOT_DS にしておくこと」
; と規定されている (Documentation/arch/x86/boot.rst の 32-bit BOOT PROTOCOL)。
; ここで 0x08/0x10 に置いてしまうとフェーズ2-B で必ず組み直しになるので、
; 最初から 0x10/0x18 に置いて、インデックス 1 (0x08) は空けてある。
align 8
gdt_start:
        ; 0x00: ヌルディスクリプタ (必須)
        dq 0x0000000000000000

        ; 0x08: 未使用 (Linux の __BOOT_CS を 0x10 に合わせるための詰め物)
        dq 0x0000000000000000

GDT_CODE32 equ 0x10             ; = Linux の __BOOT_CS
        ; 32bit コードセグメント
        ;  limit=0xFFFFF, base=0, type=0x9A(実行/読み取り可), G=1(4KB粒度), D=1(32bit)
        dw 0xFFFF               ; limit 15:0
        dw 0x0000               ; base 15:0
        db 0x00                 ; base 23:16
        db 0x9A                 ; access: P=1 DPL=0 S=1 type=1010(code, read)
        db 0xCF                 ; G=1 D=1 L=0 AVL=0 | limit 19:16 = F
        db 0x00                 ; base 31:24

GDT_DATA32 equ 0x18             ; = Linux の __BOOT_DS
        ; 32bit データセグメント
        dw 0xFFFF
        dw 0x0000
        db 0x00
        db 0x92                 ; access: type=0010(data, write)
        db 0xCF
        db 0x00
gdt_end:

gdt_desc:
        dw gdt_end - gdt_start - 1      ; リミット (サイズ-1)
        dd gdt_start                    ; 線形アドレス (セグメント0なのでそのまま)

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
cur_row:        db 0
cur_col:        db 0
cur_attr:       db ATTR_NORMAL
e820_count:     dw 0
mem_mb:         dw 0
total_lo:       dd 0
total_hi:       dd 0

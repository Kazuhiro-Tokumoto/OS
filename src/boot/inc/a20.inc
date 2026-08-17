; ============================================================================
; a20.inc  -  A20 ゲートの有効化
;
; 1MB 超のメモリにアクセスするには A20 アドレスラインを有効にする必要がある。
; 有効化の方法は複数あり、機種によって使えるものが違うので、
; 1方式に依存せず順に試し、そのつど「実際にラップアラウンドしなくなったか」
; を確かめる。BIOS の戻り値は信用しない。
;
;   1) 既に有効かチェック (最近のマシンは BIOS が有効にしている)
;   2) BIOS INT 15h AX=2401h  … 最も安全。対応していればこれで済む
;   3) キーボードコントローラ (8042) 経由 … 枯れているがタイミング依存
;   4) 高速 A20 (ポート 0x92) … 一部機種でリセットが掛かるので最後に試す
; ============================================================================

; ---------------------------------------------------------------------------
; enable_a20 - 成功で CF=0、全方式失敗で CF=1
; ---------------------------------------------------------------------------
enable_a20:
        call    check_a20
        jnc     .ok                     ; 既に有効

        mov     ax, 0x2401              ; 方式1: BIOS
        int     0x15
        call    check_a20
        jnc     .ok

        call    a20_via_kbc             ; 方式2: キーボードコントローラ
        call    check_a20
        jnc     .ok

        call    a20_via_fast            ; 方式3: Fast A20 (ポート 0x92)
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

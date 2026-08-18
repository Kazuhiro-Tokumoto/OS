; ============================================================================
; vbe.inc  -  VESA BIOS Extensions によるグラフィックモード設定
;
; フェーズ5 (VGA グラフィックス) の入口。
; Win98 もどきの GUI を描くには 16 色テキストでは話にならないので、
; リニアフレームバッファ (LFB) のあるグラフィックモードに切り替える。
;
; 方針: カーネルに任せずブートローダー側でモードを決める。
;   Linux の vesafb ドライバは「ブートローダーが VBE でモードを設定し、
;   その情報を boot_params の screen_info に書いておく」ことを前提にしている。
;   screen_info.orig_video_isVGA = VIDEO_TYPE_VLFB (0x23) がその合図。
;
; 互換性方針に従い、VBE が無ければ諦めてテキストモードのまま続行する。
; (VBE 2.0 は 1994 年からあるので Core 2 Duo 世代なら確実に載っているが、
;  「無ければ動かない」造りにはしない)
;
; 使い方:
;   call vbe_find    … 使えるモードを探すだけ (画面はまだ切り替わらない)
;   call vbe_set     … 実際にモードを切り替える (以後テキスト出力は不可)
;
; 依存: video.inc (画面出力)
; ============================================================================

VBE_INFO_BUF    equ 0x0C00      ; VbeInfoBlock (512 バイト)
VBE_MODE_BUF    equ 0x0E00      ; ModeInfoBlock (256 バイト)

; --- ModeInfoBlock のオフセット ---
MI_ATTRIBUTES   equ 0x00        ; bit0=対応, bit4=グラフィック, bit7=LFB あり
MI_PITCH        equ 0x10        ; BytesPerScanLine
MI_WIDTH        equ 0x12
MI_HEIGHT       equ 0x14
MI_BPP          equ 0x19
MI_MEMORY_MODEL equ 0x1B        ; 6 = ダイレクトカラー
MI_RED_SIZE     equ 0x1F
MI_RED_POS      equ 0x20
MI_GREEN_SIZE   equ 0x21
MI_GREEN_POS    equ 0x22
MI_BLUE_SIZE    equ 0x23
MI_BLUE_POS     equ 0x24
MI_RSVD_SIZE    equ 0x25
MI_RSVD_POS     equ 0x26
MI_PHYS_BASE    equ 0x28        ; LFB の物理アドレス

; ---------------------------------------------------------------------------
; vbe_find - 希望の解像度に合う LFB 付きモードを探す
;   出力: CF=0 見つかった ([vbe_ok]=1、各変数に情報)
;         CF=1 VBE 非対応か、希望に合うモードが無い
; ---------------------------------------------------------------------------
vbe_find:
        push    es
        pusha

        mov     byte [vbe_ok], 0

        ; --- VbeInfoBlock の取得 (AX=4F00h) ---
        xor     ax, ax
        mov     es, ax
        mov     di, VBE_INFO_BUF
        ; VBE 2.0 以降の情報が欲しいので、あらかじめ "VBE2" と書いておく
        mov     dword [es:di], 'VBE2'
        mov     ax, 0x4F00
        int     0x10
        cmp     ax, 0x004F
        jne     .fail
        cmp     dword [es:VBE_INFO_BUF], 'VESA'
        jne     .fail

        mov     ax, [es:VBE_INFO_BUF + 0x04]
        mov     [vbe_version], ax
        ; TotalMemory は 64KB ブロック単位。screen_info.lfb_size も
        ; 同じ 64KB 単位で渡す規約なのでそのまま持っておく。
        mov     ax, [es:VBE_INFO_BUF + 0x12]
        mov     [vbe_total_mem], ax

        ; VideoModePtr は far ポインタ (オフセット, セグメント) の順
        mov     ax, [es:VBE_INFO_BUF + 0x0E]
        mov     [modelist_off], ax
        mov     ax, [es:VBE_INFO_BUF + 0x10]
        mov     [modelist_seg], ax

        ; --- 設定アプリからの希望があれば、まずそれを試す ---
        ; 合うモードが無ければ黙って既定の並びに落ちる。
        ; 「設定した解像度がこの画面では出せない」ときに
        ; 起動しなくなるのが一番困るので、失敗しても止めない。
        mov     ax, [vbe_req_w]
        test    ax, ax
        jz      .use_defaults
        mov     [want_w], ax
        mov     ax, [vbe_req_h]
        mov     [want_h], ax
        mov     al, [vbe_req_bpp]
        test    al, al
        jnz     .req_bpp_ok
        mov     al, 32                  ; 色深度の指定が無ければ 32bpp
.req_bpp_ok:
        mov     [want_bpp], al
        call    .scan_modes
        jnc     .found

        ; 32bpp で駄目なら 24bpp でもう一度だけ試す
        mov     byte [want_bpp], 24
        call    .scan_modes
        jnc     .found

.use_defaults:
        ; --- 希望の解像度リストを上から順に試す ---
        mov     si, wanted_modes
.want_loop:
        mov     ax, [si]
        test    ax, ax                  ; 0 終端
        jz      .fail
        mov     [want_w], ax
        mov     ax, [si + 2]
        mov     [want_h], ax
        mov     al, [si + 4]
        mov     [want_bpp], al
        push    si
        call    .scan_modes
        pop     si
        jnc     .found
        add     si, 6
        jmp     .want_loop

.found:
        mov     byte [vbe_ok], 1
        popa
        pop     es
        clc
        ret
.fail:
        popa
        pop     es
        stc
        ret

; --- モードリストを頭から舐めて、want_* に一致するものを探す ---
.scan_modes:
        push    ds
        ; DS を差し替える前にオフセットを読むこと。
        ; 順番を逆にすると、新しい DS で modelist_off を読んでしまう。
        mov     si, [modelist_off]
        mov     ax, [modelist_seg]
        mov     ds, ax
.next_mode:
        lodsw                           ; DS:SI からモード番号を 1 つ
        cmp     ax, 0xFFFF              ; リスト終端
        je      .scan_fail
        push    ds
        push    si
        xor     bx, bx
        mov     ds, bx                  ; 変数を触るので DS を 0 に戻す
        mov     [try_mode], ax
        call    .check_mode
        pop     si
        pop     ds
        jnc     .scan_ok
        jmp     .next_mode
.scan_ok:
        pop     ds
        clc
        ret
.scan_fail:
        pop     ds
        stc
        ret

; --- 1 モードぶんの情報を取って、条件に合うか調べる ---
;     合えば vbe_* に記録して CF=0
.check_mode:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     di, VBE_MODE_BUF
        mov     cx, [try_mode]
        mov     ax, 0x4F01
        int     0x10
        cmp     ax, 0x004F
        jne     .cm_fail

        mov     ax, [es:VBE_MODE_BUF + MI_ATTRIBUTES]
        test    ax, 0x0001              ; ハードウェアが対応しているか
        jz      .cm_fail
        test    ax, 0x0010              ; グラフィックモードか
        jz      .cm_fail
        test    ax, 0x0080              ; リニアフレームバッファが使えるか
        jz      .cm_fail

        cmp     byte [es:VBE_MODE_BUF + MI_MEMORY_MODEL], 6  ; ダイレクトカラー
        jne     .cm_fail

        mov     ax, [es:VBE_MODE_BUF + MI_WIDTH]
        cmp     ax, [want_w]
        jne     .cm_fail
        mov     ax, [es:VBE_MODE_BUF + MI_HEIGHT]
        cmp     ax, [want_h]
        jne     .cm_fail
        mov     al, [es:VBE_MODE_BUF + MI_BPP]
        cmp     al, [want_bpp]
        jne     .cm_fail

        ; --- 条件を満たしたので記録する ---
        mov     ax, [try_mode]
        mov     [vbe_mode], ax
        mov     ax, [es:VBE_MODE_BUF + MI_WIDTH]
        mov     [vbe_w], ax
        mov     ax, [es:VBE_MODE_BUF + MI_HEIGHT]
        mov     [vbe_h], ax
        movzx   ax, byte [es:VBE_MODE_BUF + MI_BPP]
        mov     [vbe_bpp], ax
        mov     ax, [es:VBE_MODE_BUF + MI_PITCH]
        mov     [vbe_pitch], ax
        mov     eax, [es:VBE_MODE_BUF + MI_PHYS_BASE]
        mov     [vbe_lfb], eax

        mov     al, [es:VBE_MODE_BUF + MI_RED_SIZE]
        mov     [vbe_red_size], al
        mov     al, [es:VBE_MODE_BUF + MI_RED_POS]
        mov     [vbe_red_pos], al
        mov     al, [es:VBE_MODE_BUF + MI_GREEN_SIZE]
        mov     [vbe_green_size], al
        mov     al, [es:VBE_MODE_BUF + MI_GREEN_POS]
        mov     [vbe_green_pos], al
        mov     al, [es:VBE_MODE_BUF + MI_BLUE_SIZE]
        mov     [vbe_blue_size], al
        mov     al, [es:VBE_MODE_BUF + MI_BLUE_POS]
        mov     [vbe_blue_pos], al
        mov     al, [es:VBE_MODE_BUF + MI_RSVD_SIZE]
        mov     [vbe_rsvd_size], al
        mov     al, [es:VBE_MODE_BUF + MI_RSVD_POS]
        mov     [vbe_rsvd_pos], al

        pop     es
        clc
        ret
.cm_fail:
        pop     es
        stc
        ret

; ---------------------------------------------------------------------------
; vbe_set - vbe_find で見つけたモードに実際に切り替える
;   これを呼んだあとはテキスト出力 (0xB8000) は使えない。
;   出力: CF=0 成功 / CF=1 失敗
; ---------------------------------------------------------------------------
vbe_set:
        push    ax
        push    bx
        cmp     byte [vbe_ok], 0
        je      .fail
        mov     bx, [vbe_mode]
        or      bx, 0x4000              ; bit14: リニアフレームバッファを使う
        mov     ax, 0x4F02
        int     0x10
        cmp     ax, 0x004F
        jne     .fail
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     bx
        pop     ax
        stc
        ret

; --- 欲しい解像度 (幅, 高さ, bpp) を上から順に試す ------------------------
; Win98 の標準的な解像度に寄せてある。
; 32bpp を優先し、見つからなければ 24bpp に落とす。
wanted_modes:
        dw 1024, 768
        db 32, 0
        dw 1024, 768
        db 24, 0
        dw 800, 600
        db 32, 0
        dw 800, 600
        db 24, 0
        dw 640, 480
        db 32, 0
        dw 640, 480
        db 24, 0
        dw 0, 0
        db 0, 0

; --- 変数 ------------------------------------------------------------------
vbe_ok:         db 0
vbe_version:    dw 0
vbe_total_mem:  dw 0            ; VRAM 量 (64KB ブロック単位)
vbe_mode:       dw 0
vbe_w:          dw 0
vbe_h:          dw 0
vbe_bpp:        dw 0
vbe_pitch:      dw 0
vbe_lfb:        dd 0
vbe_red_size:   db 0
vbe_red_pos:    db 0
vbe_green_size: db 0
vbe_green_pos:  db 0
vbe_blue_size:  db 0
vbe_blue_pos:   db 0
vbe_rsvd_size:  db 0
vbe_rsvd_pos:   db 0
modelist_off:   dw 0
modelist_seg:   dw 0
try_mode:       dw 0
; 設定アプリからの希望 (Stage2 がペイロードテーブルから読んで入れる)
vbe_req_w:      dw 0
vbe_req_h:      dw 0
vbe_req_bpp:    db 0

want_w:         dw 0
want_h:         dw 0
want_bpp:       db 0

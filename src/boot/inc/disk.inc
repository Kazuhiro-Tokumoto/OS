; ============================================================================
; disk.inc  -  ディスク読み込みと 1MB 超への転送
;
; 互換性方針 (引き継ぎ資料 1章) に従い、新しめの機能は必ず
; 「対応チェック → 非対応なら枯れた方式」の順で使う。
;
;   読み込み: INT 13h AH=41h で拡張機能の有無を調べ、
;             あれば AH=42h (LBA 拡張読み込み)、
;             なければ AH=02h (CHS) にフォールバックする。
;             CHS 用のジオメトリは AH=08h で取得し、
;             それも失敗したらフロッピーの既定値 (18 sect/track, 2 heads)。
;
;   1MB 超へのロード:
;             リアルモードのセグメント:オフセットでは 1MB より上を指せない。
;             アンリアルモード (ビッグリアルモード) を使う。
;             一度プロテクトモードに入って FS に「base=0 / limit=4GB」の
;             ディスクリプタを載せ、リアルモードに戻る。
;             戻ったあと FS を二度と触らなければ、ディスクリプタキャッシュに
;             リミット 4GB が残るので fs:[edi] で 4GB 全域に書き込める。
;
; 依存: gdt.inc (GDT_DATA32, gdt_desc), video.inc (putc, puts など)
; ============================================================================

DISK_RETRY      equ 3           ; 読み込みリトライ回数
LOWBUF_SEG      equ 0x2000      ; バウンスバッファ (物理 0x20000)
LOWBUF_LIN      equ 0x00020000
CHUNK_SECTORS   equ 64          ; 1 回に読むセクタ数 (32KB)
                                ; BIOS によっては 127 セクタが上限なので
                                ; 余裕をもって 64 にしてある

; ---------------------------------------------------------------------------
; disk_init - DL のドライブを対象に、拡張機能の有無とジオメトリを調べる
;             結果は dsk_edd / dsk_spt / dsk_heads に入る
; ---------------------------------------------------------------------------
disk_init:
        push    ax
        push    bx
        push    cx
        push    dx
        push    es
        push    di

        mov     [dsk_drive], dl

        ; --- INT 13h AH=41h: 拡張機能の存在チェック ---
        mov     ah, 0x41
        mov     bx, 0x55AA
        int     0x13
        jc      .no_edd
        cmp     bx, 0xAA55              ; BX が入れ替わって返るのが目印
        jne     .no_edd
        test    cl, 0x01                ; bit0: パケット方式の読み書きが使えるか
        jz      .no_edd
        mov     byte [dsk_edd], 1
        jmp     .geometry
.no_edd:
        mov     byte [dsk_edd], 0

.geometry:
        ; --- INT 13h AH=08h: ドライブのジオメトリ取得 ---
        ; CL bit0-5 = セクタ/トラック, DH = 最大ヘッド番号
        mov     ah, 0x08
        xor     di, di
        mov     es, di                  ; 一部の BIOS のバグ対策で ES:DI=0
        mov     dl, [dsk_drive]
        int     0x13
        jc      .geo_default
        and     cl, 0x3F
        jz      .geo_default
        movzx   ax, cl
        mov     [dsk_spt], ax
        movzx   ax, dh
        inc     ax
        mov     [dsk_heads], ax
        jmp     .done
.geo_default:
        ; 取得できなければ 1.44MB フロッピーの既定値を使う
        mov     word [dsk_spt], 18
        mov     word [dsk_heads], 2
.done:
        pop     di
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; disk_read - LBA 指定でセクタを読む
;   入力: EAX = 開始 LBA, CX = セクタ数, ES:BX = 転送先
;   出力: CF=0 成功 / CF=1 失敗 (AH にエラーコード)
;   リトライはディスクリセットを挟んで DISK_RETRY 回。
; ---------------------------------------------------------------------------
disk_read:
        push    si
        push    di
        push    dx
        push    cx
        push    bx
        push    eax

        mov     [dsk_lba], eax
        mov     [dsk_cnt], cx
        mov     [dsk_off], bx
        mov     [dsk_seg], es

        mov     di, DISK_RETRY
.retry:
        ; CD は必ず LBA で読む。CHS には 2048 バイトセクタという
        ; 考えが無いので、そちらへ落ちると壊れた読み方になる。
        cmp     byte [dsk_cdrom], 0
        jne     .lba_only
        cmp     byte [dsk_edd], 0
        je      .chs
.lba_only:
        call    .do_lba
        jnc     .ok
        jmp     .after
.chs:
        call    .do_chs
        jnc     .ok
.after:
        ; 失敗したらリセットしてリトライ
        push    ax
        xor     ax, ax
        mov     dl, [dsk_drive]
        int     0x13
        pop     ax
        dec     di
        jnz     .retry
        stc
        jmp     .out
.ok:
        clc
.out:
        pop     eax
        pop     bx
        pop     cx
        pop     dx
        pop     di
        pop     si
        ret

; --- LBA 拡張読み込み (INT 13h AH=42h) ---
;
; CD の場合、BIOS が扱う 1 セクタは 2048 バイトで、LBA もその単位になる。
; 呼び出し側は常に 512 バイト単位で話すことにして、ここで 4 で割る。
; そうしておけば、ディスクから起動しても CD から起動しても
; 上位のコードは何も変えなくていい。
;
; 端数は切り上げて読む。ペイロードは 2048 境界に揃えて置いてあるので
; 開始位置は必ず割り切れる。最後のチャンクだけ数が半端になりうるが、
; バウンスバッファは 32KB あるので 3 セクタ余分に読んでも溢れない。
.do_lba:
        mov     eax, [dsk_lba]
        mov     cx, [dsk_cnt]
        cmp     byte [dsk_cdrom], 0
        je      .lba_units_ok
        shr     eax, 2                  ; 512 → 2048 バイト単位
        add     cx, 3
        shr     cx, 2                   ; 切り上げ
.lba_units_ok:
        mov     [dap_lba], eax
        mov     dword [dap_lba + 4], 0
        mov     ax, cx
        mov     [dap_count], ax
        mov     ax, [dsk_off]
        mov     [dap_off], ax
        mov     ax, [dsk_seg]
        mov     [dap_seg], ax

        mov     ah, 0x42
        mov     dl, [dsk_drive]
        mov     si, dap
        int     0x13
        ret

; --- CHS 読み込み (INT 13h AH=02h) ---
; LBA → CHS の変換:
;   sector   = (LBA % spt) + 1
;   head     = (LBA / spt) % heads
;   cylinder = (LBA / spt) / heads
; トラック境界やヘッド境界をまたぐと 1 回の呼び出しでは読めない BIOS が
; あるので、CHS 経路は 1 セクタずつ読む。遅いが確実。
.do_chs:
        push    cx
        push    bx
        ; リトライで最初からやり直せるよう、作業用のコピーを使う。
        ; dsk_lba / dsk_seg を直接進めてしまうと、失敗して再試行したときに
        ; 途中から読み直すことになり辻褄が合わなくなる。
        mov     eax, [dsk_lba]
        mov     [chs_lba], eax
        mov     ax, [dsk_seg]
        mov     [chs_seg], ax
        mov     cx, [dsk_cnt]
        mov     bx, [dsk_off]
.chs_loop:
        push    cx

        mov     eax, [chs_lba]
        xor     edx, edx
        movzx   ecx, word [dsk_spt]
        div     ecx                     ; EAX = LBA/spt, EDX = LBA%spt
        mov     [chs_sector], dl
        inc     byte [chs_sector]       ; セクタ番号は 1 始まり

        xor     edx, edx
        movzx   ecx, word [dsk_heads]
        div     ecx                     ; EAX = シリンダ, EDX = ヘッド
        mov     [chs_head], dl
        cmp     eax, 1023               ; CHS で表現できる上限
        ja      .chs_fail
        mov     [chs_cyl], ax

        mov     ax, [chs_seg]
        mov     es, ax
        mov     ah, 0x02
        mov     al, 1                   ; 1 セクタずつ
        mov     cx, [chs_cyl]
        mov     ch, cl                  ; CH = シリンダ下位 8bit
        mov     cl, [chs_cyl + 1]
        shl     cl, 6                   ; CL bit6-7 = シリンダ上位 2bit
        or      cl, [chs_sector]        ; CL bit0-5 = セクタ番号
        mov     dh, [chs_head]
        mov     dl, [dsk_drive]
        int     0x13
        jc      .chs_fail_pop

        add     bx, 512                 ; 次のバッファ位置
        jnc     .no_wrap
        ; 64KB 境界を越えたらセグメントを進める
        mov     ax, [chs_seg]
        add     ax, 0x1000
        mov     [chs_seg], ax
.no_wrap:
        inc     dword [chs_lba]

        pop     cx
        loop    .chs_loop

        pop     bx
        pop     cx
        clc
        ret

.chs_fail_pop:
        pop     cx
.chs_fail:
        pop     bx
        pop     cx
        stc
        ret

; ---------------------------------------------------------------------------
; enter_unreal - アンリアルモードに入る (FS だけリミット 4GB にする)
;
; FS に base=0 / limit=4GB のディスクリプタを載せた状態でリアルモードに戻る。
; 戻ったあと FS を一切触らなければ、ディスクリプタキャッシュのリミットが
; 4GB のまま残るので fs:[edi] で 4GB 全域を読み書きできる。
; DS/ES を使うとリアルモードで再ロードされたときに壊れる恐れがあるので、
; 高位アクセスは必ず FS 経由にすること。
; ---------------------------------------------------------------------------
enter_unreal:
        push    eax
        push    bx
        pushf
        cli

        lgdt    [gdt_desc]

        mov     eax, cr0
        or      al, 1                   ; PE = 1
        mov     cr0, eax
        jmp     short $ + 2             ; パイプラインをフラッシュ

        mov     bx, GDT_DATA32
        mov     fs, bx                  ; ← ここで limit=4GB が FS に載る

        mov     eax, cr0
        and     al, 0xFE                ; PE = 0 (リアルモードへ戻る)
        mov     cr0, eax
        jmp     short $ + 2

        popf
        pop     bx
        pop     eax
        ret

; ---------------------------------------------------------------------------
; check_unreal - アンリアルモードが実際に効いているか確かめる
;   物理 0x00100000 に magic を書いて読み返す。
;   出力: CF=0 成功 / CF=1 失敗
; ---------------------------------------------------------------------------
check_unreal:
        push    eax
        push    ebx
        push    edi

        mov     edi, 0x00100000
        mov     ebx, [fs:edi]           ; 元の値を退避
        mov     eax, 0xA5C3F00D
        mov     [fs:edi], eax
        mov     eax, [fs:edi]
        mov     [fs:edi], ebx           ; 元に戻す
        cmp     eax, 0xA5C3F00D
        jne     .fail
        clc
        jmp     .out
.fail:
        stc
.out:
        pop     edi
        pop     ebx
        pop     eax
        ret

; ---------------------------------------------------------------------------
; disk_detect_cdrom - El Torito (CD) から起動したかを調べる
;   入力: [dsk_drive] にブートドライブ番号が入っていること
;   出力: [dsk_cdrom] に 0 (ディスク) または 1 (CD)
;         CF は壊さない前提では使わない
;
; INT 13h AH=4Bh AL=01h は「El Torito の起動情報を返せ」という命令。
; CD から起動していればこれが通り、そうでなければ CF が立つ。
; 返ってくる 0x13 バイトの中身:
;   +0  パケットの大きさ (0x13)
;   +1  エミュレーション種別 (0 = ノーエミュレーション)
;   +2  ドライブ番号
;   ...
;
; ドライブ番号を決め打ちで 0xE0 などと見るやり方もあるが、
; 機種によって値が違うので当てにならない。BIOS に聞くのが確実。
; ---------------------------------------------------------------------------
disk_detect_cdrom:
        push    ax
        push    dx
        push    si

        mov     byte [dsk_cdrom], 0

        ; パケットを 0 で埋めて大きさだけ入れておく
        mov     si, etspec
        mov     byte [si], 0x13

        mov     ax, 0x4B01
        mov     dl, [dsk_drive]
        int     0x13
        jc      .not_cd

        ; ノーエミュレーション起動のときだけ 2048 バイトセクタになる。
        ; フロッピー/HDD エミュレーションでは BIOS が 512 バイトに
        ; 見せてくれるので、そのときは通常の扱いでよい。
        cmp     byte [etspec + 1], 0
        jne     .not_cd

        mov     byte [dsk_cdrom], 1

        ; BIOS が教えてくれたドライブ番号を採用する。
        ; DL で渡ってきた値と食い違う機種があるため。
        mov     al, [etspec + 2]
        test    al, al
        jz      .keep_drive
        mov     [dsk_drive], al
.keep_drive:
.not_cd:
        pop     si
        pop     dx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; disk_load_high - ディスクから 1MB 超の物理アドレスへ読み込む
;   入力: EAX = 開始 LBA
;         ECX = セクタ数
;         EDI = 転送先の物理(線形)アドレス
;   出力: CF=0 成功 / CF=1 失敗
;   バウンスバッファ (LOWBUF_SEG) 経由で CHUNK_SECTORS ずつ運ぶ。
;   前提: enter_unreal 済みであること。
; ---------------------------------------------------------------------------
disk_load_high:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    es

        mov     [hi_lba], eax
        mov     [hi_left], ecx
        mov     [hi_dst], edi

.chunk_loop:
        mov     ecx, [hi_left]
        test    ecx, ecx
        jz      .done

        cmp     ecx, CHUNK_SECTORS
        jbe     .have_count
        mov     ecx, CHUNK_SECTORS
.have_count:
        mov     [hi_this], ecx

        ; --- ディスク → バウンスバッファ ---
        mov     ax, LOWBUF_SEG
        mov     es, ax
        xor     bx, bx
        mov     eax, [hi_lba]
        mov     cx, [hi_this]
        call    disk_read
        jc      .fail

        ; BIOS 呼び出しの中で FS が書き換えられるとアンリアルモードの
        ; リミットが失われる。INT 13h のたびに張り直しておく。
        call    enter_unreal

        ; --- バウンスバッファ → 高位メモリ (FS 経由の 32bit アドレッシング) ---
        ; DS を差し替える前に、必要な値をレジスタに読み出しておく。
        mov     edi, [hi_dst]
        mov     ecx, [hi_this]
        shl     ecx, 7                  ; セクタ数 * 512 / 4 = dword 数
        mov     ax, LOWBUF_SEG
        mov     ds, ax
        xor     si, si
.copy_loop:
        lodsd                           ; EAX = [ds:si], SI += 4
        mov     [fs:edi], eax
        add     edi, 4
        dec     ecx
        jnz     .copy_loop

        xor     ax, ax
        mov     ds, ax                  ; DS を 0 に戻す

        ; --- 進捗を更新 ---
        mov     eax, [hi_this]
        add     [hi_lba], eax
        sub     [hi_left], eax
        shl     eax, 9                  ; * 512
        add     [hi_dst], eax

        ; 進捗の点。チャンクごとに打つとモノリシックなカーネル
        ; (数十 MB) では画面が点で埋まるので、1MB ごとに 1 個にする。
        inc     byte [hi_dots]
        cmp     byte [hi_dots], 32      ; 32 * 32KB = 1MB
        jb      .chunk_loop
        mov     byte [hi_dots], 0
        call    tick_dots
        jmp     .chunk_loop

.done:
        clc
        jmp     .out
.fail:
        xor     ax, ax
        mov     ds, ax
        stc
.out:
        pop     es
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; --- DAP (Disk Address Packet, INT 13h AH=42h 用) --------------------------
align 4
dap:
dap_size:       db 0x10         ; この構造体のサイズ
                db 0            ; 予約
dap_count:      dw 0            ; 読み込むセクタ数
dap_off:        dw 0            ; 転送先オフセット
dap_seg:        dw 0            ; 転送先セグメント
dap_lba:        dq 0            ; 開始 LBA (64bit)

; --- 変数 ------------------------------------------------------------------
dsk_drive:      db 0
dsk_edd:        db 0            ; 1 なら INT 13h 拡張機能が使える
dsk_spt:        dw 18           ; セクタ/トラック
dsk_heads:      dw 2            ; ヘッド数
dsk_lba:        dd 0
dsk_cnt:        dw 0
dsk_cdrom:      db 0            ; 1 なら 1 セクタ 2048 バイトの CD

; INT 13h AH=4Bh が書き込む El Torito の起動情報 (0x13 バイト)
align 4
etspec:         times 0x14 db 0
dsk_off:        dw 0
dsk_seg:        dw 0
chs_lba:        dd 0            ; CHS 経路の作業用コピー
chs_seg:        dw 0
chs_cyl:        dw 0
chs_head:       db 0
chs_sector:     db 0
hi_lba:         dd 0
hi_left:        dd 0
hi_this:        dd 0
hi_dst:         dd 0
hi_dots:        db 0

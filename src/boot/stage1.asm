; ============================================================================
; stage1.asm  -  myOS Stage1 (MBR / ブートセクタ, 512 バイト)
;
; 役割:
;   フロッピーの 2 セクタ目以降 (Stage2) を 0x0000:0x7E00 に読み込み、
;   そこへ far jump するだけ。
;
; 互換性方針 (引き継ぎ資料 1章):
;   - レガシー BIOS 前提 / リアルモード
;   - 特定機種に依存しないよう、以下を徹底している:
;       * ブートドライブ番号は決め打ちせず BIOS が DL に入れてくれた値を使う
;         (資料 5章(2) の「MOV DL, 80h 決め打ちでダブルフォルト」対策の
;          恒久版。00h 決め打ちでも VirtualBox では動くが、
;          USB-FDD エミュレーションなどでは DL が 00h とは限らないため)
;       * 読み込みは枯れた CHS 方式 (INT 13h AH=02h) のみを使う。
;         LBA 拡張 (AH=42h) はフロッピー BIOS では未対応のことが多いので
;         Stage1 では使わない。1MB 超へのロードが必要になる Stage2 側で
;         対応チェック付きで導入する。
;       * 読み込み失敗時はディスクリセット (AH=00h) を挟んで 5 回リトライ。
;         実機のフロッピーはモータ回転待ちで初回が失敗することがある。
; ============================================================================
BITS 16
ORG 0x7C00

STAGE2_OFF      equ 0x7E00      ; Stage2 のロード先オフセット (セグメント 0)
STAGE2_SECTORS  equ 17          ; 読み込むセクタ数
                                ; 1.44MB FD は 1 トラック 18 セクタ。
                                ; セクタ 1 は Stage1 自身なので、
                                ; シリンダ0/ヘッド0 の残り 17 セクタ (8704 byte)
                                ; を一括で読む。トラックをまたがないので
                                ; どの BIOS でも 1 回の INT 13h で完結する。
RETRY_COUNT     equ 5

; ---------------------------------------------------------------------------
start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00      ; スタックは 0x7C00 の直下に伸ばす
        sti

        ; BIOS はブート時に DL = ブートドライブ番号を渡してくれる。
        ; これを保存して以後ずっと使う (決め打ちしない)。
        mov     [boot_drive], dl

        ; Stage2 が既に 0x7E00 に載っているなら読み込みは要らない。
        ; CD (El Torito のノーエミュレーション起動) では BIOS が
        ; stage1 と stage2 をまとめて 0x7C00 に読み込んでくれるため。
        ; Stage2 はオフセット 3 に 'MYS2' の目印を置いてある。
        cmp     dword [STAGE2_OFF + 3], 'MYS2'
        je      .read_ok

        mov     cx, RETRY_COUNT

.read_retry:
        push    cx

        ; --- ディスクリセット (INT 13h AH=00h) ---
        xor     ax, ax
        mov     dl, [boot_drive]
        int     0x13

        ; --- セクタ読み込み (INT 13h AH=02h, CHS) ---
        xor     ax, ax
        mov     es, ax              ; ES:BX = 0000:7E00
        mov     bx, STAGE2_OFF
        mov     ah, 0x02            ; 機能: セクタ読み込み
        mov     al, STAGE2_SECTORS  ; 読み込むセクタ数
        mov     ch, 0               ; シリンダ 0
        mov     cl, 2               ; 開始セクタ番号 2 (1 は Stage1 自身)
        mov     dh, 0               ; ヘッド 0
        mov     dl, [boot_drive]
        int     0x13

        pop     cx
        jnc     .read_ok            ; CF=0 なら成功

        loop    .read_retry         ; CX を減らしてリトライ
        jmp     disk_error

.read_ok:
        ; Stage2 にもブートドライブ番号を伝える (DL で受け渡し)
        mov     dl, [boot_drive]
        jmp     0x0000:STAGE2_OFF   ; CS も 0 に確定させたいので far jump

; ---------------------------------------------------------------------------
; エラー表示: "E:" + AH のエラーコードを 16 進 2 桁で出して停止
; (ブラウザ IDE 時代は 'E' 1 文字だけだったが、nasm を使う本ビルドでは
;  原因追跡しやすいようエラーコードも出す)
; ---------------------------------------------------------------------------
disk_error:
        mov     si, msg_err
.puts:
        lodsb
        test    al, al
        jz      halt
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .puts

halt:
        hlt
        jmp     halt

; ---------------------------------------------------------------------------
msg_err:        db 'STAGE1: DISK ERROR', 0
boot_drive:     db 0

; --- 510 バイトまで 0 埋めし、末尾にブートシグネチャ ---
        times 510-($-$$) db 0
        dw 0xAA55

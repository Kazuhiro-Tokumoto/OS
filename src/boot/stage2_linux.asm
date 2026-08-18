; ============================================================================
; stage2_linux.asm  -  myOS Stage2 / Linux Boot Protocol ローダー (フェーズ2-B)
;
; ロード先: 0x0000:0x7E00 (Stage1 が 17 セクタ読み込む)
; 入力:     DL = BIOS が渡してきたブートドライブ番号 (Stage1 から中継)
;
; やること:
;   1. ディスクの拡張機能 (INT 13h AH=41h) を調べる
;   2. メモリマップ取得 (E820h → E801h → 88h)
;   3. A20 有効化 (3方式フォールバック + 実地検証)
;   4. アンリアルモードに入る (1MB 超へ書けるようにする)
;   5. ペイロードテーブル (LBA 20) を読み、bzImage / initramfs の位置を知る
;   6. bzImage の setup ヘッダを解析 ('HdrS' / setup_sects / version)
;   7. カーネル本体を 0x100000 へ、initramfs を 0x04000000 へ読み込む
;   8. boot_params (ゼロページ) を 0x90000 に構築
;   9. プロテクトモードへ移行し、32bit エントリポイントへジャンプ
;
; 参照: Documentation/arch/x86/boot.rst の "32-bit BOOT PROTOCOL"
;   「CPU は 32bit プロテクトモード / ページング無効。
;     GDT に __BOOT_CS(0x10) と __BOOT_DS(0x18) を積み、
;     CS=__BOOT_CS、DS/ES/SS=__BOOT_DS。割り込み禁止。
;     %esi に boot_params の物理アドレス。%ebp/%edi/%ebx は 0。」
; ============================================================================
BITS 16
ORG 0x7E00

; Stage1 は 0x7E00 の先頭バイトへジャンプしてくる。
; %include したルーチン群が先に展開されるので、必ずここで本体へ飛ばすこと。
; (これを忘れると 0x7E00 が video.inc の cls の途中になり、いきなり暴走する)
        jmp     stage2_start
stage2_magic:
        db      'MYS2'          ; Stage1 が「Stage2 はもう載っているか」を
                                ; 見分けるための目印。オフセット 3 に固定。
                                ; CD (El Torito) から起動したときは BIOS が
                                ; stage1+stage2 をまとめて 0x7C00 に読み込んで
                                ; くれるので、Stage1 の読み込みが不要になる。

%include "inc/video.inc"
%include "inc/a20.inc"
%include "inc/memmap.inc"
%include "inc/gdt.inc"
%include "inc/disk.inc"
%include "inc/vbe.inc"

; --- メモリ配置 ------------------------------------------------------------
BOOTPARAMS_SEG  equ 0x9000              ; boot_params (ゼロページ) = 0x00090000
BOOTPARAMS_LIN  equ 0x00090000
CMDLINE_OFF     equ 0x2000              ; ゼロページと同じセグメント内に置く
CMDLINE_LIN     equ 0x00092000
KERNEL_DST      equ 0x00100000          ; カーネル本体 (1MB)
INITRD_DST      equ 0x08000000          ; initramfs (128MB)
                                        ; モノリシック構成のカーネルは
                                        ; 展開前でも数十 MB になるので、
                                        ; 1MB から伸びる本体とぶつからない
                                        ; よう十分に離してある
SCRATCH_SEG     equ 0x1000              ; bzImage 先頭を置く作業領域 = 0x10000
PTBL_OFF        equ 0x0900              ; ペイロードテーブルの置き場 (0x0900)
PTBL_LBA        equ 20                  ; ペイロードテーブルのあるセクタ。
                                        ; 20 * 512 = 10240 = 5 * 2048 なので
                                        ; CD の 2048 バイトセクタ境界にも乗る
PM_STACK        equ 0x0008FFF0

; --- ペイロードテーブルのレイアウト (tools/build_image.py が書き込む) ------
; 全ての LBA は 4 の倍数 (= 2048 バイト境界) に置かれている。
; CD から起動する場合、INT 13h は 2048 バイト単位でしか読めないため。
; bzImage は setup 部と本体に分けて別々に配置してある。
; こうしないと本体の開始位置が setup_sects 次第で境界に乗らなくなる。
PT_MAGIC        equ 0x00                ; 8 bytes "MYOSPLD2"
PT_SETUP_LBA    equ 0x08                ; bzImage の setup 部 (ヘッダを含む)
PT_SETUP_SECT   equ 0x0C
PT_BODY_LBA     equ 0x10                ; プロテクトモード用カーネル本体
PT_BODY_SECT    equ 0x14
PT_BODY_SIZE    equ 0x18
PT_ILBA         equ 0x1C                ; initramfs
PT_ISECT        equ 0x20
PT_ISIZE        equ 0x24
PT_CMDLINE      equ 0x28                ; コマンドライン (0終端)

; 画面の希望。0 なら wanted_modes の既定の並びに任せる。
; セクタの末尾を使う。コマンドラインは 0x28 から伸びるので、
; 手前に足すと既存のイメージと互換が無くなる。
;
; ここに置く理由: 解像度は X からは変えられない。
; nomodeset で DRM に触らせず、ブートローダーが VBE で決めた
; フレームバッファをそのまま使い続ける作りなので、
; 「次に起動するときの希望」を書いておく場所が要る。
PT_VIDEO_W      equ 0x1F0               ; u16 幅  (0 = おまかせ)
PT_VIDEO_H      equ 0x1F2               ; u16 高さ
PT_VIDEO_BPP    equ 0x1F4               ; u8  色深度 (0 = おまかせ)

; --- boot_params (ゼロページ) の主なオフセット -----------------------------
BP_ORIG_X           equ 0x000
BP_ORIG_Y           equ 0x001
BP_ORIG_VIDEO_PAGE  equ 0x004
BP_ORIG_VIDEO_MODE  equ 0x006
BP_ORIG_VIDEO_COLS  equ 0x007
BP_ORIG_VIDEO_LINES equ 0x00E
BP_ORIG_VIDEO_ISVGA equ 0x00F
BP_ORIG_VIDEO_POINTS equ 0x010
; VESA リニアフレームバッファの情報 (screen_info の続き)
BP_LFB_WIDTH        equ 0x012
BP_LFB_HEIGHT       equ 0x014
BP_LFB_DEPTH        equ 0x016
BP_LFB_BASE         equ 0x018
BP_LFB_SIZE         equ 0x01C           ; 64KB ブロック単位
BP_LFB_LINELENGTH   equ 0x024
BP_RED_SIZE         equ 0x026
BP_RED_POS          equ 0x027
BP_GREEN_SIZE       equ 0x028
BP_GREEN_POS        equ 0x029
BP_BLUE_SIZE        equ 0x02A
BP_BLUE_POS         equ 0x02B
BP_RSVD_SIZE        equ 0x02C
BP_RSVD_POS         equ 0x02D
BP_PAGES            equ 0x032
BP_VESA_ATTRIBUTES  equ 0x034

VIDEO_TYPE_VGAC     equ 0x22            ; VGA テキスト
VIDEO_TYPE_VLFB     equ 0x23            ; VESA リニアフレームバッファ
BP_E820_ENTRIES     equ 0x1E8           ; u8: エントリ数
BP_SETUP_SECTS      equ 0x1F1
BP_HDR_JUMP         equ 0x200           ; EB xx : xx がヘッダ長のヒント
BP_HDR_MAGIC        equ 0x202           ; 'HdrS'
BP_VERSION          equ 0x206
BP_TYPE_OF_LOADER   equ 0x210
BP_LOADFLAGS        equ 0x211
BP_CODE32_START     equ 0x214
BP_RAMDISK_IMAGE    equ 0x218
BP_RAMDISK_SIZE     equ 0x21C
BP_CMD_LINE_PTR     equ 0x228
BP_E820_TABLE       equ 0x2D0           ; 20 バイト × 最大 128 エントリ
E820_ZP_ENTSZ       equ 20              ; ゼロページ側のエントリサイズ

LOADFLAG_LOADED_HIGH equ 0x01
LOADFLAG_QUIET       equ 0x20

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

        mov     ax, 0x0003              ; 80x25 16色テキストに確定させる
        int     0x10

        mov     byte [cur_attr], ATTR_NORMAL
        call    cls
        mov     si, msg_title
        call    draw_title_bar
        mov     byte [cur_row], 2
        call    sync_hw_cursor

        ; ------------------------------------------------------------
        ; 1. ディスクの能力を調べる
        ; ------------------------------------------------------------
        mov     si, msg_disk
        call    puts
        mov     dl, [boot_drive]
        call    disk_init
        call    disk_detect_cdrom
        cmp     byte [dsk_cdrom], 0
        je      .not_cd
        mov     ah, ATTR_OK
        mov     si, msg_cdrom
        call    puts_attr
        call    newline
        mov     byte [cur_attr], ATTR_NORMAL
.not_cd:
        cmp     byte [dsk_edd], 0
        je      .no_edd
        mov     ah, ATTR_OK
        mov     si, msg_edd_yes
        call    puts_attr
        jmp     .disk_geo
.no_edd:
        mov     ah, ATTR_WARN
        mov     si, msg_edd_no
        call    puts_attr
.disk_geo:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_geo
        call    puts
        mov     ax, [dsk_spt]
        call    putdec16
        mov     si, msg_sect_head
        call    puts
        mov     ax, [dsk_heads]
        call    putdec16
        call    newline

        ; ------------------------------------------------------------
        ; 1.5 CPU が 64bit かどうか
        ; ------------------------------------------------------------
        ; カーネルは x86_64。ロングモードが無い CPU では動かない。
        ;
        ; ここで見ずに進むと、カーネルの展開部が自前で判定して
        ;   no_longmode: hlt / jmp $  (無言の永久停止)
        ; に落ちる。画面は真っ黒、キー入力も効かず、理由がどこにも
        ; 出ない。実際これで何時間も溶かした。
        ;
        ; 仮想環境で「ゲストの種類」を 32bit にしていると、
        ; ホストが 64bit でも CPUID からロングモードのビットが
        ; 消されるので、実機より先に VM で踏むことが多い。
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_cpu
        call    puts
        call    check_longmode
        jc      .no_lm
        mov     ah, ATTR_OK
        mov     si, msg_cpu_ok
        call    puts_attr
        call    newline
        jmp     .do_mem
.no_lm:
        mov     ah, ATTR_ERR
        mov     si, msg_cpu_no
        call    puts_attr
        call    newline
        call    newline
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_cpu_hint1
        call    puts
        call    newline
        mov     si, msg_cpu_hint2
        call    puts
        call    newline
        jmp     fatal

.do_mem:
        ; ------------------------------------------------------------
        ; 2. メモリマップ
        ; ------------------------------------------------------------
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_mem
        call    puts
        call    detect_memory
        jc      .mem_failed
        call    show_mem_result
        call    newline
        jmp     .do_a20
.mem_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_mem_fail
        call    puts_attr
        jmp     fatal

        ; ------------------------------------------------------------
        ; 3. A20
        ; ------------------------------------------------------------
.do_a20:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_a20
        call    puts
        call    enable_a20
        jc      .a20_failed
        mov     ah, ATTR_OK
        mov     si, msg_ok
        call    puts_attr
        call    newline
        jmp     .do_unreal
.a20_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_fail
        call    puts_attr
        jmp     fatal

        ; ------------------------------------------------------------
        ; 4. アンリアルモード (1MB 超に書けるようにする)
        ; ------------------------------------------------------------
.do_unreal:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_unreal
        call    puts
        call    enter_unreal
        call    check_unreal
        jc      .unreal_failed
        mov     ah, ATTR_OK
        mov     si, msg_ok
        call    puts_attr
        call    newline
        jmp     .read_ptbl
.unreal_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_fail
        call    puts_attr
        jmp     fatal

        ; ------------------------------------------------------------
        ; 5. ペイロードテーブルを読む
        ; ------------------------------------------------------------
.read_ptbl:
        mov     si, msg_ptbl
        call    puts
        ; CD から起動したときは、ペイロードは ISO の中の別の場所にある。
        ; その位置は ISO を作るときに cd_ptbl_lba へ書き込まれる。
        ;
        ; 見つからなければ、もう一方の置き方でもう一度試す。
        ; CD かどうかの判定は BIOS 頼みで、外す機種がある。
        ; 判定を間違えても起動できなくならないように、
        ; 両方見てから諦めることにした。
        mov     byte [ptbl_retry], 0
.ptbl_try:
        xor     ax, ax
        mov     es, ax
        mov     bx, PTBL_OFF
        mov     eax, PTBL_LBA
        cmp     byte [dsk_cdrom], 0
        je      .ptbl_lba_ok
        mov     eax, [cd_ptbl_lba]
        shl     eax, 2                  ; 2048 → 512 バイト単位に直す
.ptbl_lba_ok:
        mov     cx, 1
        call    disk_read
        jc      .ptbl_next

        ; マジック "MYOSPLD2" の確認
        mov     si, PTBL_OFF + PT_MAGIC
        mov     di, magic_str
        mov     cx, 8
        repe    cmpsb
        je      .ptbl_found

.ptbl_next:
        cmp     byte [ptbl_retry], 0
        jne     .ptbl_failed
        mov     byte [ptbl_retry], 1
        ; 置き方を間違えていたのだから、セクタの大きさの見方も入れ替える。
        xor     byte [dsk_cdrom], 1
        jmp     .ptbl_try

.ptbl_found:
        mov     ah, ATTR_OK
        mov     si, msg_ok
        call    puts_attr
        call    newline
        jmp     .read_hdr
.ptbl_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_ptbl_fail
        call    puts_attr
        jmp     fatal

        ; ------------------------------------------------------------
        ; 6. bzImage の setup ヘッダを解析
        ; ------------------------------------------------------------
.read_hdr:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_hdr
        call    puts

        mov     ax, SCRATCH_SEG
        mov     es, ax
        xor     bx, bx
        mov     eax, [PTBL_OFF + PT_SETUP_LBA]
        mov     cx, 4                   ; setup ヘッダは 0x268 まで伸びるので 4 セクタ読む
        call    disk_read
        jc      .hdr_failed

        mov     ax, SCRATCH_SEG
        mov     es, ax
        cmp     word [es:0x1FE], 0xAA55         ; ブートシグネチャ
        jne     .hdr_badmagic
        cmp     dword [es:BP_HDR_MAGIC], 0x53726448   ; 'HdrS' (リトルエンディアン)
        jne     .hdr_badmagic

        mov     ax, [es:BP_VERSION]
        mov     [kver], ax
        mov     al, [es:BP_SETUP_SECTS]
        test    al, al
        jnz     .have_sects
        mov     al, 4                   ; 0 は 4 とみなす (古い規約)
.have_sects:
        movzx   ax, al
        mov     [setup_sects], ax

        ; ヘッダ長 = (0x202 + [0x201]) - 0x1F1
        ; 0x200 の 'EB xx' の xx がヘッダ末尾へのオフセットになっている。
        ; プロトコルのバージョンが上がるとヘッダが伸びるので、決め打ちせず
        ; この値から求めるのが正しい。
        movzx   ax, byte [es:0x201]
        add     ax, 0x202
        sub     ax, BP_SETUP_SECTS
        mov     [hdr_len], ax

        mov     ah, ATTR_OK
        mov     si, msg_hdrs_ok
        call    puts_attr
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_proto
        call    puts
        mov     al, byte [kver + 1]
        call    putdec16w
        mov     al, '.'
        call    putc
        mov     al, byte [kver]
        call    putdec16w
        mov     si, msg_setupsects
        call    puts
        mov     ax, [setup_sects]
        call    putdec16
        call    newline
        jmp     .load_kernel

.hdr_badmagic:
        mov     ah, ATTR_ERR
        mov     si, msg_no_hdrs
        call    puts_attr
        jmp     fatal
.hdr_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_fail
        call    puts_attr
        jmp     fatal

        ; ------------------------------------------------------------
        ; 7. カーネル本体を 0x100000 へ
        ;    本体はファイル先頭から (setup_sects + 1) * 512 バイト目から
        ; ------------------------------------------------------------
.load_kernel:
        mov     si, msg_kernel
        call    puts

        ; 本体はイメージ上で setup 部と分けて置いてあるので、
        ; setup_sects から位置を計算する必要はない。
        mov     ecx, [PTBL_OFF + PT_BODY_SECT]
        test    ecx, ecx
        jz      .kernel_failed
        mov     eax, [PTBL_OFF + PT_BODY_LBA]
        mov     edi, KERNEL_DST
        call    disk_load_high
        jc      .kernel_failed

        mov     ah, ATTR_OK
        mov     si, msg_ok
        call    puts_attr
        call    newline

        ; ------------------------------------------------------------
        ;    initramfs (あれば)
        ; ------------------------------------------------------------
        mov     byte [cur_attr], ATTR_NORMAL
        mov     eax, [PTBL_OFF + PT_ISECT]
        test    eax, eax
        jz      .no_initrd

        mov     si, msg_initrd
        call    puts
        ; puts を挟んだのでレジスタは当てにせず読み直す
        mov     ecx, [PTBL_OFF + PT_ISECT]
        mov     eax, [PTBL_OFF + PT_ILBA]
        mov     edi, INITRD_DST
        call    disk_load_high
        jc      .initrd_failed
        mov     ah, ATTR_OK
        mov     si, msg_ok
        call    puts_attr
        call    newline
        jmp     .find_vbe
.no_initrd:
        mov     ah, ATTR_WARN
        mov     si, msg_no_initrd
        call    puts_attr
        call    newline
        jmp     .find_vbe

.kernel_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_fail
        call    puts_attr
        jmp     fatal
.initrd_failed:
        mov     ah, ATTR_ERR
        mov     si, msg_fail
        call    puts_attr
        jmp     fatal

        ; ------------------------------------------------------------
        ; 8. グラフィックモードを探す (まだ切り替えない)
        ;    フェーズ5 の入口。GUI を描くには LFB のあるモードが要る。
        ; ------------------------------------------------------------
.find_vbe:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_vbe
        call    puts
        ; ペイロードテーブルに希望が書いてあれば、それを最優先で試す。
        ; (設定アプリが書き込む。合うモードが無ければ既定の並びに落ちる)
        mov     ax, [PTBL_OFF + PT_VIDEO_W]
        mov     [vbe_req_w], ax
        mov     ax, [PTBL_OFF + PT_VIDEO_H]
        mov     [vbe_req_h], ax
        mov     al, [PTBL_OFF + PT_VIDEO_BPP]
        mov     [vbe_req_bpp], al

        call    vbe_find
        jc      .no_vbe
        mov     ah, ATTR_OK
        mov     si, msg_vbe_found
        call    puts_attr
        mov     byte [cur_attr], ATTR_NORMAL
        mov     ax, [vbe_w]
        call    putdec16
        mov     al, 'x'
        call    putc
        mov     ax, [vbe_h]
        call    putdec16
        mov     al, 'x'
        call    putc
        mov     ax, [vbe_bpp]
        call    putdec16
        mov     si, msg_vbe_lfb
        call    puts
        mov     eax, [vbe_lfb]
        call    puthex32
        call    newline
        jmp     .build_bp
.no_vbe:
        mov     ah, ATTR_WARN
        mov     si, msg_vbe_none
        call    puts_attr
        call    newline

        ; ------------------------------------------------------------
        ; 9. boot_params (ゼロページ) の構築
        ; ------------------------------------------------------------
.build_bp:
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_bootparams
        call    puts
        call    build_boot_params
        mov     ah, ATTR_OK
        mov     si, msg_ok
        call    puts_attr
        call    newline

        ; ------------------------------------------------------------
        ; 10. カーネルへジャンプ
        ; ------------------------------------------------------------
        mov     byte [cur_attr], ATTR_WARN
        mov     si, msg_jump
        call    puts

        ; 最後にグラフィックモードへ切り替える。
        ; これ以降テキスト出力 (0xB8000) は使えないので、
        ; 表示したいことは全てこの前に済ませておくこと。
        call    vbe_set

        call    jump_to_kernel          ; ここから戻ってこない

; ---------------------------------------------------------------------------
fatal:
        call    newline
        mov     ah, ATTR_ERR
        mov     si, msg_halted
        call    puts_attr
.hang:
        hlt
        jmp     .hang

; ---------------------------------------------------------------------------
; show_mem_result - detect_memory の結果を表示
; ---------------------------------------------------------------------------
show_mem_result:
        cmp     byte [mem_method], 1
        je      .e820
        cmp     byte [mem_method], 2
        je      .e801
        mov     ah, ATTR_WARN
        mov     si, msg_via_88
        jmp     .show
.e820:
        mov     ah, ATTR_OK
        mov     si, msg_via_e820
        jmp     .show
.e801:
        mov     ah, ATTR_WARN
        mov     si, msg_via_e801
.show:
        call    puts_attr
        mov     byte [cur_attr], ATTR_NORMAL
        mov     si, msg_entries
        call    puts
        mov     ax, [e820_count]
        call    putdec16
        mov     si, msg_usable
        call    puts
        mov     ax, [mem_mb]
        call    putdec16
        mov     si, msg_mb
        call    puts
        ret

; ---------------------------------------------------------------------------
; putdec16w - AL (0-255) を 10 進で表示する小道具
; ---------------------------------------------------------------------------
putdec16w:
        push    ax
        movzx   ax, al
        call    putdec16
        pop     ax
        ret

; ---------------------------------------------------------------------------
; build_boot_params - ゼロページを 0x90000 に構築する
;
; ゼロページは 4KB。boot_params は 1MB 未満に置けるので、
; アンリアルモードを使わずリアルモードのセグメント (0x9000) で扱える。
; ---------------------------------------------------------------------------
build_boot_params:
        pusha
        push    ds
        push    es

        ; --- 4KB を 0 で埋める ---
        mov     ax, BOOTPARAMS_SEG
        mov     es, ax
        xor     di, di
        xor     ax, ax
        mov     cx, 4096 / 2
        rep     stosw

        ; --- setup ヘッダを bzImage からそのままコピー ---
        ;     コピー範囲は 0x1F1 から hdr_len バイト
        mov     cx, [hdr_len]           ; DS を差し替える前に読んでおく
        mov     si, BP_SETUP_SECTS
        mov     di, BP_SETUP_SECTS
        mov     ax, SCRATCH_SEG
        mov     ds, ax
        rep     movsb

        xor     ax, ax
        mov     ds, ax                  ; DS を 0 に戻す

        ; --- 必須フィールドを書き換える ---
        mov     ax, BOOTPARAMS_SEG
        mov     es, ax

        mov     byte [es:BP_TYPE_OF_LOADER], 0xFF       ; 自作ローダー

        mov     al, [es:BP_LOADFLAGS]
        or      al, LOADFLAG_LOADED_HIGH                ; 本体は 1MB に置いた
        and     al, ~LOADFLAG_QUIET & 0xFF              ; 起動メッセージを出す
        mov     [es:BP_LOADFLAGS], al

        mov     dword [es:BP_CODE32_START], KERNEL_DST
        mov     dword [es:BP_CMD_LINE_PTR], CMDLINE_LIN

        ; --- initramfs ---
        mov     eax, [PTBL_OFF + PT_ISIZE]
        test    eax, eax
        jz      .no_initrd
        mov     dword [es:BP_RAMDISK_IMAGE], INITRD_DST
        mov     [es:BP_RAMDISK_SIZE], eax
.no_initrd:

        ; --- screen_info ---
        ; これを埋めておかないとカーネルがコンソールを立ち上げられず
        ; 画面に何も出ない。
        mov     byte [es:BP_ORIG_X], 0
        mov     byte [es:BP_ORIG_Y], 0
        mov     word [es:BP_ORIG_VIDEO_PAGE], 0
        mov     byte [es:BP_ORIG_VIDEO_MODE], 0x03      ; 80x25 16色テキスト
        mov     byte [es:BP_ORIG_VIDEO_COLS], 80
        mov     byte [es:BP_ORIG_VIDEO_LINES], 25
        mov     word [es:BP_ORIG_VIDEO_POINTS], 16      ; フォント高さ

        cmp     byte [vbe_ok], 0
        jne     .vlfb
        ; VBE が使えないときはテキストモードのまま。
        ; arch/x86/boot/video-vga.c が VGA テキストで 1 を入れているのに合わせる。
        mov     byte [es:BP_ORIG_VIDEO_ISVGA], 1
        jmp     .video_done

.vlfb:
        ; VESA リニアフレームバッファ。
        ; orig_video_isVGA = VIDEO_TYPE_VLFB が「LFB を使っている」合図で、
        ; これを見て vesafb ドライバが /dev/fb0 を作ってくれる。
        mov     byte [es:BP_ORIG_VIDEO_ISVGA], VIDEO_TYPE_VLFB
        mov     ax, [vbe_w]
        mov     [es:BP_LFB_WIDTH], ax
        mov     ax, [vbe_h]
        mov     [es:BP_LFB_HEIGHT], ax
        mov     ax, [vbe_bpp]
        mov     [es:BP_LFB_DEPTH], ax
        mov     eax, [vbe_lfb]
        mov     [es:BP_LFB_BASE], eax
        movzx   eax, word [vbe_total_mem]       ; 64KB ブロック単位のまま渡す
        mov     [es:BP_LFB_SIZE], eax
        mov     ax, [vbe_pitch]
        mov     [es:BP_LFB_LINELENGTH], ax
        mov     al, [vbe_red_size]
        mov     [es:BP_RED_SIZE], al
        mov     al, [vbe_red_pos]
        mov     [es:BP_RED_POS], al
        mov     al, [vbe_green_size]
        mov     [es:BP_GREEN_SIZE], al
        mov     al, [vbe_green_pos]
        mov     [es:BP_GREEN_POS], al
        mov     al, [vbe_blue_size]
        mov     [es:BP_BLUE_SIZE], al
        mov     al, [vbe_blue_pos]
        mov     [es:BP_BLUE_POS], al
        mov     al, [vbe_rsvd_size]
        mov     [es:BP_RSVD_SIZE], al
        mov     al, [vbe_rsvd_pos]
        mov     [es:BP_RSVD_POS], al
        mov     word [es:BP_PAGES], 1
        mov     word [es:BP_VESA_ATTRIBUTES], 0
.video_done:

        ; --- コマンドラインをコピー ---
        mov     si, PTBL_OFF + PT_CMDLINE
        mov     di, CMDLINE_OFF
        mov     cx, 256
.cmd_loop:
        lodsb
        stosb
        test    al, al
        jz      .cmd_done
        loop    .cmd_loop
        mov     byte [es:di], 0
.cmd_done:

        ; --- E820 メモリマップを 20 バイト単位に詰め直す ---
        ; 取得時は ACPI3.0 拡張属性込みの 24 バイトで受けているが、
        ; ゼロページ側は 20 バイト (base 8 + length 8 + type 4) 単位。
        movzx   cx, byte [e820_count]
        test    cx, cx
        jz      .e820_done
        mov     si, E820_BUF
        mov     di, BP_E820_TABLE
.e820_loop:
        push    cx
        push    si
        mov     cx, E820_ZP_ENTSZ
        rep     movsb                   ; 20 バイトだけ写す
        pop     si
        add     si, E820_ENTSZ          ; 元は 24 バイト刻み
        pop     cx
        loop    .e820_loop
.e820_done:
        mov     al, [e820_count]
        mov     [es:BP_E820_ENTRIES], al

        pop     es
        pop     ds
        popa
        ret

; ---------------------------------------------------------------------------
; check_longmode - CPU がロングモード (x86_64) を持つか調べる
;   出力: CF=0 あり / CF=1 無し
;
; 手順は 2 段。まず拡張 CPUID が使えるかを確かめてから中身を見る。
; いきなり 0x80000001 を叩くと、拡張に対応していない古い CPU では
; 別の葉の値が返ってきて誤判定する。
; ---------------------------------------------------------------------------
check_longmode:
        push    eax
        push    ebx
        push    ecx
        push    edx

        ; 拡張 CPUID の最大葉番号を聞く
        mov     eax, 0x80000000
        cpuid
        cmp     eax, 0x80000001
        jb      .none                   ; 0x80000001 が無い = 32bit 専用

        mov     eax, 0x80000001
        cpuid
        test    edx, 1 << 29            ; EDX bit29 = LM (ロングモード)
        jz      .none

        clc
        jmp     .out
.none:
        stc
.out:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; ---------------------------------------------------------------------------
; jump_to_kernel - プロテクトモードに入り、32bit エントリポイントへ渡す
; ---------------------------------------------------------------------------
jump_to_kernel:
        cli

        ; NMI も止める (CMOS インデックスレジスタ bit7)
        in      al, 0x70
        or      al, 0x80
        out     0x70, al
        in      al, 0x71

        lgdt    [gdt_desc]

        mov     eax, cr0
        or      eax, 1                  ; PE = 1 (ページングは触らない → PG=0)
        mov     cr0, eax

        jmp     GDT_CODE32:pm32_entry

BITS 32
pm32_entry:
        mov     ax, GDT_DATA32          ; = __BOOT_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, PM_STACK

        ; boot.rst の規約どおりレジスタを整える
        mov     esi, BOOTPARAMS_LIN     ; ESI = boot_params の物理アドレス
        xor     ebp, ebp
        xor     edi, edi
        xor     ebx, ebx

        jmp     KERNEL_DST              ; CS は既に __BOOT_CS なので near jump

BITS 16

; --- 文字列 ----------------------------------------------------------------
msg_title:      db 'myOS Stage2  -  Phase2-B: Linux Boot Protocol loader', 0
msg_disk:       db 'Disk        : ', 0
msg_edd_yes:    db 'INT 13h extensions (LBA) available ', 0
msg_edd_no:     db 'no extensions, using CHS ', 0
msg_geo:        db '/ geometry ', 0
msg_sect_head:  db ' sect/track, ', 0
msg_cpu:        db 'CPU         : ', 0
msg_cpu_ok:     db 'x86_64 (long mode) available ', 0
msg_cpu_no:     db 'this CPU has no 64-bit (long mode) support', 0
msg_cpu_hint1:  db '  myOS needs a 64-bit x86 CPU. It cannot run here.', 0
msg_cpu_hint2:  db '  On a virtual machine, set the guest type to a 64-bit OS.', 0
msg_mem:        db 'Memory map  : ', 0
msg_via_e820:   db 'INT 15h E820h', 0
msg_via_e801:   db 'INT 15h E801h', 0
msg_via_88:     db 'INT 15h AH=88h', 0
msg_entries:    db ' / ', 0
msg_usable:     db ' entries / usable ', 0
msg_mb:         db ' MB', 0
msg_mem_fail:   db 'FAILED (no BIOS method worked)', 0
msg_a20:        db 'A20 gate    : ', 0
msg_unreal:     db 'Unreal mode : ', 0
msg_ptbl:       db 'Payload tbl : ', 0
msg_ptbl_fail:  db 'NOT FOUND (build the image with --kernel)', 0
msg_hdr:        db 'bzImage hdr : ', 0
msg_hdrs_ok:    db "'HdrS' found ", 0
msg_no_hdrs:    db "no 'HdrS' magic - not a bzImage", 0
msg_proto:      db '/ boot protocol ', 0
msg_setupsects: db ' / setup_sects ', 0
msg_cdrom:      db 'Boot medium : CD-ROM (El Torito, 2048-byte sectors)', 0
msg_kernel:     db 'Booting now', 0

; ISO を作るときに build_iso.py が書き換える場所。
; 'CDPT' を目印に探して、直後の 4 バイトへペイロードの LBA
; (2048 バイト単位) を入れる。ここが 0 のままだと CD からは起動できない。
align 4
cd_ptbl_magic:  db 'CDPT'
cd_ptbl_lba:    dd 0
ptbl_retry:     db 0
msg_initrd:     db 'initramfs   : loading ', 0
msg_no_initrd:  db 'initramfs   : none', 0
msg_vbe:        db 'VESA (VBE)  : ', 0
msg_vbe_found:  db 'mode found ', 0
msg_vbe_lfb:    db '  LFB @ 0x', 0
msg_vbe_none:   db 'not available - staying in text mode', 0
msg_bootparams: db 'boot_params : building zero page at 0x90000 ', 0
msg_jump:       db 'Jumping to kernel entry (ESI=boot_params, EBX=EBP=EDI=0)...', 0
msg_ok:         db 'OK', 0
msg_fail:       db 'FAILED', 0
msg_halted:     db 'Halted.', 0
magic_str:      db 'MYOSPLD2'

; --- 変数 ------------------------------------------------------------------
boot_drive:     db 0
kver:           dw 0
setup_sects:    dw 0
hdr_len:        dw 0

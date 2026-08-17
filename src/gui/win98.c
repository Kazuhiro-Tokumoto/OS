/* ==========================================================================
 * win98.c  -  myOS の GUI 層 (フェーズ5・6の第一歩)
 *
 * Windows 98 もどきのデスクトップをリニアフレームバッファに直接描く。
 *
 * 位置づけ:
 *   カーネルは Linux を採用したので、GUI はカーネル内ではなく
 *   ユーザーランドのプログラムとして作る。
 *     自作ブートローダーが VBE でグラフィックモードを設定
 *       -> カーネルの vesafb が /dev/fb0 として見せる
 *         -> このプログラムがそこへ直接ピクセルを書く
 *   将来ウィンドウシステムに育てるとしても、この位置づけは変わらない。
 *
 * libc は使わず syscall を直接叩いている。
 * (引き継ぎ資料の方針では最終的に glibc/musl をそのまま流用するので、
 *  これは「まだ何も無い段階で動かすための足場」)
 *
 * ビルド:
 *   gcc -static -nostdlib -nostartfiles -ffreestanding -Os -o win98 win98.c
 * ========================================================================== */

#include <linux/fb.h>
#include "font8x8.h"

/* --- syscall ------------------------------------------------------------ */
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return r;
}

#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_mmap        9
#define SYS_ioctl      16
#define SYS_nanosleep  35

#define O_RDONLY   0
#define O_RDWR     2
#define O_NONBLOCK 0x800

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/* 点 (px,py) が矩形の中にあるか */
static int in_rect(long px, long py, long x, long y, long w, long h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

struct timespec { long tv_sec; long tv_nsec; };

static unsigned long slen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* 進捗はシリアル (=/dev/console) に出す。画面は GUI が占有するため。 */
static void log_str(const char *s) { sys3(SYS_write, 2, (long)s, (long)slen(s)); }

static void log_num(unsigned long v)
{
    char buf[24];
    int i = 23;
    buf[i--] = 0;
    if (!v) buf[i--] = '0';
    while (v) { buf[i--] = (char)('0' + v % 10); v /= 10; }
    log_str(&buf[i + 1]);
}

/* --- フレームバッファ ---------------------------------------------------- */
/* 描画は全て裏バッファ (back) に対して行い、1 フレーム分描き終えてから
 * まとめて画面へ転送する。直接描くとカーソルやウィンドウを動かすたびに
 * ちらつくため。 */
static unsigned char *fb;       /* mmap した /dev/fb0 */
static unsigned char *back;     /* 裏バッファ (同じ pitch/bpp) */
static unsigned long  fb_pitch;
static unsigned long  fb_bytes_pp;
static unsigned long  fb_w, fb_h;
static unsigned long  fb_size;

/* Windows 98 の標準的な配色 */
#define C_DESKTOP   0x008080u   /* ティール */
#define C_FACE      0xC0C0C0u   /* ボタン面 */
#define C_LIGHT     0xFFFFFFu   /* ハイライト */
#define C_SHADOW    0x808080u   /* 影 */
#define C_DKSHADOW  0x000000u   /* 濃い影 */
#define C_TITLE1    0x000080u   /* タイトルバー (濃) */
#define C_TITLE2    0x1084D0u   /* タイトルバー (淡) グラデーション用 */
#define C_TITLETXT  0xFFFFFFu
#define C_TEXT      0x000000u
#define C_WHITE     0xFFFFFFu
#define C_BLUE      0x0000A0u

static void put_px(long x, long y, unsigned int c)
{
    if (x < 0 || y < 0 || (unsigned long)x >= fb_w || (unsigned long)y >= fb_h)
        return;
    unsigned char *p = back + (unsigned long)y * fb_pitch
                            + (unsigned long)x * fb_bytes_pp;
    p[0] = (unsigned char)(c & 0xFF);         /* B */
    p[1] = (unsigned char)((c >> 8) & 0xFF);  /* G */
    p[2] = (unsigned char)((c >> 16) & 0xFF); /* R */
    if (fb_bytes_pp == 4)
        p[3] = 0;
}

/* 32bpp のときは 1 行ぶんを 32bit 単位でまとめて埋める。
 * 全画面を毎フレーム描き直すので、ここが遅いと目に見えて重くなる。 */
static void fill(long x, long y, long w, long h, unsigned int c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (long)fb_w) w = (long)fb_w - x;
    if (y + h > (long)fb_h) h = (long)fb_h - y;
    if (w <= 0 || h <= 0) return;

    if (fb_bytes_pp == 4) {
        for (long j = 0; j < h; j++) {
            unsigned int *p = (unsigned int *)(back + (unsigned long)(y + j) * fb_pitch)
                              + x;
            for (long i = 0; i < w; i++)
                p[i] = c;
        }
    } else {
        for (long j = 0; j < h; j++)
            for (long i = 0; i < w; i++)
                put_px(x + i, y + j, c);
    }
}

/* 裏バッファを画面へ転送する */
static void present(void)
{
    unsigned long n = fb_size;
    if (fb_bytes_pp == 4 && (n & 3) == 0) {
        unsigned int *d = (unsigned int *)fb;
        unsigned int *s = (unsigned int *)back;
        for (unsigned long i = 0; i < n / 4; i++)
            d[i] = s[i];
    } else {
        for (unsigned long i = 0; i < n; i++)
            fb[i] = back[i];
    }
}

static void hline(long x, long y, long w, unsigned int c)
{
    for (long i = 0; i < w; i++) put_px(x + i, y, c);
}

static void vline(long x, long y, long h, unsigned int c)
{
    for (long i = 0; i < h; i++) put_px(x, y + i, c);
}

/* Win98 の立体枠。raised=1 で盛り上がり、0 で凹み。
 * 外側 1px と内側 1px の 2 重になっているのがこの時代の作法。 */
static void bevel(long x, long y, long w, long h, int raised)
{
    unsigned int out_tl = raised ? C_LIGHT   : C_SHADOW;
    unsigned int out_br = raised ? C_DKSHADOW : C_LIGHT;
    unsigned int in_tl  = raised ? C_FACE    : C_DKSHADOW;
    unsigned int in_br  = raised ? C_SHADOW  : C_FACE;

    hline(x, y, w, out_tl);
    vline(x, y, h, out_tl);
    hline(x, y + h - 1, w, out_br);
    vline(x + w - 1, y, h, out_br);

    hline(x + 1, y + 1, w - 2, in_tl);
    vline(x + 1, y + 1, h - 2, in_tl);
    hline(x + 1, y + h - 2, w - 2, in_br);
    vline(x + w - 2, y + 1, h - 2, in_br);
}

/* --- 文字描画 ------------------------------------------------------------ */
static void draw_char(long x, long y, char ch, unsigned int fg, int scale)
{
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST)
        return;
    const unsigned char *g = font8x8[(unsigned char)ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        unsigned char bits = g[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col)) {
                if (scale == 1) {
                    put_px(x + col, y + row, fg);
                } else {
                    fill(x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
    }
}

static void draw_text(long x, long y, const char *s, unsigned int fg, int scale)
{
    for (; *s; s++) {
        draw_char(x, y, *s, fg, scale);
        x += FONT_W * scale;
    }
}

/* 影付き文字 (タイトルバーなどで使う) */
static void draw_text_sh(long x, long y, const char *s, unsigned int fg,
                         unsigned int sh, int scale)
{
    draw_text(x + 1, y + 1, s, sh, scale);
    draw_text(x, y, s, fg, scale);
}

/* --- 画面の部品 ---------------------------------------------------------- */

#define TASKBAR_H 30

static void draw_button(long x, long y, long w, long h, const char *label,
                        int pressed)
{
    fill(x, y, w, h, C_FACE);
    bevel(x, y, w, h, !pressed);
    long tx = x + (w - (long)slen(label) * FONT_W) / 2 + (pressed ? 1 : 0);
    long ty = y + (h - FONT_H) / 2 + (pressed ? 1 : 0);
    draw_text(tx, ty, label, C_TEXT, 1);
}

/* Win98 のロゴ代わり: 4 色の小さな旗 */
static void draw_flag(long x, long y)
{
    static const unsigned int cols[4] = {
        0xE04040u, 0x40C040u, 0x4060E0u, 0xE0E040u
    };
    for (int i = 0; i < 4; i++) {
        long dx = (i & 1) * 7;
        long dy = (i >> 1) * 7;
        fill(x + dx, y + dy + (dx ? 0 : 1), 6, 6, cols[i]);
    }
}

static void draw_taskbar(void)
{
    long y = (long)fb_h - TASKBAR_H;
    fill(0, y, (long)fb_w, TASKBAR_H, C_FACE);
    hline(0, y, (long)fb_w, C_LIGHT);

    /* スタートボタン */
    fill(3, y + 4, 68, TASKBAR_H - 8, C_FACE);
    bevel(3, y + 4, 68, TASKBAR_H - 8, 1);
    draw_flag(8, y + 9);
    draw_text(26, y + 4 + (TASKBAR_H - 8 - FONT_H) / 2, "Start", C_TEXT, 1);

    /* 区切り */
    vline(76, y + 5, TASKBAR_H - 10, C_SHADOW);
    vline(77, y + 5, TASKBAR_H - 10, C_LIGHT);

    /* タスクボタン (押された状態で表示) */
    draw_button(84, y + 4, 150, TASKBAR_H - 8, "myOS - Welcome", 1);

    /* 時計 (凹み枠) */
    long cw = 62;
    long cx = (long)fb_w - cw - 4;
    fill(cx, y + 4, cw, TASKBAR_H - 8, C_FACE);
    bevel(cx, y + 4, cw, TASKBAR_H - 8, 0);
    draw_text(cx + 8, y + 4 + (TASKBAR_H - 8 - FONT_H) / 2, "12:00", C_TEXT, 1);
}

/* アイコンは cell_w 幅のセルの中央に描く。
 * ラベルはアイコン本体より横に広がるので、セル幅を基準に中央揃えしないと
 * 画面の端で文字が切れてしまう。 */
#define ICON_CELL_W 96
#define ICON_BODY_W 32

static void draw_icon(long cell_x, long y, const char *label, unsigned int body)
{
    long x = cell_x + (ICON_CELL_W - ICON_BODY_W) / 2;

    /* 本体 (モニタ風) */
    fill(x + 4, y, 24, 20, body);
    bevel(x + 4, y, 24, 20, 1);
    fill(x + 7, y + 3, 18, 12, C_BLUE);
    /* 台座 */
    fill(x + 10, y + 20, 12, 4, C_FACE);
    fill(x, y + 24, ICON_BODY_W, 5, C_FACE);
    bevel(x, y + 24, ICON_BODY_W, 5, 1);

    /* ラベル (デスクトップなので白抜き + 影) */
    long tw = (long)slen(label) * FONT_W;
    draw_text_sh(cell_x + (ICON_CELL_W - tw) / 2, y + 34, label,
                 C_WHITE, 0x004040u, 1);
}

static void draw_window(long x, long y, long w, long h, const char *title)
{
    /* 枠 */
    fill(x, y, w, h, C_FACE);
    bevel(x, y, w, h, 1);

    /* タイトルバー (横方向グラデーション) */
    long tb_x = x + 4, tb_y = y + 4;
    long tb_w = w - 8, tb_h = 20;
    for (long i = 0; i < tb_w; i++) {
        /* C_TITLE1 -> C_TITLE2 の単純な線形補間 */
        unsigned int r = (0x00 * (tb_w - i) + 0x10 * i) / tb_w;
        unsigned int g = (0x00 * (tb_w - i) + 0x84 * i) / tb_w;
        unsigned int b = (0x80 * (tb_w - i) + 0xD0 * i) / tb_w;
        vline(tb_x + i, tb_y, tb_h, (r << 16) | (g << 8) | b);
    }
    draw_text(tb_x + 4, tb_y + (tb_h - FONT_H) / 2, title, C_TITLETXT, 1);

    /* 右上のボタン 3 つ */
    long bs = 16;
    long bx = tb_x + tb_w - bs - 2;
    long by = tb_y + 2;
    draw_button(bx, by, bs, bs, "X", 0);
    draw_button(bx - bs - 2, by, bs, bs, "[", 0);
    draw_button(bx - 2 * (bs + 2), by, bs, bs, "_", 0);

    /* メニューバー */
    long my = tb_y + tb_h + 2;
    draw_text(x + 8, my + 2, "File", C_TEXT, 1);
    draw_text(x + 8 + 6 * FONT_W, my + 2, "Edit", C_TEXT, 1);
    draw_text(x + 8 + 12 * FONT_W, my + 2, "View", C_TEXT, 1);
    draw_text(x + 8 + 18 * FONT_W, my + 2, "Help", C_TEXT, 1);
    hline(x + 4, my + 14, w - 8, C_SHADOW);
    hline(x + 4, my + 15, w - 8, C_LIGHT);

    /* クライアント領域 (凹み + 白) */
    long cx = x + 6, cy = my + 18;
    long cw = w - 12, ch = h - (cy - y) - 6;
    bevel(cx, cy, cw, ch, 0);
    fill(cx + 2, cy + 2, cw - 4, ch - 4, C_WHITE);

    static const char *lines[] = {
        "Welcome to myOS.",
        "",
        "This desktop is drawn straight into the linear",
        "framebuffer by our own code - no X11, no toolkit.",
        "",
        "  bootloader : stage1 (MBR, 512 bytes)",
        "               stage2_linux (Linux boot protocol)",
        "               VBE mode set by the bootloader",
        "  kernel     : Linux (adopted, not written by us)",
        "  userland   : this program, statically linked,",
        "               no libc, writing to /dev/fb0",
        "",
        "Phase 5 (VGA graphics) has started.",
        0
    };
    long ty = cy + 10;
    for (int i = 0; lines[i]; i++) {
        draw_text(cx + 10, ty, lines[i], C_TEXT, 1);
        ty += FONT_H + 4;
    }
}

/* --- マウスカーソル ------------------------------------------------------ */
/* 12x19 の矢印。'X'=黒 '.'=白 ' '=透明 */
static const char *cursor_bits[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
    0
};

#define CUR_W 12
#define CUR_H 19

static long cur_x = 200, cur_y = 200;

/* 毎フレーム全部描き直すので、下の絵を退避する必要はない。
 * 最後にカーソルを描くだけでよい。 */
static void draw_cursor(void)
{
    for (int j = 0; j < CUR_H && cursor_bits[j]; j++) {
        const char *row = cursor_bits[j];
        for (int i = 0; i < CUR_W && row[i]; i++) {
            if (row[i] == 'X')      put_px(cur_x + i, cur_y + j, 0x000000u);
            else if (row[i] == '.') put_px(cur_x + i, cur_y + j, 0xFFFFFFu);
        }
    }
}

/* --- デスクトップの状態 -------------------------------------------------- */
static long win_x = 140, win_y = 60, win_w = 620, win_h = 400;
static int  start_open = 0;

#define MENU_W 168
#define MENU_ITEM_H 22

static const char *menu_items[] = {
    "Programs", "Documents", "Settings", "Find", "Help", "Run...", "Shut Down",
    0
};

static int menu_count(void)
{
    int n = 0;
    while (menu_items[n]) n++;
    return n;
}

static long menu_h(void) { return menu_count() * MENU_ITEM_H + 8; }
static long menu_y(void) { return (long)fb_h - TASKBAR_H - menu_h(); }

static void draw_start_menu(void)
{
    long x = 2, y = menu_y(), w = MENU_W, h = menu_h();

    fill(x, y, w, h, C_FACE);
    bevel(x, y, w, h, 1);

    /* 左端の縦帯 (Win98 の "Windows 98" と書いてあるアレ) */
    fill(x + 3, y + 3, 20, h - 6, C_TITLE1);
    const char *side = "myOS";
    long sy = y + h - 12;
    for (int i = 0; side[i]; i++) {
        draw_char(x + 9, sy, side[i], C_TITLETXT, 1);
        sy -= FONT_H + 2;
    }

    long iy = y + 4;
    for (int i = 0; menu_items[i]; i++) {
        draw_text(x + 32, iy + (MENU_ITEM_H - FONT_H) / 2, menu_items[i],
                  C_TEXT, 1);
        /* Shut Down の上に区切り線 */
        if (menu_items[i + 1] == 0) {
            hline(x + 26, iy - 3, w - 32, C_SHADOW);
            hline(x + 26, iy - 2, w - 32, C_LIGHT);
        }
        iy += MENU_ITEM_H;
    }
}

/* --- デスクトップ全体 ---------------------------------------------------- */
static void draw_desktop(void)
{
    fill(0, 0, (long)fb_w, (long)fb_h, C_DESKTOP);

    draw_icon(8, 20, "My Computer", C_FACE);
    draw_icon(8, 92, "Recycle Bin", C_FACE);
    draw_icon(8, 164, "My Docs", C_FACE);

    draw_window(win_x, win_y, win_w, win_h, "myOS - Welcome");

    draw_taskbar();
    if (start_open)
        draw_start_menu();

    draw_cursor();
}

/* --- 起動 ---------------------------------------------------------------- */
void _start(void)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    log_str("\n[win98] opening /dev/fb0\n");
    long fd = sys3(SYS_open, (long)"/dev/fb0", O_RDWR, 0);
    if (fd < 0) {
        log_str("[win98] ERROR: cannot open /dev/fb0."
                " Is the framebuffer enabled?\n");
        for (;;) {
            struct timespec ts = { 3600, 0 };
            sys3(SYS_nanosleep, (long)&ts, 0, 0);
        }
    }

    if (sys3(SYS_ioctl, fd, FBIOGET_VSCREENINFO, (long)&vinfo) < 0 ||
        sys3(SYS_ioctl, fd, FBIOGET_FSCREENINFO, (long)&finfo) < 0) {
        log_str("[win98] ERROR: FBIOGET_*SCREENINFO failed\n");
        for (;;) {
            struct timespec ts = { 3600, 0 };
            sys3(SYS_nanosleep, (long)&ts, 0, 0);
        }
    }

    fb_w        = vinfo.xres;
    fb_h        = vinfo.yres;
    fb_pitch    = finfo.line_length;
    fb_bytes_pp = (vinfo.bits_per_pixel + 7) / 8;

    log_str("[win98] framebuffer ");
    log_num(fb_w); log_str("x"); log_num(fb_h);
    log_str("x"); log_num(vinfo.bits_per_pixel);
    log_str("  pitch="); log_num(fb_pitch);
    log_str("  size="); log_num(finfo.smem_len);
    log_str("\n");

    fb_size = finfo.smem_len;

    long map = sys6(SYS_mmap, 0, (long)fb_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map < 0 && map > -4096) {
        log_str("[win98] ERROR: mmap of /dev/fb0 failed\n");
        for (;;) {
            struct timespec ts = { 3600, 0 };
            sys3(SYS_nanosleep, (long)&ts, 0, 0);
        }
    }
    fb = (unsigned char *)map;

    /* 裏バッファは無名メモリを確保して使う */
    long bmap = sys6(SYS_mmap, 0, (long)fb_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bmap < 0 && bmap > -4096) {
        log_str("[win98] ERROR: back buffer mmap failed\n");
        for (;;) {
            struct timespec ts = { 3600, 0 };
            sys3(SYS_nanosleep, (long)&ts, 0, 0);
        }
    }
    back = (unsigned char *)bmap;

    /* ウィンドウを画面の大きさに合わせる */
    win_x = ICON_CELL_W + 40;
    win_w = (long)fb_w - win_x - 60;
    if (win_w > 620) win_w = 620;
    win_h = (long)fb_h - TASKBAR_H - 120;
    if (win_h > 400) win_h = 400;
    if (win_h < 200) win_h = 200;

    cur_x = (long)fb_w / 2;
    cur_y = (long)fb_h / 2;

    draw_desktop();
    present();
    log_str("[win98] desktop drawn\n");

    /* --- マウス --------------------------------------------------------- */
    /* /dev/input/mice は PS/2 互換の 3 バイトパケットを返すので扱いが楽。
     * 無くても致命的ではないので、開けなければカーソルを止めたまま続ける。 */
    long mfd = sys3(SYS_open, (long)"/dev/input/mice", O_RDONLY, 0);
    if (mfd < 0) {
        log_str("[win98] /dev/input/mice not available; cursor is static\n");
        for (;;) {
            struct timespec ts = { 3600, 0 };
            sys3(SYS_nanosleep, (long)&ts, 0, 0);
        }
    }
    log_str("[win98] reading /dev/input/mice\n");

    int  prev_btn = 0;
    int  dragging = 0;
    long drag_dx = 0, drag_dy = 0;

    for (;;) {
        signed char pkt[3];
        long n = sys3(SYS_read, mfd, (long)pkt, 3);
        if (n != 3)
            continue;

        int btn = pkt[0] & 1;           /* 左ボタン */

        cur_x += pkt[1];
        cur_y -= pkt[2];                /* 画面は下向きが正なので反転 */
        if (cur_x < 0) cur_x = 0;
        if (cur_y < 0) cur_y = 0;
        if ((unsigned long)cur_x > fb_w - 1) cur_x = (long)fb_w - 1;
        if ((unsigned long)cur_y > fb_h - 1) cur_y = (long)fb_h - 1;

        /* --- 押した瞬間の判定 --- */
        if (btn && !prev_btn) {
            long tb_y = (long)fb_h - TASKBAR_H;

            if (in_rect(cur_x, cur_y, 3, tb_y + 4, 68, TASKBAR_H - 8)) {
                /* スタートボタン */
                start_open = !start_open;
            } else if (start_open &&
                       in_rect(cur_x, cur_y, 2, menu_y(), MENU_W, menu_h())) {
                /* メニュー項目を選んだ (今は閉じるだけ) */
                start_open = 0;
            } else if (in_rect(cur_x, cur_y,
                               win_x + 4, win_y + 4, win_w - 8, 20)) {
                /* タイトルバーを掴んだ → ドラッグ開始 */
                dragging = 1;
                drag_dx = cur_x - win_x;
                drag_dy = cur_y - win_y;
                start_open = 0;
            } else {
                start_open = 0;
            }
        }
        if (!btn)
            dragging = 0;

        if (dragging) {
            win_x = cur_x - drag_dx;
            win_y = cur_y - drag_dy;
            /* 画面からはみ出しすぎないように留める */
            if (win_x < -(win_w - 80)) win_x = -(win_w - 80);
            if (win_y < 0) win_y = 0;
            if (win_x > (long)fb_w - 80) win_x = (long)fb_w - 80;
            if (win_y > (long)fb_h - TASKBAR_H - 24)
                win_y = (long)fb_h - TASKBAR_H - 24;
        }

        prev_btn = btn;

        draw_desktop();
        present();
    }
}

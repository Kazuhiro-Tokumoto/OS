/* ==========================================================================
 * myos_settings.c  -  Windows 98 風の「画面のプロパティ」
 *
 * 配色を選んで /etc/myos/theme.conf に書き、ウィンドウマネージャに
 * SIGUSR1 を送って即座に反映させる。
 *
 * このアプリ自身も x98.h の色を使って描いているので、
 * 選んだ配色がそのまま自分の見た目に反映される (生きたプレビューになる)。
 *
 * ビルド:
 *   gcc -O2 -o myos-settings myos_settings.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "x98.h"

#define WIN_W 620
#define WIN_H 500

#define PREV_X   12
#define PREV_Y   12
#define PREV_W   (WIN_W - 24)
#define PREV_H   150

#define LIST_X   12
#define LIST_Y   196
#define LIST_W   270
#define LIST_H   116
#define ROW_H    18

#define PAL_X    300
#define PAL_Y    196
#define PAL_CELL 26
#define PAL_COLS 8
#define PAL_ROWS 2

#define SPD_Y    334
#define BTN_Y    (WIN_H - 40)
#define BTN_W    80
#define BTN_H    26

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

/* --- 配色のプリセット (Win98 の配色に寄せてある) ------------------------- */
typedef struct {
    const char  *name;
    unsigned int desktop, face, title1, title2, titletxt, text, select_bg;
} Scheme;

static const Scheme schemes[] = {
    { "Windows Standard", 0x008080, 0xC0C0C0, 0x000080, 0x1084D0,
      0xFFFFFF, 0x000000, 0x000080 },
    { "Desert",           0x87794E, 0xD5CCBB, 0x80725B, 0xB0A184,
      0xFFFFFF, 0x000000, 0x80725B },
    { "Eggplant",         0x213B57, 0x9F9F9F, 0x40364D, 0x6D5F7D,
      0xFFFFFF, 0x000000, 0x40364D },
    { "Rainy Day",        0x4E6E8E, 0x9EA5B0, 0x27394B, 0x53718F,
      0xFFFFFF, 0x000000, 0x27394B },
    { "Slate",            0x5C5C5C, 0x808080, 0x3A4B5C, 0x5A7B9C,
      0xFFFFFF, 0x000000, 0x3A4B5C },
    { "Rose",             0x8E6A6A, 0xC4B4B4, 0x7A4A4A, 0xB07070,
      0xFFFFFF, 0x000000, 0x7A4A4A },
};
#define N_SCHEMES ((int)(sizeof(schemes) / sizeof(schemes[0])))

/* デスクトップの色だけ差し替えたいとき用の 16 色 */
static const unsigned int palette[PAL_COLS * PAL_ROWS] = {
    0x000000, 0x800000, 0x008000, 0x808000,
    0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

static const struct { const char *label; int ms; } speeds[] = {
    { "Slow",   1000 },
    { "Normal",  700 },
    { "Fast",    400 },
};
#define N_SPEEDS 3

static int cur_scheme = 0;

/* --- 反映 ---------------------------------------------------------------- */
static void apply_scheme(int i)
{
    const Scheme *s = &schemes[i];
    x98.theme.desktop   = s->desktop;
    x98.theme.face      = s->face;
    x98.theme.title1    = s->title1;
    x98.theme.title2    = s->title2;
    x98.theme.titletxt  = s->titletxt;
    x98.theme.text      = s->text;
    x98.theme.select_bg = s->select_bg;
    x98_apply(&x98);
    cur_scheme = i;
}

static void save_theme(void)
{
    /* 設定ディレクトリが無ければ作る */
    if (system("mkdir -p /etc/myos") != 0) { /* 失敗しても書き込みで分かる */ }

    FILE *f = fopen(X98_THEME_CONF, "w");
    if (!f) {
        fprintf(stderr, "[myos-settings] cannot write %s\n", X98_THEME_CONF);
        return;
    }
    fprintf(f, "# myOS のテーマ。myos-settings が書き出す。\n");
    fprintf(f, "# 値は 6 桁の 16 進 RGB。\n");
    fprintf(f, "desktop     = %06X\n", x98.theme.desktop);
    fprintf(f, "face        = %06X\n", x98.theme.face);
    fprintf(f, "title1      = %06X\n", x98.theme.title1);
    fprintf(f, "title2      = %06X\n", x98.theme.title2);
    fprintf(f, "titletext   = %06X\n", x98.theme.titletxt);
    fprintf(f, "text        = %06X\n", x98.theme.text);
    fprintf(f, "select      = %06X\n", x98.theme.select_bg);
    fprintf(f, "dblclick_ms = %d\n",   x98.theme.dblclick_ms);
    fclose(f);

    /* ウィンドウマネージャに読み直させる */
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/sh", "sh", "-c", "pkill -USR1 -x myos-wm", (char *)NULL);
        _exit(127);
    }
}

/* --- 描画 ---------------------------------------------------------------- */
static void draw_preview(void)
{
    x98_bevel(&x98, win, PREV_X, PREV_Y, PREV_W, PREV_H, 0);

    int ix = PREV_X + 2, iy = PREV_Y + 2;
    int iw = PREV_W - 4, ih = PREV_H - 4;

    /* 小さなデスクトップ */
    x98_fill(&x98, win, ix, iy, iw, ih, x98.desktop);

    /* 小さなウィンドウ */
    int wx = ix + 40, wy = iy + 18, ww = iw - 120, wh = ih - 52;
    x98_fill(&x98, win, wx, wy, ww, wh, x98.face);
    x98_bevel(&x98, win, wx, wy, ww, wh, 1);
    x98_titlebar(&x98, win, wx + 3, wy + 3, ww - 6, 16, "Active Window");
    x98_bevel(&x98, win, wx + 3, wy + 22, ww - 6, wh - 28, 0);
    x98_fill(&x98, win, wx + 5, wy + 24, ww - 10, wh - 32, x98.white);
    x98_text(&x98, win, wx + 10, wy + 30, "Window Text", x98.text);
    x98_fill(&x98, win, wx + 10, wy + 46, 90, 14, x98.select_bg);
    x98_text(&x98, win, wx + 13, wy + 47, "Selected", x98.titletxt);

    /* 小さなタスクバー */
    x98_fill(&x98, win, ix, iy + ih - 16, iw, 16, x98.face);
    x98_hline(&x98, win, ix, iy + ih - 16, iw, x98.light);
    x98_button(&x98, win, ix + 2, iy + ih - 14, 44, 12, "Start", 0);
}

static void draw_list(void)
{
    x98_text(&x98, win, LIST_X, LIST_Y - 16, "Scheme:", x98.text);
    x98_bevel(&x98, win, LIST_X, LIST_Y, LIST_W, LIST_H, 0);
    x98_fill(&x98, win, LIST_X + 2, LIST_Y + 2, LIST_W - 4, LIST_H - 4,
             x98.white);

    for (int i = 0; i < N_SCHEMES; i++) {
        int ry = LIST_Y + 2 + i * ROW_H;
        unsigned long fg = x98.text;
        if (i == cur_scheme) {
            x98_fill(&x98, win, LIST_X + 2, ry, LIST_W - 4, ROW_H,
                     x98.select_bg);
            fg = x98.titletxt;
        }
        x98_text(&x98, win, LIST_X + 8, ry + (ROW_H - x98_text_h(&x98)) / 2,
                 schemes[i].name, fg);
    }
}

static void draw_palette(void)
{
    x98_text(&x98, win, PAL_X, PAL_Y - 16, "Desktop color:", x98.text);
    for (int r = 0; r < PAL_ROWS; r++) {
        for (int c = 0; c < PAL_COLS; c++) {
            int i = r * PAL_COLS + c;
            int px = PAL_X + c * PAL_CELL;
            int py = PAL_Y + r * PAL_CELL;
            x98_bevel(&x98, win, px, py, PAL_CELL - 2, PAL_CELL - 2, 0);
            x98_fill(&x98, win, px + 2, py + 2, PAL_CELL - 6, PAL_CELL - 6,
                     x98_rgb24(&x98, palette[i]));
            if (palette[i] == x98.theme.desktop) {
                /* 選択中の色に印を付ける */
                x98_hline(&x98, win, px, py + PAL_CELL - 2, PAL_CELL - 2,
                          x98.text);
            }
        }
    }
}

static void draw_speed(void)
{
    x98_text(&x98, win, PAL_X, SPD_Y - 16, "Double-click speed:", x98.text);
    for (int i = 0; i < N_SPEEDS; i++) {
        int px = PAL_X + i * 74;
        x98_button(&x98, win, px, SPD_Y, 70, 24, speeds[i].label,
                   x98.theme.dblclick_ms == speeds[i].ms);
    }
}

static void draw_buttons(void)
{
    int bx = WIN_W - (BTN_W + 8) * 3 - 4;
    x98_button(&x98, win, bx, BTN_Y, BTN_W, BTN_H, "OK", 0);
    x98_button(&x98, win, bx + BTN_W + 8, BTN_Y, BTN_W, BTN_H, "Apply", 0);
    x98_button(&x98, win, bx + (BTN_W + 8) * 2, BTN_Y, BTN_W, BTN_H,
               "Cancel", 0);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);
    draw_preview();
    draw_list();
    draw_palette();
    draw_speed();
    draw_buttons();
}

/* --- 本体 ---------------------------------------------------------------- */
int main(void)
{
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-settings] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);

    /* 今の設定がどのプリセットに一番近いか探しておく */
    for (int i = 0; i < N_SCHEMES; i++)
        if (schemes[i].desktop == x98.theme.desktop &&
            schemes[i].face == x98.theme.face) { cur_scheme = i; break; }

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XStoreName(dpy, win, "Display Properties");

    /* 大きさを変えられると配置が崩れるので固定にする */
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = WIN_W;
    hints.min_height = hints.max_height = WIN_H;
    XSetWMNormalHints(dpy, win, &hints);

    XMapWindow(dpy, win);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (!ev.xexpose.count) redraw();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a_wm_delete) {
                XCloseDisplay(dpy);
                return 0;
            }
            break;

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            /* 配色の一覧 */
            if (mx >= LIST_X && mx < LIST_X + LIST_W &&
                my >= LIST_Y + 2 && my < LIST_Y + LIST_H) {
                int i = (my - LIST_Y - 2) / ROW_H;
                if (i >= 0 && i < N_SCHEMES) {
                    apply_scheme(i);
                    XSetWindowBackground(dpy, win, x98.face);
                    redraw();
                }
                break;
            }

            /* デスクトップの色 */
            if (mx >= PAL_X && mx < PAL_X + PAL_COLS * PAL_CELL &&
                my >= PAL_Y && my < PAL_Y + PAL_ROWS * PAL_CELL) {
                int c = (mx - PAL_X) / PAL_CELL;
                int r = (my - PAL_Y) / PAL_CELL;
                int i = r * PAL_COLS + c;
                if (i >= 0 && i < PAL_COLS * PAL_ROWS) {
                    x98.theme.desktop = palette[i];
                    x98_apply(&x98);
                    redraw();
                }
                break;
            }

            /* ダブルクリックの速さ */
            if (my >= SPD_Y && my < SPD_Y + 24) {
                for (int i = 0; i < N_SPEEDS; i++) {
                    int px = PAL_X + i * 74;
                    if (mx >= px && mx < px + 70) {
                        x98.theme.dblclick_ms = speeds[i].ms;
                        redraw();
                        break;
                    }
                }
                break;
            }

            /* OK / Apply / Cancel */
            if (my >= BTN_Y && my < BTN_Y + BTN_H) {
                int bx = WIN_W - (BTN_W + 8) * 3 - 4;
                if (mx >= bx && mx < bx + BTN_W) {              /* OK */
                    save_theme();
                    XCloseDisplay(dpy);
                    return 0;
                }
                if (mx >= bx + BTN_W + 8 && mx < bx + 2 * BTN_W + 8) {
                    save_theme();                               /* Apply */
                    break;
                }
                if (mx >= bx + (BTN_W + 8) * 2) {               /* Cancel */
                    XCloseDisplay(dpy);
                    return 0;
                }
            }
            break;
        }

        default:
            break;
        }
    }
    return 0;
}

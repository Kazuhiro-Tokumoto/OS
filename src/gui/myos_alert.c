/* ==========================================================================
 * myos_alert.c  -  Win98 風のメッセージボックス
 *
 *   myos-alert [-warn|-info|-error] "タイトル" "本文"
 *
 * 本文は \n で改行できる。
 * ウイルス検知の知らせなど、常駐から出したいときに使う。
 * 常駐側がシェルで組み立てなくて済むよう、引数は 2 つだけにしてある。
 *
 * ビルド:
 *   gcc -O2 -o myos-alert myos_alert.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x98.h"

#define MAX_LINES 24
#define BTN_W  80
#define BTN_H  24
#define PAD    16
#define ICON   32

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

static char  *lines[MAX_LINES];
static int    n_lines = 0;
static int    win_w = 380, win_h = 150;
static int    kind = 0;      /* 0 = info / 1 = warn / 2 = error */

/* 丸に「!」「i」「x」。画像を持たずに済ませる。 */
static void draw_icon(int x, int y)
{
    unsigned long ring = x98_rgb24(&x98, kind == 2 ? 0xC00000 :
                                          kind == 1 ? 0xC0C000 : 0x000080);
    XSetForeground(dpy, x98.gc, ring);
    XFillArc(dpy, win, x98.gc, x, y, ICON, ICON, 0, 360 * 64);
    XSetForeground(dpy, x98.gc, x98.white);
    XFillArc(dpy, win, x98.gc, x + 3, y + 3, ICON - 6, ICON - 6, 0, 360 * 64);

    XSetForeground(dpy, x98.gc, ring);
    if (kind == 2) {
        XDrawLine(dpy, win, x98.gc, x + 11, y + 11, x + ICON - 11, y + ICON - 11);
        XDrawLine(dpy, win, x98.gc, x + ICON - 11, y + 11, x + 11, y + ICON - 11);
    } else {
        /* 縦棒と点。! も i も同じ形で、点の位置だけが違う */
        int top = (kind == 1) ? y + 8 : y + 15;
        int bot = (kind == 1) ? y + 20 : y + 25;
        XFillRectangle(dpy, win, x98.gc, x + ICON / 2 - 1, top, 3, bot - top);
        int dy = (kind == 1) ? y + 23 : y + 9;
        XFillRectangle(dpy, win, x98.gc, x + ICON / 2 - 1, dy, 3, 3);
    }
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, win_w, win_h, x98.face);
    draw_icon(PAD, PAD + 2);

    int tx = PAD + ICON + 14;
    int ty = PAD;
    int lh = x98_text_h(&x98) + 4;
    for (int i = 0; i < n_lines; i++)
        x98_text(&x98, win, tx, ty + i * lh, lines[i], x98.text);

    x98_button(&x98, win, (win_w - BTN_W) / 2, win_h - BTN_H - PAD,
               BTN_W, BTN_H, "OK", 0);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if      (!strcmp(argv[i], "-warn"))  kind = 1;
        else if (!strcmp(argv[i], "-error")) kind = 2;
        else if (!strcmp(argv[i], "-info"))  kind = 0;
        else break;
    }
    const char *title = (i < argc) ? argv[i++] : "myOS";
    const char *body  = (i < argc) ? argv[i++] : "";

    /* 本文を \n で行に割る */
    char *copy = strdup(body);
    if (!copy) return 1;
    char *p = copy;
    while (n_lines < MAX_LINES) {
        lines[n_lines++] = p;
        char *nl = strstr(p, "\\n");
        if (!nl) break;
        *nl = 0;
        p = nl + 2;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        /* X が無ければ端末に出す。常駐から呼ばれるので黙って消えないように。 */
        fprintf(stderr, "%s: %s\n", title, body);
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);

    /* 一番長い行に合わせて窓の幅を決める */
    int longest = 0;
    for (int k = 0; k < n_lines; k++) {
        int w = x98_text_w(&x98, lines[k]);
        if (w > longest) longest = w;
    }
    win_w = PAD + ICON + 14 + longest + PAD;
    if (win_w < 300) win_w = 300;
    if (win_w > 720) win_w = 720;
    win_h = PAD + n_lines * (x98_text_h(&x98) + 4) + 20 + BTN_H + PAD;
    if (win_h < 130) win_h = 130;

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              win_w, win_h, 0, x98.shadow, x98.face);
    XStoreName(dpy, win, title);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = win_w;
    hints.min_height = hints.max_height = win_h;
    XSetWMNormalHints(dpy, win, &hints);

    XMapRaised(dpy, win);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) redraw();
        else if (ev.type == ButtonPress || ev.type == KeyPress) break;
        else if (ev.type == ClientMessage &&
                 (Atom)ev.xclient.data.l[0] == a_wm_delete) break;
    }

    XCloseDisplay(dpy);
    free(copy);
    return 0;
}

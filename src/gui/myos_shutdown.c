/* ==========================================================================
 * myos_shutdown.c  -  「myOS の終了」画面
 *
 *   myos-shutdown
 *
 * Windows 98 の「Windows の終了」と同じ形。
 * 選ばせてから実行する。押し間違いで電源が落ちるのが一番困る。
 *
 * 実際に切るのは myos-poweroff (要 root)。sudoers で NOPASSWD に
 * してあるので、ここではパスワードを聞かない。
 *
 * ビルド:
 *   gcc -O2 -o myos-shutdown myos_shutdown.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "x98.h"

#define WIN_W  340
#define WIN_H  190
#define BTN_W   80
#define BTN_H   24

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

static const struct { const char *label, *desc; } choices[] = {
    { "Shut down",  "Turn the computer off." },
    { "Restart",    "Start myOS again."      },
};
#define N_CHOICES ((int)(sizeof(choices) / sizeof(choices[0])))

static int sel = 0;

/* 電源のしるし。丸に縦棒。画像を持たずに済ませる。 */
static void draw_icon(int x, int y)
{
    unsigned long blue = x98_rgb24(&x98, 0x000080);
    XSetForeground(dpy, x98.gc, blue);
    XSetLineAttributes(dpy, x98.gc, 3, LineSolid, CapButt, JoinMiter);
    XDrawArc(dpy, win, x98.gc, x, y, 28, 28, 60 * 64, 240 * 64);
    XDrawLine(dpy, win, x98.gc, x + 14, y - 2, x + 14, y + 12);
    XSetLineAttributes(dpy, x98.gc, 1, LineSolid, CapButt, JoinMiter);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);

    draw_icon(20, 24);
    x98_text(&x98, win, 64, 30, "What do you want the computer to do?",
             x98.text);

    for (int i = 0; i < N_CHOICES; i++) {
        int ry = 70 + i * 34;
        x98_bevel(&x98, win, 64, ry, 13, 13, 0);
        x98_fill(&x98, win, 66, ry + 2, 9, 9, x98.white);
        if (i == sel) x98_fill(&x98, win, 68, ry + 4, 5, 5, x98.text);
        x98_text(&x98, win, 86, ry - 1, choices[i].label, x98.text);
        x98_text(&x98, win, 86, ry + 14, choices[i].desc, x98.shadow);
    }

    int by = WIN_H - BTN_H - 16;
    x98_button(&x98, win, WIN_W - 2 * BTN_W - 24, by, BTN_W, BTN_H,
               "OK", 0);
    x98_button(&x98, win, WIN_W - BTN_W - 16, by, BTN_W, BTN_H,
               "Cancel", 0);
    XFlush(dpy);
}

/* 選んだものを実行する。戻ってこない (成功すれば電源が切れる)。 */
static void go(void)
{
    /* 画面を消してから渡す。切れるまでの数秒、操作できる見た目のまま
     * 固まっていると壊れたように見える。 */
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);
    x98_text(&x98, win, 30, WIN_H / 2 - 8,
             sel == 1 ? "Restarting myOS..." : "myOS is shutting down...",
             x98.text);
    XFlush(dpy);
    XSync(dpy, False);

    execlp("sudo", "sudo", "-n", "/usr/local/bin/myos-poweroff",
           sel == 1 ? "-r" : "--", (char *)NULL);
    /* ここに来たら sudo が無いか許可されていない */
    perror("myos-shutdown: sudo");
    _exit(1);
}

int main(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-shutdown: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);

    int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                              (sw - WIN_W) / 2, (sh - WIN_H) / 2,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XStoreName(dpy, win, "Shut Down myOS");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    /* 大きさを固定する。伸ばされても中身は動かないので、
     * 動かせないことをはっきり伝えておく。 */
    XSizeHints *sz = XAllocSizeHints();
    if (sz) {
        sz->flags = PMinSize | PMaxSize;
        sz->min_width = sz->max_width = WIN_W;
        sz->min_height = sz->max_height = WIN_H;
        XSetWMNormalHints(dpy, win, sz);
        XFree(sz);
    }

    XMapRaised(dpy, win);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) redraw();
        else if (ev.type == ButtonPress) {
            int mx = ev.xbutton.x, my = ev.xbutton.y;
            for (int i = 0; i < N_CHOICES; i++) {
                int ry = 70 + i * 34;
                if (my >= ry - 4 && my < ry + 26 && mx >= 64 && mx < WIN_W - 20) {
                    sel = i;
                    redraw();
                }
            }
            int by = WIN_H - BTN_H - 16;
            if (my >= by && my < by + BTN_H) {
                if (mx >= WIN_W - 2 * BTN_W - 24 && mx < WIN_W - BTN_W - 24)
                    go();
                if (mx >= WIN_W - BTN_W - 16 && mx < WIN_W - 16)
                    break;
            }
        } else if (ev.type == KeyPress) {
            KeySym k = XLookupKeysym(&ev.xkey, 0);
            if (k == XK_Escape) break;
            if (k == XK_Return || k == XK_KP_Enter) go();
            if (k == XK_Up   && sel > 0) { sel--; redraw(); }
            if (k == XK_Down && sel < N_CHOICES - 1) { sel++; redraw(); }
        } else if (ev.type == ClientMessage &&
                   (Atom)ev.xclient.data.l[0] == a_wm_delete) {
            break;
        }
    }

    XCloseDisplay(dpy);
    return 0;
}

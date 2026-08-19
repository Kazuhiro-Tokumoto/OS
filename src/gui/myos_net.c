/* ==========================================================================
 * myos_net.c  -  無線 LAN に繋ぐ画面 (Windows 98 のネットワーク)
 *
 * 一覧から選んで、鍵が要るものは合言葉を入れて繋ぐ。それだけ。
 *
 * root が要る操作 (scan / wpa_supplicant / dhclient) は全部
 * /usr/local/bin/myos-wifi に寄せてあり、こちらは sudo -n でそれを呼ぶ。
 * デスクトップは一般ユーザーで動くので、この形にしないと何も出来ない。
 *
 * SSID は近所の AP が名乗ってくる文字列で、こちらで中身を選べない。
 * シェルを経由すると引用符や $ を仕込まれる余地ができるので、
 * popen は使わず fork + execvp で直接渡す。
 * 合言葉も引数ではなく標準入力で渡す (引数だと ps で丸見えになる)。
 *
 * ビルド:
 *   gcc -O2 -o myos-net myos_net.c -lX11 -lXft
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "x98.h"

static XIC ic;

#define WIN_W   460
#define WIN_H   360
#define PAD     12
#define LIST_X  PAD
#define LIST_Y  40
#define LIST_W  (WIN_W - PAD * 2)
#define LIST_H  200
#define ROW_H   20
#define BTN_W   84
#define BTN_H   24

#define MAX_AP  64

typedef struct {
    char ssid[64];
    int  signal;            /* dBm */
    char sec[8];            /* "open" / "WPA" / "WPA2" */
} Ap;

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;

static Ap   aps[MAX_AP];
static int  n_aps = 0;
static int  sel = 0;
static int  top = 0;

static char iface[32] = "";
static char status[160] = "";
static char cur_ssid[64] = "";
static char cur_ip[64] = "";

static int      asking = 0;         /* 合言葉を聞いている最中か */
static X98Edit  pw;

/* --- 外部コマンド -------------------------------------------------------- */
/* シェルを通さずに実行し、標準出力を out に取る。戻り値は終了ステータス。 */
static int run_capture(char *const argv[], char *out, size_t n)
{
    int fd[2];
    if (out && n) out[0] = 0;
    if (pipe(fd) != 0) return -1;

    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (p == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        close(fd[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 2); close(nul); }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);

    size_t got = 0;
    if (out && n) {
        ssize_t r;
        while (got + 1 < n && (r = read(fd[0], out + got, n - got - 1)) > 0)
            got += (size_t)r;
        out[got] = 0;
    }
    /* 読み切らないと相手が書き込みで詰まる */
    char sink[256];
    while (read(fd[0], sink, sizeof(sink)) > 0) { }
    close(fd[0]);

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* 標準入力に 1 行渡して実行する。合言葉を引数に載せないため。 */
static int run_feed(char *const argv[], const char *line)
{
    int fd[2];
    if (pipe(fd) != 0) return -1;

    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (p == 0) {
        close(fd[1]);
        dup2(fd[0], 0);
        close(fd[0]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); close(nul); }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[0]);
    dprintf(fd[1], "%s\n", line ? line : "");
    close(fd[1]);

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* --- 無線の操作 ---------------------------------------------------------- */
static void find_iface(void)
{
    char out[256];
    char *av[] = { "sudo", "-n", "/usr/local/bin/myos-wifi", "list", NULL };
    if (run_capture(av, out, sizeof(out)) != 0) { iface[0] = 0; return; }
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    snprintf(iface, sizeof(iface), "%s", out);
}

static void load_status(void)
{
    cur_ssid[0] = cur_ip[0] = 0;
    if (!iface[0]) return;
    char out[256];
    char *av[] = { "sudo", "-n", "/usr/local/bin/myos-wifi", "status",
                   iface, NULL };
    if (run_capture(av, out, sizeof(out)) != 0) return;
    char *p = strstr(out, "ssid=");
    if (p) sscanf(p + 5, "%63[^\n]", cur_ssid);
    p = strstr(out, "ip=");
    if (p) sscanf(p + 3, "%63[^\n]", cur_ip);
}

static void do_scan(void)
{
    n_aps = 0; sel = 0; top = 0;
    if (!iface[0]) {
        snprintf(status, sizeof(status), "No wireless adapter was found.");
        return;
    }
    snprintf(status, sizeof(status), "Looking for networks...");

    char *out = malloc(65536);
    if (!out) return;
    char *av[] = { "sudo", "-n", "/usr/local/bin/myos-wifi", "scan",
                   iface, NULL };
    int rc = run_capture(av, out, 65536);

    char *line = out, *nl;
    while (n_aps < MAX_AP && line && *line) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *t1 = strchr(line, '\t');
        if (t1) {
            *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t');
            if (t2) {
                *t2 = 0;
                snprintf(aps[n_aps].ssid, sizeof(aps[n_aps].ssid), "%s", line);
                aps[n_aps].signal = atoi(t1 + 1);
                snprintf(aps[n_aps].sec, sizeof(aps[n_aps].sec), "%s", t2 + 1);
                if (aps[n_aps].ssid[0]) n_aps++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(out);

    if (n_aps == 0)
        snprintf(status, sizeof(status), rc == 0
                 ? "No networks were found. Try again."
                 : "Could not search. Is the adapter switched on?");
    else
        snprintf(status, sizeof(status), "%d network%s found on %s.",
                 n_aps, n_aps == 1 ? "" : "s", iface);
}

static void do_connect(const char *psk)
{
    if (sel < 0 || sel >= n_aps) return;
    snprintf(status, sizeof(status), "Connecting to %s...", aps[sel].ssid);

    char *av[] = { "sudo", "-n", "/usr/local/bin/myos-wifi", "connect",
                   iface, aps[sel].ssid, NULL };
    int rc = run_feed(av, psk);

    load_status();
    if (rc == 0 && cur_ssid[0])
        snprintf(status, sizeof(status), "Connected to %s.%s%s",
                 cur_ssid, cur_ip[0] ? "  Address: " : "", cur_ip);
    else
        snprintf(status, sizeof(status),
                 "Could not connect to %s. Check the password.",
                 aps[sel].ssid);
}

/* --- 描画 ---------------------------------------------------------------- */
/* 電波の強さを 4 本の棒で。dBm の生値を出しても分かりにくい。 */
static void draw_bars(int x, int y, int dbm)
{
    int lv = dbm > -50 ? 4 : dbm > -60 ? 3 : dbm > -70 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 3;
        unsigned long c = (i < lv) ? x98_rgb24(&x98, 0x000080) : x98.shadow;
        x98_fill(&x98, win, x + i * 4, y + 12 - h, 3, h, c);
    }
}

static void draw_list(void)
{
    x98_bevel(&x98, win, LIST_X, LIST_Y, LIST_W, LIST_H, 0);
    x98_fill(&x98, win, LIST_X + 2, LIST_Y + 2, LIST_W - 4, LIST_H - 4,
             x98.white);

    int vis = (LIST_H - 4) / ROW_H;
    for (int i = 0; i < vis && top + i < n_aps; i++) {
        Ap *a = &aps[top + i];
        int y = LIST_Y + 2 + i * ROW_H;
        int on = (top + i == sel);
        if (on)
            x98_fill(&x98, win, LIST_X + 2, y, LIST_W - 4, ROW_H,
                     x98.select_bg);
        unsigned long fg = on ? x98.white : x98.text;

        draw_bars(LIST_X + 8, y + 4, a->signal);
        x98_text(&x98, win, LIST_X + 32, y + (ROW_H - x98_text_h(&x98)) / 2,
                 a->ssid, fg);
        /* 鍵の有無は右端に。開いている網は黙って繋がるので目立たせる。 */
        x98_text(&x98, win, LIST_X + LIST_W - 60,
                 y + (ROW_H - x98_text_h(&x98)) / 2,
                 strcmp(a->sec, "open") ? a->sec : "open", fg);
        if (cur_ssid[0] && !strcmp(cur_ssid, a->ssid))
            x98_text(&x98, win, LIST_X + LIST_W - 100,
                     y + (ROW_H - x98_text_h(&x98)) / 2, "*", fg);
    }
}

static void draw_ask(void)
{
    int w = 320, h = 110;
    int x = (WIN_W - w) / 2, y = (WIN_H - h) / 2;
    x98_fill(&x98, win, x, y, w, h, x98.face);
    x98_bevel(&x98, win, x, y, w, h, 1);
    x98_titlebar(&x98, win, x + 3, y + 3, w - 6, 18, "Password");

    char msg[128];
    snprintf(msg, sizeof(msg), "Password for %s:", aps[sel].ssid);
    x98_text(&x98, win, x + 12, y + 32, msg, x98.text);

    x98_bevel(&x98, win, x + 12, y + 52, w - 24, 20, 0);
    x98_fill(&x98, win, x + 14, y + 54, w - 28, 16, x98.white);
    /* 合言葉は伏せる。肩越しに見られるのを防ぐだけだが、無いと落ち着かない */
    char dots[X98_EDIT_MAX];
    int nch = 0;
    for (int i = 0; i < pw.len; i++)
        if (((unsigned char)pw.buf[i] & 0xC0) != 0x80) nch++;
    if (nch > (int)sizeof(dots) - 1) nch = (int)sizeof(dots) - 1;
    memset(dots, '*', (size_t)nch);
    dots[nch] = 0;
    x98_text(&x98, win, x + 18, y + 54 + (16 - x98_text_h(&x98)) / 2,
             dots, x98.text);

    x98_text(&x98, win, x + 12, y + 82, "Enter = OK    Esc = Cancel",
             x98.shadow);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);
    x98_text(&x98, win, PAD, 14, "Wireless networks", x98.text);

    draw_list();

    int sy = LIST_Y + LIST_H + 10;
    x98_text(&x98, win, PAD, sy, status, x98.text);
    if (cur_ssid[0]) {
        char b[160];
        snprintf(b, sizeof(b), "Now on: %s%s%s", cur_ssid,
                 cur_ip[0] ? "   " : "", cur_ip);
        x98_text(&x98, win, PAD, sy + 18, b, x98.shadow);
    }

    int by = WIN_H - BTN_H - PAD;
    x98_button(&x98, win, WIN_W - 3 * BTN_W - 26, by, BTN_W, BTN_H,
               "Scan", 0);
    x98_button(&x98, win, WIN_W - 2 * BTN_W - 19, by, BTN_W, BTN_H,
               "Connect", 0);
    x98_button(&x98, win, WIN_W - BTN_W - 12, by, BTN_W, BTN_H, "Close", 0);
    /* 操作の案内はボタンの上。同じ行に置くと Scan に重なる。 */
    x98_text(&x98, win, PAD, by - 20,
             "Up/Down = choose   Enter = connect   S = scan", x98.shadow);

    if (asking) draw_ask();
}

/* --- 繋ぐ流れ ------------------------------------------------------------ */
static void start_connect(void)
{
    if (sel < 0 || sel >= n_aps) return;
    if (strcmp(aps[sel].sec, "open") == 0) {
        do_connect("");
        return;
    }
    /* 鍵が要るものは先に合言葉を聞く */
    asking = 1;
    x98_edit_set(&pw, "");
}

int main(void)
{
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-net: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XStoreName(dpy, win, "Network");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &del, 1);
    XMapWindow(dpy, win);

    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    find_iface();
    load_status();
    if (!iface[0])
        snprintf(status, sizeof(status), "No wireless adapter was found.");
    else
        snprintf(status, sizeof(status), "Press Scan to look for networks.");

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            redraw();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == del) goto done;
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);

            if (asking) {
                if (ks == XK_Return || ks == XK_KP_Enter) {
                    asking = 0;
                    redraw(); XFlush(dpy);
                    do_connect(pw.buf);
                    memset(pw.buf, 0, sizeof(pw.buf));
                    pw.len = pw.cur = 0;
                } else if (ks == XK_Escape) {
                    asking = 0;
                    memset(pw.buf, 0, sizeof(pw.buf));
                    pw.len = pw.cur = 0;
                } else if (ks == XK_BackSpace) {
                    x98_edit_backspace(&pw);
                } else if (n > 0 && (unsigned char)buf[0] >= 0x20) {
                    x98_edit_insert(&pw, buf, n);
                }
                redraw();
                break;
            }

            if (ks == XK_Escape) goto done;
            else if (ks == XK_Up && sel > 0) sel--;
            else if (ks == XK_Down && sel < n_aps - 1) sel++;
            else if (ks == XK_Return || ks == XK_KP_Enter) {
                start_connect();
            } else if (ks == XK_F5 || (n > 0 && (buf[0] == 's' || buf[0] == 'S'))) {
                redraw(); XFlush(dpy);
                do_scan();
                load_status();
            }
            /* 選んだものが見えるように寄せる */
            {
                int vis = (LIST_H - 4) / ROW_H;
                if (sel < top) top = sel;
                if (sel >= top + vis) top = sel - vis + 1;
                if (top < 0) top = 0;
            }
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;
            if (asking) break;

            int by = WIN_H - BTN_H - PAD;
            if (my >= by && my < by + BTN_H) {
                if (mx >= WIN_W - 3 * BTN_W - 26 && mx < WIN_W - 2 * BTN_W - 26) {
                    redraw(); XFlush(dpy);
                    do_scan();
                    load_status();
                } else if (mx >= WIN_W - 2 * BTN_W - 19 &&
                           mx < WIN_W - BTN_W - 19) {
                    start_connect();
                } else if (mx >= WIN_W - BTN_W - 12) {
                    goto done;
                }
            } else if (mx >= LIST_X && mx < LIST_X + LIST_W &&
                       my >= LIST_Y + 2 && my < LIST_Y + LIST_H - 2) {
                int i = top + (my - LIST_Y - 2) / ROW_H;
                if (i >= 0 && i < n_aps) sel = i;
            }
            redraw();
            break;
        }
        }
    }

done:
    memset(pw.buf, 0, sizeof(pw.buf));
    XCloseDisplay(dpy);
    return 0;
}

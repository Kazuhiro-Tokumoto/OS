/* ==========================================================================
 * myos_image.c  -  画像ビューア
 *
 * 画像のデコードは自前で書かない。PNG / JPEG / GIF / BMP と形式ごとに
 * デコーダを抱えるのは本題ではないので、ImageMagick の convert に
 * PPM (P6) へ変換させて、その生データだけを受け取る。
 * 1 本の経路で全形式を扱えるので、見通しがよい。
 *
 * 操作:
 *   +/-      拡大 / 縮小
 *   0        等倍
 *   F        ウィンドウに合わせる (既定)
 *   矢印     スクロール
 *
 * ビルド:
 *   gcc -O2 -o myos-image myos_image.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>

#include "x98.h"

#define WIN_W 800
#define WIN_H 600
#define BAR_H 24

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

static unsigned char *img;          /* RGB 3 バイト/画素 */
static int img_w, img_h;
static int win_w = WIN_W, win_h = WIN_H;
static int fit = 1;                 /* ウィンドウに合わせるか */
static double zoom = 1.0;
static int off_x = 0, off_y = 0;
static char status[256] = "";
static char fname[PATH_MAX] = "";

/* --- convert に流し込んで PPM で受け取る -------------------------------- */
static unsigned char *load_via_convert(const char *p, int *w, int *h)
{
    int fd[2];
    if (pipe(fd) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return NULL; }
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
        int devnull = open("/dev/null", 1 /* O_WRONLY */);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        /* シェルを通さないので、ファイル名に空白や引用符があっても平気 */
        /* -depth 8 を必ず付けること。付けないと ImageMagick が
         * 16bit/チャンネル (maxval 65535) の PPM を吐くことがあり、
         * 1 画素 3 バイト前提で読むと色も位置もずれる。 */
        execlp("convert", "convert", p, "-depth", "8", "ppm:-", (char *)NULL);
        _exit(127);
    }
    close(fd[1]);

    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { close(fd[0]); return NULL; }
    for (;;) {
        if (len + 65536 > cap) {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd[0]); return NULL; }
            buf = nb;
        }
        ssize_t r = read(fd[0], buf + len, 65536);
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(fd[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0) { }

    /* P6 のヘッダを読む (コメント行は飛ばす) */
    if (len < 10 || buf[0] != 'P' || buf[1] != '6') { free(buf); return NULL; }
    size_t pos = 2;
    int fields[3], nf = 0;
    while (nf < 3 && pos < len) {
        while (pos < len && isspace(buf[pos])) pos++;
        if (pos < len && buf[pos] == '#') {
            while (pos < len && buf[pos] != '\n') pos++;
            continue;
        }
        int v = 0;
        while (pos < len && buf[pos] >= '0' && buf[pos] <= '9')
            v = v * 10 + (buf[pos++] - '0');
        fields[nf++] = v;
    }
    pos++;   /* ヘッダ末尾の空白 1 個 */
    if (nf < 3) { free(buf); return NULL; }

    *w = fields[0];
    *h = fields[1];
    int maxval = fields[2];
    if (maxval > 255) { free(buf); return NULL; }   /* 8bit 以外は受けない */
    size_t need = (size_t)*w * *h * 3;
    if (pos + need > len) { free(buf); return NULL; }

    unsigned char *px = malloc(need);
    if (!px) { free(buf); return NULL; }
    memcpy(px, buf + pos, need);
    free(buf);
    return px;
}

/* --- 描画 ---------------------------------------------------------------- */
static double fit_scale(void)
{
    if (!img_w || !img_h) return 1.0;
    double sx = (double)win_w / img_w;
    double sy = (double)(win_h - BAR_H) / img_h;
    double s = sx < sy ? sx : sy;
    return s > 1.0 ? 1.0 : s;     /* 小さい画像は拡大しない */
}

static void draw_bar(void)
{
    x98_fill(&x98, win, 0, win_h - BAR_H, win_w, BAR_H, x98.face);
    x98_hline(&x98, win, 0, win_h - BAR_H, win_w, x98.light);
    x98_text(&x98, win, 6, win_h - BAR_H + (BAR_H - x98_text_h(&x98)) / 2,
             status, x98.text);
}

static void draw_image(void)
{
    int area_h = win_h - BAR_H;
    x98_fill(&x98, win, 0, 0, win_w, area_h, x98_rgb24(&x98, 0x404040));

    if (!img) {
        x98_text(&x98, win, 12, 12,
                 "Cannot open this image. Is ImageMagick installed?",
                 x98.white);
        return;
    }

    double s = fit ? fit_scale() : zoom;
    int dw = (int)(img_w * s), dh = (int)(img_h * s);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    int dx = (dw < win_w) ? (win_w - dw) / 2 : -off_x;
    int dy = (dh < area_h) ? (area_h - dh) / 2 : -off_y;

    /* 出す範囲だけ組み立てる。全体を作ると大きい画像で無駄が多い。 */
    int vx0 = dx < 0 ? -dx : 0, vy0 = dy < 0 ? -dy : 0;
    int vw = dw - vx0, vh = dh - vy0;
    if (vw > win_w) vw = win_w;
    if (vh > area_h) vh = area_h;
    if (vw <= 0 || vh <= 0) return;

    unsigned int *pix = malloc((size_t)vw * vh * 4);
    if (!pix) return;

    for (int y = 0; y < vh; y++) {
        int sy = (int)((y + vy0) / s);
        if (sy >= img_h) sy = img_h - 1;
        for (int px = 0; px < vw; px++) {
            int sx = (int)((px + vx0) / s);
            if (sx >= img_w) sx = img_w - 1;
            const unsigned char *p = img + ((size_t)sy * img_w + sx) * 3;
            pix[(size_t)y * vw + px] = (unsigned int)
                x98_rgb(&x98, p[0], p[1], p[2]);
        }
    }

    XImage *xi = XCreateImage(dpy, DefaultVisual(dpy, screen),
                              DefaultDepth(dpy, screen), ZPixmap, 0,
                              (char *)pix, vw, vh, 32, 0);
    if (xi) {
        XPutImage(dpy, win, x98.gc, xi, 0, 0,
                  dx < 0 ? 0 : dx, dy < 0 ? 0 : dy, vw, vh);
        xi->data = NULL;      /* pix は自分で解放する */
        XDestroyImage(xi);
    }
    free(pix);
}

static void update_status(void)
{
    double s = fit ? fit_scale() : zoom;
    snprintf(status, sizeof(status), "%s   %dx%d   %d%%%s",
             fname, img_w, img_h, (int)(s * 100 + 0.5),
             fit ? "  (fit)" : "");
}

static void redraw(void)
{
    draw_image();
    draw_bar();
}

int main(int argc, char **argv)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-image] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);

    if (argc > 1) {
        img = load_via_convert(argv[1], &img_w, &img_h);
        const char *b = strrchr(argv[1], '/');
        snprintf(fname, sizeof(fname), "%s", b ? b + 1 : argv[1]);
    }
    update_status();

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
                           StructureNotifyMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    char title[PATH_MAX + 32];
    snprintf(title, sizeof(title), "%s - Image Viewer",
             fname[0] ? fname : "Image Viewer");
    XStoreName(dpy, win, title);
    XMapWindow(dpy, win);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (!ev.xexpose.count) redraw();
            break;

        case ConfigureNotify:
            if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                update_status();
            }
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a_wm_delete) {
                XCloseDisplay(dpy);
                return 0;
            }
            break;

        case ButtonPress:
            if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                fit = 0;
                if (zoom <= 0.01) zoom = fit_scale();
                zoom *= (ev.xbutton.button == 4) ? 1.25 : 0.8;
                if (zoom < 0.05) zoom = 0.05;
                if (zoom > 20.0) zoom = 20.0;
                update_status();
                redraw();
            }
            break;

        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int step = 60;
            if (ks == XK_plus || ks == XK_equal || ks == XK_KP_Add) {
                if (fit) { zoom = fit_scale(); fit = 0; }
                zoom *= 1.25;
            } else if (ks == XK_minus || ks == XK_KP_Subtract) {
                if (fit) { zoom = fit_scale(); fit = 0; }
                zoom *= 0.8;
            } else if (ks == XK_0) {
                fit = 0; zoom = 1.0;
            } else if (ks == XK_f || ks == XK_F) {
                fit = 1; off_x = off_y = 0;
            } else if (ks == XK_Left)  { off_x -= step; }
            else if (ks == XK_Right)   { off_x += step; }
            else if (ks == XK_Up)      { off_y -= step; }
            else if (ks == XK_Down)    { off_y += step; }
            else if (ks == XK_q || ks == XK_Escape) {
                XCloseDisplay(dpy);
                return 0;
            }
            if (off_x < 0) off_x = 0;
            if (off_y < 0) off_y = 0;
            update_status();
            redraw();
            break;
        }

        default:
            break;
        }
    }
    return 0;
}

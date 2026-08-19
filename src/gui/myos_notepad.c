/* ==========================================================================
 * myos_notepad.c  -  Windows 98 のメモ帳もどき
 *
 * X には文字入力のウィジェットが無いので、テキスト編集は全部自前で書く。
 * 等幅フォント前提で、1 行 = 1 本の文字列として持つ。
 *
 * 使える操作 (Windows でおなじみのもの):
 *   文字入力 / Enter / BackSpace / Delete
 *   矢印 / Home / End / PageUp / PageDown
 *   Ctrl+S  保存        Ctrl+N  新規
 *   Ctrl+A  全選択の代わりに先頭へ (選択は未実装)
 *
 * 日本語入力は扱わない (IME を抱えるのは別の話)。表示はできる。
 *
 * ビルド:
 *   gcc -O2 -o myos-notepad myos_notepad.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "x98.h"

/* 日本語入力の入力文脈。IME が上がっていなければ NULL のまま。 */
static XIC ic;

#define WIN_W      680
#define WIN_H      480
#define MENU_H     22
#define PAD        4
#define MAX_LINES  20000
#define LINE_CHUNK 128

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

static char  *lines[MAX_LINES];
static int    n_lines = 1;
static int    cur_l = 0, cur_c = 0;   /* カーソル位置 (行, 桁) */
static int    top_l = 0, left_c = 0;  /* 表示の左上 */
static int    modified = 0;
static char   path[PATH_MAX] = "";
static int    win_w = WIN_W, win_h = WIN_H;
static int    menu_open = 0;          /* 1 = File メニューが開いている */

/* --- 行の管理 ------------------------------------------------------------ */
static char *line_new(const char *src)
{
    size_t n = src ? strlen(src) : 0;
    size_t cap = ((n + 1) / LINE_CHUNK + 1) * LINE_CHUNK;
    char *p = malloc(cap);
    if (!p) return NULL;
    if (src) memcpy(p, src, n);
    p[n] = 0;
    return p;
}

static void doc_clear(void)
{
    for (int i = 0; i < n_lines; i++) { free(lines[i]); lines[i] = NULL; }
    n_lines = 1;
    lines[0] = line_new("");
    cur_l = cur_c = top_l = left_c = 0;
    modified = 0;
}

static void set_title(void)
{
    char t[PATH_MAX + 64];
    const char *name = path[0] ? path : "Untitled";
    snprintf(t, sizeof(t), "%s%s - Notepad", modified ? "*" : "", name);
    XStoreName(dpy, win, t);
}

static void doc_load(const char *p)
{
    doc_clear();
    FILE *f = fopen(p, "r");
    if (!f) { snprintf(path, sizeof(path), "%s", p); set_title(); return; }

    free(lines[0]);
    n_lines = 0;

    char buf[8192];
    while (fgets(buf, sizeof(buf), f) && n_lines < MAX_LINES) {
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
        lines[n_lines++] = line_new(buf);
    }
    fclose(f);
    if (!n_lines) lines[n_lines++] = line_new("");

    snprintf(path, sizeof(path), "%s", p);
    modified = 0;
    set_title();
}

static void doc_save(void)
{
    if (!path[0]) snprintf(path, sizeof(path), "/root/untitled.txt");
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n_lines; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
    modified = 0;
    set_title();
}

/* 行に文字を挿す。必要なら伸ばす。 */
static void line_insert(int l, int c, const char *s, int n)
{
    size_t len = strlen(lines[l]);
    char *p = realloc(lines[l], len + n + 1);
    if (!p) return;
    lines[l] = p;
    memmove(p + c + n, p + c, len - c + 1);
    memcpy(p + c, s, n);
}

static void split_line(void)
{
    if (n_lines >= MAX_LINES) return;
    char *rest = line_new(lines[cur_l] + cur_c);
    lines[cur_l][cur_c] = 0;
    for (int i = n_lines; i > cur_l + 1; i--) lines[i] = lines[i - 1];
    lines[cur_l + 1] = rest;
    n_lines++;
    cur_l++;
    cur_c = 0;
}

static void join_prev(void)
{
    if (cur_l == 0) return;
    int plen = (int)strlen(lines[cur_l - 1]);
    line_insert(cur_l - 1, plen, lines[cur_l], (int)strlen(lines[cur_l]));
    free(lines[cur_l]);
    for (int i = cur_l; i < n_lines - 1; i++) lines[i] = lines[i + 1];
    n_lines--;
    cur_l--;
    cur_c = plen;
}

/* --- 描画 ----------------------------------------------------------------
 * 本文はコアフォント + XDrawString で描いていた。あれは 1 バイトを 1 字と
 * 見なすので、日本語を打つと必ず化ける (1.1.2 で他の画面は Xft に直したが、
 * ここだけ桁の計算が楽という理由で残っていた)。
 * 日本語が入るようになった以上、ここも Xft にする必要がある。
 *
 * そのぶん「1 文字 = 何ピクセル」が使えなくなるので、横位置は全て
 * 実測に変えた。左端 (left_c) は桁ではなくバイト位置で持つ。 */
static int line_h(void)  { return x98_text_h(&x98) + 2; }

static int text_x(void) { return PAD + 2; }
static int text_y(void) { return MENU_H + PAD + 2; }
static int rows_vis(void) { return (win_h - text_y() - PAD - 2) / line_h(); }
static int text_w(void) { return win_w - text_x() - PAD - 2; }

/* s の [from, to) の幅。x98_text_w は 0 終端しか測れないので写して測る。 */
static int seg_w(const char *s, int from, int to)
{
    char tmp[1024];
    int n = to - from;
    if (n <= 0) return 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    memcpy(tmp, s + from, (size_t)n);
    tmp[n] = 0;
    return x98_text_w(&x98, tmp);
}

/* 幅 w に入るところまでを out に写す。文字の途中では切らない。 */
static void fit_seg(const char *s, int from, int w, char *out, int outsz)
{
    int len = (int)strlen(s);
    int i = from, last = from;
    out[0] = 0;
    while (i < len) {
        int nx = x98_u8_next(s, i, len);
        if (nx - from >= outsz - 1) break;
        memcpy(out, s + from, (size_t)(nx - from));
        out[nx - from] = 0;
        if (x98_text_w(&x98, out) > w) { out[last - from] = 0; return; }
        last = nx;
        i = nx;
    }
}

static void draw_menu(void)
{
    x98_fill(&x98, win, 0, 0, win_w, MENU_H, x98.face);
    x98_text(&x98, win, 8, (MENU_H - x98_text_h(&x98)) / 2, "File", x98.text);
    x98_text(&x98, win, 56, (MENU_H - x98_text_h(&x98)) / 2, "Help", x98.text);
    x98_hline(&x98, win, 0, MENU_H - 2, win_w, x98.shadow);
    x98_hline(&x98, win, 0, MENU_H - 1, win_w, x98.light);

    if (menu_open) {
        static const char *items[] = { "New", "Save", "Exit" };
        int mw = 120, ih = 20, mh = 3 * ih + 6;
        x98_fill(&x98, win, 4, MENU_H, mw, mh, x98.face);
        x98_bevel(&x98, win, 4, MENU_H, mw, mh, 1);
        for (int i = 0; i < 3; i++)
            x98_text(&x98, win, 14, MENU_H + 4 + i * ih +
                     (ih - x98_text_h(&x98)) / 2, items[i], x98.text);
    }
}

static void draw_text(void)
{
    int tx = text_x(), ty = text_y();
    int tw = win_w - tx - PAD, th = win_h - ty - PAD;

    x98_bevel(&x98, win, tx - 2, ty - 2, tw + 4, th + 4, 0);
    x98_fill(&x98, win, tx, ty, tw, th, x98.white);

    int rows = rows_vis(), lh = line_h();
    char buf[1024];
    for (int r = 0; r < rows; r++) {
        int l = top_l + r;
        if (l >= n_lines) break;
        const char *s = lines[l];
        if (left_c >= (int)strlen(s)) continue;
        fit_seg(s, left_c, tw, buf, (int)sizeof(buf));
        if (buf[0]) x98_text(&x98, win, tx, ty + r * lh, buf, x98.text);
    }

    /* カーソル */
    if (cur_l >= top_l && cur_l < top_l + rows) {
        int cx = tx + seg_w(lines[cur_l], left_c, cur_c);
        int cy = ty + (cur_l - top_l) * lh;
        if (cx >= tx && cx < tx + tw)
            x98_vline(&x98, win, cx, cy, lh, x98.text);
    }
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, win_w, win_h, x98.face);
    draw_text();
    draw_menu();
}

/* カーソルが見えるように表示範囲を寄せる */
static void scroll_to_cursor(void)
{
    int rows = rows_vis();
    if (cur_l < top_l) top_l = cur_l;
    if (cur_l >= top_l + rows) top_l = cur_l - rows + 1;
    if (top_l < 0) top_l = 0;

    /* 横は桁で数えられない (全角と半角で幅が違う)。
     * カーソルが右端からはみ出している間、左端を 1 文字ずつ送る。 */
    if (cur_c < left_c) left_c = cur_c;
    if (left_c < 0) left_c = 0;
    int ln = (int)strlen(lines[cur_l]);
    while (left_c < cur_c && seg_w(lines[cur_l], left_c, cur_c) > text_w())
        left_c = x98_u8_next(lines[cur_l], left_c, ln);
}

static int clampc(void)
{
    int len = (int)strlen(lines[cur_l]);
    if (cur_c > len) cur_c = len;
    /* 上下に動くと、行の長さが違うぶん文字の途中に着地することがある。
     * 継続バイトの上に居たら手前の境目まで下げる。 */
    while (cur_c > 0 && ((unsigned char)lines[cur_l][cur_c] & 0xC0) == 0x80)
        cur_c--;
    return len;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    /* 日本語入力より前にロケールを立てる。X を開いたあとだと
     * Xlib が古いロケールのまま動いてしまう。 */
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-notepad] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);


    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask |
                           StructureNotifyMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XMapWindow(dpy, win);

    /* 窓が出てから入力文脈を作る。窓より先に作ると
     * XNClientWindow に渡すものが無い。 */
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    lines[0] = line_new("");
    if (argc > 1) doc_load(argv[1]);
    else set_title();

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        /* IME が使う鍵はここで吸われる。忘れると
         * かなも漢字も一生入ってこない。 */
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            if (!ev.xexpose.count) redraw();
            break;

        case ConfigureNotify:
            if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
            }
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a_wm_delete) {
                XCloseDisplay(dpy);
                return 0;
            }
            break;

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            if (menu_open) {
                /* メニューの項目 */
                if (mx >= 4 && mx < 124 && my >= MENU_H && my < MENU_H + 66) {
                    int i = (my - MENU_H - 4) / 20;
                    menu_open = 0;
                    if (i == 0) { doc_clear(); path[0] = 0; set_title(); }
                    else if (i == 1) doc_save();
                    else if (i == 2) { XCloseDisplay(dpy); return 0; }
                    redraw();
                    break;
                }
                menu_open = 0;
                redraw();
                break;
            }

            if (my < MENU_H) {
                if (mx < 48) { menu_open = 1; redraw(); }
                break;
            }

            /* 本文をクリックしたらそこへカーソルを移す */
            if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                top_l += (ev.xbutton.button == 5) ? 3 : -3;
                if (top_l > n_lines - 1) top_l = n_lines - 1;
                if (top_l < 0) top_l = 0;
                redraw();
                break;
            }
            {
                int r = (my - text_y()) / line_h();
                cur_l = top_l + r;
                if (cur_l < 0) cur_l = 0;
                if (cur_l >= n_lines) cur_l = n_lines - 1;

                /* 桁で割り出せないので、左端から 1 文字ずつ足して
                 * 押された位置を跨ぐところを探す。行の長さは高が知れて
                 * いるので、これで十分速い。 */
                {
                    const char *s = lines[cur_l];
                    int ln = (int)strlen(s), want = mx - text_x();
                    int i = left_c;
                    cur_c = left_c;
                    while (i < ln) {
                        int nx = x98_u8_next(s, i, ln);
                        int wa = seg_w(s, left_c, i);
                        int wb = seg_w(s, left_c, nx);
                        if (want < (wa + wb) / 2) break;
                        cur_c = nx;
                        i = nx;
                    }
                }
                clampc();
                scroll_to_cursor();
                redraw();
            }
            break;
        }

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);
            int ctrl = (ev.xkey.state & ControlMask) != 0;

            if (ctrl) {
                if (ks == XK_s || ks == XK_S) { doc_save(); redraw(); break; }
                if (ks == XK_n || ks == XK_N) {
                    doc_clear(); path[0] = 0; set_title(); redraw(); break;
                }
                if (ks == XK_q || ks == XK_Q) { XCloseDisplay(dpy); return 0; }
                if (ks == XK_Home) { cur_l = 0; cur_c = 0; }
                if (ks == XK_End)  { cur_l = n_lines - 1; cur_c = (int)strlen(lines[cur_l]); }
                scroll_to_cursor();
                redraw();
                break;
            }

            switch (ks) {
            case XK_Return: case XK_KP_Enter:
                split_line(); modified = 1; break;
            /* 消す / 動かすのは 1 バイトではなく 1 文字ぶん。
             * 日本語は 3 バイトなので、バイト単位だと文字の途中で切れて
             * 壊れた列が残る (Xft では豆腐になる)。 */
            case XK_BackSpace:
                if (cur_c > 0) {
                    int p = x98_u8_prev(lines[cur_l], cur_c);
                    memmove(lines[cur_l] + p, lines[cur_l] + cur_c,
                            strlen(lines[cur_l]) - cur_c + 1);
                    cur_c = p;
                } else join_prev();
                modified = 1;
                break;
            case XK_Delete: {
                int len = (int)strlen(lines[cur_l]);
                if (cur_c < len) {
                    int q = x98_u8_next(lines[cur_l], cur_c, len);
                    memmove(lines[cur_l] + cur_c, lines[cur_l] + q,
                            len - q + 1);
                } else if (cur_l < n_lines - 1) {
                    cur_l++; cur_c = 0; join_prev();
                }
                modified = 1;
                break;
            }
            case XK_Left:
                if (cur_c > 0) cur_c = x98_u8_prev(lines[cur_l], cur_c);
                else if (cur_l > 0) { cur_l--; cur_c = (int)strlen(lines[cur_l]); }
                break;
            case XK_Right: {
                int ln = (int)strlen(lines[cur_l]);
                if (cur_c < ln) cur_c = x98_u8_next(lines[cur_l], cur_c, ln);
                else if (cur_l < n_lines - 1) { cur_l++; cur_c = 0; }
                break;
            }
            case XK_Up:    if (cur_l > 0) { cur_l--; clampc(); } break;
            case XK_Down:  if (cur_l < n_lines - 1) { cur_l++; clampc(); } break;
            case XK_Home:  cur_c = 0; break;
            case XK_End:   cur_c = (int)strlen(lines[cur_l]); break;
            case XK_Prior: cur_l -= rows_vis(); if (cur_l < 0) cur_l = 0; clampc(); break;
            case XK_Next:  cur_l += rows_vis();
                           if (cur_l >= n_lines) cur_l = n_lines - 1;
                           clampc(); break;
            case XK_Tab:
                line_insert(cur_l, cur_c, "    ", 4); cur_c += 4; modified = 1;
                break;
            default:
                if (n > 0 && (unsigned char)buf[0] >= 0x20) {
                    line_insert(cur_l, cur_c, buf, n);
                    cur_c += n;
                    modified = 1;
                }
                break;
            }
            set_title();
            scroll_to_cursor();
            redraw();
            break;
        }

        default:
            break;
        }
    }
    return 0;
}

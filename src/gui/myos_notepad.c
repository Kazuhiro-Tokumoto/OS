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
static XFontStruct *mono;
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

/* --- 描画 ---------------------------------------------------------------- */
static int char_w(void)  { return mono ? mono->max_bounds.width : 8; }
static int line_h(void)  { return mono ? mono->ascent + mono->descent : 14; }

static int text_x(void) { return PAD + 2; }
static int text_y(void) { return MENU_H + PAD + 2; }
static int rows_vis(void) { return (win_h - text_y() - PAD - 2) / line_h(); }
static int cols_vis(void) { return (win_w - text_x() - PAD - 2) / char_w(); }

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

    if (!mono) return;
    XSetForeground(dpy, x98.gc, x98.text);
    XSetFont(dpy, x98.gc, mono->fid);

    int rows = rows_vis(), cw = char_w(), lh = line_h();
    for (int r = 0; r < rows; r++) {
        int l = top_l + r;
        if (l >= n_lines) break;
        const char *s = lines[l];
        int len = (int)strlen(s);
        if (left_c >= len) continue;
        int n = len - left_c;
        if (n > cols_vis()) n = cols_vis();
        XDrawString(dpy, win, x98.gc, tx, ty + r * lh + mono->ascent,
                    s + left_c, n);
    }

    /* カーソル */
    if (cur_l >= top_l && cur_l < top_l + rows) {
        int cx = tx + (cur_c - left_c) * cw;
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
    int rows = rows_vis(), cols = cols_vis();
    if (cur_l < top_l) top_l = cur_l;
    if (cur_l >= top_l + rows) top_l = cur_l - rows + 1;
    if (cur_c < left_c) left_c = cur_c;
    if (cur_c >= left_c + cols) left_c = cur_c - cols + 1;
    if (top_l < 0) top_l = 0;
    if (left_c < 0) left_c = 0;
}

static int clampc(void)
{
    int len = (int)strlen(lines[cur_l]);
    if (cur_c > len) cur_c = len;
    return len;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-notepad] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);

    /* 本文は等幅で出す。メモ帳らしさもあるし、桁の計算が楽になる。 */
    mono = XLoadQueryFont(dpy, "9x15");
    if (!mono) mono = XLoadQueryFont(dpy, "fixed");
    if (!mono) mono = x98.font;

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask |
                           StructureNotifyMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XMapWindow(dpy, win);

    lines[0] = line_new("");
    if (argc > 1) doc_load(argv[1]);
    else set_title();

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
                int c = (mx - text_x() + char_w() / 2) / char_w();
                cur_l = top_l + r;
                if (cur_l < 0) cur_l = 0;
                if (cur_l >= n_lines) cur_l = n_lines - 1;
                cur_c = left_c + c;
                clampc();
                scroll_to_cursor();
                redraw();
            }
            break;
        }

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
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
            case XK_BackSpace:
                if (cur_c > 0) {
                    memmove(lines[cur_l] + cur_c - 1, lines[cur_l] + cur_c,
                            strlen(lines[cur_l]) - cur_c + 1);
                    cur_c--;
                } else join_prev();
                modified = 1;
                break;
            case XK_Delete: {
                int len = (int)strlen(lines[cur_l]);
                if (cur_c < len) {
                    memmove(lines[cur_l] + cur_c, lines[cur_l] + cur_c + 1,
                            len - cur_c);
                } else if (cur_l < n_lines - 1) {
                    cur_l++; cur_c = 0; join_prev();
                }
                modified = 1;
                break;
            }
            case XK_Left:  if (cur_c > 0) cur_c--;
                           else if (cur_l > 0) { cur_l--; cur_c = (int)strlen(lines[cur_l]); }
                           break;
            case XK_Right: if (cur_c < (int)strlen(lines[cur_l])) cur_c++;
                           else if (cur_l < n_lines - 1) { cur_l++; cur_c = 0; }
                           break;
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

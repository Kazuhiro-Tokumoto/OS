/* ==========================================================================
 * myos_wm.c  -  Windows 98 風の X11 ウィンドウマネージャ
 *
 * 位置づけ:
 *   フェーズ6 (ウィンドウシステム) の本体。
 *   Firefox のような本物のアプリを載せるために X11 を採用したので、
 *   「Win98 の見た目」を担うのはウィンドウマネージャという形になる。
 *
 *     自作ブートローダー
 *       -> Linux カーネル
 *         -> Xorg (画面を持つ)
 *           -> このウィンドウマネージャ (見た目を持つ)
 *             -> Firefox などのアプリ
 *
 *   src/gui/win98.c がフレームバッファに直接描いていたもの
 *   (立体枠・タイトルバー・タスクバー) を、X のウィンドウに対して
 *   やり直したもの。描き方の考え方はそのまま引き継いでいる。
 *
 * やっていること:
 *   - ルートウィンドウを Win98 のティール色に塗る
 *   - クライアントを枠 (frame) に入れ直す (reparenting)
 *   - タイトルバーを横グラデーションで描き、最小化/最大化/閉じるを付ける
 *   - タイトルバーのドラッグでウィンドウを移動
 *   - 画面下にタスクバー: スタートボタン + タスクボタン + 時計
 *
 * ビルド:
 *   gcc -O2 -o myos-wm myos_wm.c -lX11
 *
 *   ただし rootfs (Debian bookworm / glibc 2.36) と、ビルドするホスト
 *   (Ubuntu 24.04 / glibc 2.39) では glibc の版が違う。
 *   静的リンクで逃げようとすると、libX11 がカーソル作成で libXcursor を
 *   dlopen した瞬間に別版の glibc を読み込んで落ちる。
 *   そのため tools/build_rootfs.sh は rootfs の中でコンパイルしている。
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- Win98 の寸法 -------------------------------------------------------- */
#define MAX_CLIENTS 32
#define BORDER      4       /* フレームの太さ */
#define TITLE_H     20      /* タイトルバーの高さ */
#define BTN_SZ      14      /* 右上のボタン */
#define TASKBAR_H   28
#define START_W     72
#define TASKBTN_W   160
#define MENU_W      168
#define MENU_ITEM_H 22

/* --- Win98 の配色 -------------------------------------------------------- */
#define RGB_DESKTOP   0x008080
#define RGB_FACE      0xC0C0C0
#define RGB_LIGHT     0xFFFFFF
#define RGB_SHADOW    0x808080
#define RGB_DKSHADOW  0x000000
#define RGB_TITLETXT  0xFFFFFF
#define RGB_TEXT      0x000000

static Display *dpy;
static int      screen;
static Window   root;
static int      scr_w, scr_h;
static GC       gc;
static XFontStruct *font;
static Window   taskbar, startmenu;
static int      menu_open = 0;

static Atom a_wm_protocols, a_wm_delete, a_net_wm_name, a_utf8;

static unsigned long px_desktop, px_face, px_light, px_shadow, px_dkshadow,
                     px_titletxt, px_text;

/* TrueColor のマスクからピクセル値を直接組み立てる。
 * 色数が少ないので XAllocColor でもよいが、タイトルバーのグラデーションで
 * 何百色も使うため、確保せずに済むこちらのほうが素直。 */
static int truecolor;
static int r_shift, g_shift, b_shift, r_bits, g_bits, b_bits;

static int mask_shift(unsigned long m)
{
    int s = 0;
    if (!m) return 0;
    while (!(m & 1)) { m >>= 1; s++; }
    return s;
}

static int mask_bits(unsigned long m)
{
    int b = 0;
    m >>= mask_shift(m);
    while (m & 1) { b++; m >>= 1; }
    return b;
}

static unsigned long rgb(int r, int g, int b)
{
    if (truecolor) {
        return (((unsigned long)(r >> (8 - r_bits))) << r_shift)
             | (((unsigned long)(g >> (8 - g_bits))) << g_shift)
             | (((unsigned long)(b >> (8 - b_bits))) << b_shift);
    }
    XColor c;
    c.red = r * 257; c.green = g * 257; c.blue = b * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
    return c.pixel;
}

static unsigned long rgb24(unsigned int v)
{
    return rgb((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

/* --- クライアント -------------------------------------------------------- */
typedef struct {
    Window client;
    Window frame;
    int    x, y, w, h;      /* frame の位置と大きさ */
    int    cw, ch;          /* クライアント領域の大きさ */
    int    used;
    int    minimized;
    int    maximized;
    int    old_x, old_y, old_w, old_h;
    char   title[256];
} Client;

static Client clients[MAX_CLIENTS];

static Client *find_by_client(Window w)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used && clients[i].client == w) return &clients[i];
    return NULL;
}

static Client *find_by_frame(Window w)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used && clients[i].frame == w) return &clients[i];
    return NULL;
}

static Client *alloc_client(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].used) return &clients[i];
    return NULL;
}

/* --- 描画の基本 ---------------------------------------------------------- */
static void fill(Window w, int x, int y, int W, int H, unsigned long p)
{
    if (W <= 0 || H <= 0) return;
    XSetForeground(dpy, gc, p);
    XFillRectangle(dpy, w, gc, x, y, W, H);
}

static void hline(Window w, int x, int y, int len, unsigned long p)
{
    fill(w, x, y, len, 1, p);
}

static void vline(Window w, int x, int y, int len, unsigned long p)
{
    fill(w, x, y, 1, len, p);
}

/* Win98 の立体枠。外側 1px と内側 1px の 2 重になっているのがこの時代の作法。
 * raised=1 で盛り上がり、0 で凹み。 */
static void bevel(Window w, int x, int y, int W, int H, int raised)
{
    unsigned long out_tl = raised ? px_light    : px_shadow;
    unsigned long out_br = raised ? px_dkshadow : px_light;
    unsigned long in_tl  = raised ? px_face     : px_dkshadow;
    unsigned long in_br  = raised ? px_shadow   : px_face;

    hline(w, x, y, W, out_tl);
    vline(w, x, y, H, out_tl);
    hline(w, x, y + H - 1, W, out_br);
    vline(w, x + W - 1, y, H, out_br);

    hline(w, x + 1, y + 1, W - 2, in_tl);
    vline(w, x + 1, y + 1, H - 2, in_tl);
    hline(w, x + 1, y + H - 2, W - 2, in_br);
    vline(w, x + W - 2, y + 1, H - 2, in_br);
}

static void text(Window w, int x, int y, const char *s, unsigned long p)
{
    if (!font) return;
    XSetForeground(dpy, gc, p);
    XSetFont(dpy, gc, font->fid);
    XDrawString(dpy, w, gc, x, y + font->ascent, s, (int)strlen(s));
}

static int text_w(const char *s)
{
    if (!font) return (int)strlen(s) * 6;
    return XTextWidth(font, s, (int)strlen(s));
}

static int text_h(void)
{
    if (!font) return 12;
    return font->ascent + font->descent;
}

static void button(Window w, int x, int y, int W, int H,
                   const char *label, int pressed)
{
    fill(w, x, y, W, H, px_face);
    bevel(w, x, y, W, H, !pressed);
    int tx = x + (W - text_w(label)) / 2 + (pressed ? 1 : 0);
    int ty = y + (H - text_h()) / 2 + (pressed ? 1 : 0);
    text(w, tx, ty, label, px_text);
}

/* --- ウィンドウの枠 ------------------------------------------------------ */
/* 右上のボタンの位置。i=0 が最小化、1 が最大化、2 が閉じる。 */
static void btn_rect(Client *c, int i, int *bx, int *by)
{
    int right = c->w - BORDER - 2;
    *bx = right - (3 - i) * (BTN_SZ + 2) + 2;
    *by = BORDER + (TITLE_H - BTN_SZ) / 2;
}

static void draw_frame(Client *c)
{
    /* 外枠 */
    fill(c->frame, 0, 0, c->w, c->h, px_face);
    bevel(c->frame, 0, 0, c->w, c->h, 1);

    /* タイトルバー: 横方向のグラデーション (濃紺 -> 明るい青) */
    int tx = BORDER, ty = BORDER;
    int tw = c->w - 2 * BORDER, th = TITLE_H;
    for (int i = 0; i < tw; i++) {
        int r = (0x00 * (tw - i) + 0x10 * i) / tw;
        int g = (0x00 * (tw - i) + 0x84 * i) / tw;
        int b = (0x80 * (tw - i) + 0xD0 * i) / tw;
        vline(c->frame, tx + i, ty, th, rgb(r, g, b));
    }
    text(c->frame, tx + 4, ty + (th - text_h()) / 2, c->title, px_titletxt);

    /* 右上のボタン */
    static const char *labels[3] = { "_", "[]", "X" };
    for (int i = 0; i < 3; i++) {
        int bx, by;
        btn_rect(c, i, &bx, &by);
        button(c->frame, bx, by, BTN_SZ, BTN_SZ, labels[i], 0);
    }

    /* クライアント領域の縁を凹ませる */
    bevel(c->frame, BORDER - 2, BORDER + TITLE_H - 1,
          c->w - 2 * (BORDER - 2), c->h - (BORDER + TITLE_H - 1) - (BORDER - 2), 0);
}

static void fetch_title(Client *c)
{
    c->title[0] = 0;

    /* まず _NET_WM_NAME (UTF-8) を見る。最近のアプリはこちらしか設定しない */
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, c->client, a_net_wm_name, 0, 256, False,
                           a_utf8, &type, &fmt, &n, &after, &data) == Success
        && data) {
        snprintf(c->title, sizeof(c->title), "%s", (char *)data);
        XFree(data);
    }
    if (!c->title[0]) {
        char *name = NULL;
        if (XFetchName(dpy, c->client, &name) && name) {
            snprintf(c->title, sizeof(c->title), "%s", name);
            XFree(name);
        }
    }
    if (!c->title[0])
        snprintf(c->title, sizeof(c->title), "%s", "(untitled)");
}

/* --- タスクバー ---------------------------------------------------------- */
static const char *menu_items[] = {
    "Programs", "Documents", "Settings", "Find", "Help", "Run...", "Shut Down"
};
#define MENU_N ((int)(sizeof(menu_items) / sizeof(menu_items[0])))
#define MENU_H (MENU_N * MENU_ITEM_H + 8)

static void draw_startmenu(void)
{
    int w = MENU_W, h = MENU_H;
    fill(startmenu, 0, 0, w, h, px_face);
    bevel(startmenu, 0, 0, w, h, 1);

    /* 左端の縦帯 (Win98 で "Windows 98" と入っているところ) */
    fill(startmenu, 3, 3, 20, h - 6, rgb24(0x000080));
    int sy = h - 16;
    for (const char *p = "myOS"; *p; p++) {
        char s[2] = { *p, 0 };
        text(startmenu, 8, sy, s, px_titletxt);
        sy -= text_h() + 2;
    }

    int iy = 4;
    for (int i = 0; i < MENU_N; i++) {
        text(startmenu, 32, iy + (MENU_ITEM_H - text_h()) / 2,
             menu_items[i], px_text);
        if (i == MENU_N - 2) {
            hline(startmenu, 26, iy + MENU_ITEM_H - 2, w - 32, px_shadow);
            hline(startmenu, 26, iy + MENU_ITEM_H - 1, w - 32, px_light);
        }
        iy += MENU_ITEM_H;
    }
}

static void draw_taskbar(void)
{
    fill(taskbar, 0, 0, scr_w, TASKBAR_H, px_face);
    hline(taskbar, 0, 0, scr_w, px_light);

    /* スタートボタン。旗と文字が重ならないよう、中央揃えではなく
     * 旗の右に文字を置く。 */
    int sb_h = TASKBAR_H - 6;
    fill(taskbar, 3, 3, START_W, sb_h, px_face);
    bevel(taskbar, 3, 3, START_W, sb_h, !menu_open);
    int off = menu_open ? 1 : 0;
    static const unsigned int flag[4] = {
        0xE04040, 0x40C040, 0x4060E0, 0xE0E040
    };
    for (int i = 0; i < 4; i++) {
        int dx = (i & 1) * 6, dy = (i >> 1) * 6;
        fill(taskbar, 9 + off + dx, 8 + off + dy, 5, 5, rgb24(flag[i]));
    }
    text(taskbar, 24 + off, 3 + off + (sb_h - text_h()) / 2, "Start", px_text);

    /* 区切り */
    vline(taskbar, START_W + 8, 4, TASKBAR_H - 8, px_shadow);
    vline(taskbar, START_W + 9, 4, TASKBAR_H - 8, px_light);

    /* タスクボタン */
    int x = START_W + 14;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        if (x + TASKBTN_W > scr_w - 80) break;
        char label[40];
        snprintf(label, sizeof(label), "%.34s", clients[i].title);
        button(taskbar, x, 3, TASKBTN_W, TASKBAR_H - 6, label,
               !clients[i].minimized);
        x += TASKBTN_W + 4;
    }

    /* 時計 (凹み枠) */
    int cw = 62, cx = scr_w - cw - 4;
    fill(taskbar, cx, 3, cw, TASKBAR_H - 6, px_face);
    bevel(taskbar, cx, 3, cw, TASKBAR_H - 6, 0);
    text(taskbar, cx + 10, 3 + (TASKBAR_H - 6 - text_h()) / 2, "12:00", px_text);
}

static int taskbtn_hit(int mx)
{
    int x = START_W + 14;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        if (x + TASKBTN_W > scr_w - 80) break;
        if (mx >= x && mx < x + TASKBTN_W) return i;
        x += TASKBTN_W + 4;
    }
    return -1;
}

/* --- ウィンドウ操作 ------------------------------------------------------ */
static void set_menu(int open)
{
    menu_open = open;
    if (open) {
        XMoveResizeWindow(dpy, startmenu, 2, scr_h - TASKBAR_H - MENU_H,
                          MENU_W, MENU_H);
        XMapRaised(dpy, startmenu);
        draw_startmenu();
    } else {
        XUnmapWindow(dpy, startmenu);
    }
    draw_taskbar();
}

static void raise_client(Client *c)
{
    XRaiseWindow(dpy, c->frame);
    XRaiseWindow(dpy, taskbar);
    if (menu_open) XRaiseWindow(dpy, startmenu);
    XSetInputFocus(dpy, c->client, RevertToPointerRoot, CurrentTime);
}

static void frame_client(Window w, int adopt)
{
    if (find_by_client(w)) return;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;
    if (adopt && wa.map_state != IsViewable) return;

    Client *c = alloc_client();
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->client = w;

    /* クライアント領域の大きさを決める。
     * Firefox は画面より大きい大きさを要求してくることがあるので、
     * タスクバーを避けた作業領域に収まるよう抑える。 */
    int work_h = scr_h - TASKBAR_H;
    int max_cw = scr_w - 2 * BORDER - 16;
    int max_ch = work_h - BORDER - TITLE_H - BORDER - 16;
    c->cw = wa.width  > max_cw ? max_cw : wa.width;
    c->ch = wa.height > max_ch ? max_ch : wa.height;
    if (c->cw < 120) c->cw = max_cw;
    if (c->ch < 80)  c->ch = max_ch;

    c->w = c->cw + 2 * BORDER;
    c->h = c->ch + BORDER + TITLE_H + BORDER;
    c->x = (scr_w - c->w) / 2;
    c->y = (work_h - c->h) / 2;
    if (c->x < 0) c->x = 0;
    if (c->y < 0) c->y = 0;

    c->frame = XCreateSimpleWindow(dpy, root, c->x, c->y, c->w, c->h,
                                   0, 0, px_face);
    XSelectInput(dpy, c->frame,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | SubstructureRedirectMask |
                 SubstructureNotifyMask);
    XAddToSaveSet(dpy, w);
    XReparentWindow(dpy, w, c->frame, BORDER, BORDER + TITLE_H);
    XResizeWindow(dpy, w, c->cw, c->ch);
    XSelectInput(dpy, w, PropertyChangeMask);

    fetch_title(c);

    XMapWindow(dpy, c->frame);
    XMapWindow(dpy, w);
    raise_client(c);
    draw_frame(c);
    draw_taskbar();
}

static void unframe(Client *c)
{
    XUnmapWindow(dpy, c->frame);
    XReparentWindow(dpy, c->client, root, c->x, c->y);
    XRemoveFromSaveSet(dpy, c->client);
    XDestroyWindow(dpy, c->frame);
    c->used = 0;
    draw_taskbar();
}

static void close_client(Client *c)
{
    /* WM_DELETE_WINDOW に対応しているなら行儀よく頼む。
     * 対応していなければ強制的に切る。 */
    Atom *protos = NULL;
    int n = 0, supported = 0;
    if (XGetWMProtocols(dpy, c->client, &protos, &n)) {
        for (int i = 0; i < n; i++)
            if (protos[i] == a_wm_delete) supported = 1;
        if (protos) XFree(protos);
    }
    if (supported) {
        XEvent e;
        memset(&e, 0, sizeof(e));
        e.xclient.type = ClientMessage;
        e.xclient.window = c->client;
        e.xclient.message_type = a_wm_protocols;
        e.xclient.format = 32;
        e.xclient.data.l[0] = (long)a_wm_delete;
        e.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, c->client, False, NoEventMask, &e);
    } else {
        XKillClient(dpy, c->client);
    }
}

static void set_geometry(Client *c)
{
    XMoveResizeWindow(dpy, c->frame, c->x, c->y, c->w, c->h);
    XMoveResizeWindow(dpy, c->client, BORDER, BORDER + TITLE_H, c->cw, c->ch);
    draw_frame(c);
}

static void toggle_max(Client *c)
{
    if (c->maximized) {
        c->x = c->old_x; c->y = c->old_y;
        c->w = c->old_w; c->h = c->old_h;
        c->maximized = 0;
    } else {
        c->old_x = c->x; c->old_y = c->y;
        c->old_w = c->w; c->old_h = c->h;
        c->x = 0; c->y = 0;
        c->w = scr_w;
        c->h = scr_h - TASKBAR_H;
        c->maximized = 1;
    }
    c->cw = c->w - 2 * BORDER;
    c->ch = c->h - BORDER - TITLE_H - BORDER;
    set_geometry(c);
}

static void minimize(Client *c)
{
    c->minimized = 1;
    XUnmapWindow(dpy, c->frame);
    draw_taskbar();
}

static void restore(Client *c)
{
    c->minimized = 0;
    XMapWindow(dpy, c->frame);
    raise_client(c);
    draw_taskbar();
}

/* ルートウィンドウの矢印カーソル。
 *
 * 注意: このプログラムは静的リンクしてはいけない。
 * libX11 はカーソルを作るときに libXcursor を dlopen する。
 * 静的リンクした実行ファイルで dlopen が起きると、リンク時とは別の版の
 * glibc が読み込まれて落ちる (Debian bookworm の rootfs で SIGFPE になった)。
 * そのため tools/build_rootfs.sh は rootfs の中でこれをコンパイルしている。 */
static void set_root_cursor(void)
{
    XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
}

/* --- エラーハンドラ ------------------------------------------------------ */
/* ウィンドウが消える競合はごく普通に起きる。落とさず握り潰す。 */
static int ignore_errors(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    return 0;
}

static int other_wm_running = 0;
static int detect_other_wm(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) other_wm_running = 1;
    return 0;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-wm] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);
    scr_w  = DisplayWidth(dpy, screen);
    scr_h  = DisplayHeight(dpy, screen);

    Visual *vis = DefaultVisual(dpy, screen);
    truecolor = (vis->class == TrueColor || vis->class == DirectColor);
    if (truecolor) {
        r_shift = mask_shift(vis->red_mask);
        g_shift = mask_shift(vis->green_mask);
        b_shift = mask_shift(vis->blue_mask);
        r_bits  = mask_bits(vis->red_mask);
        g_bits  = mask_bits(vis->green_mask);
        b_bits  = mask_bits(vis->blue_mask);
    }

    px_desktop  = rgb24(RGB_DESKTOP);
    px_face     = rgb24(RGB_FACE);
    px_light    = rgb24(RGB_LIGHT);
    px_shadow   = rgb24(RGB_SHADOW);
    px_dkshadow = rgb24(RGB_DKSHADOW);
    px_titletxt = rgb24(RGB_TITLETXT);
    px_text     = rgb24(RGB_TEXT);

    a_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_net_wm_name  = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_utf8         = XInternAtom(dpy, "UTF8_STRING", False);

    gc = XCreateGC(dpy, root, 0, NULL);

    font = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal--12-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "9x15bold");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) fprintf(stderr, "[myos-wm] no usable font, drawing without text\n");

    /* 既に別の WM がいないか確かめる */
    XSetErrorHandler(detect_other_wm);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                            ButtonPressMask);
    XSync(dpy, False);
    if (other_wm_running) {
        fprintf(stderr, "[myos-wm] another window manager is already running\n");
        return 1;
    }
    XSetErrorHandler(ignore_errors);

    /* デスクトップをティール色に */
    XSetWindowBackground(dpy, root, px_desktop);
    XClearWindow(dpy, root);
    set_root_cursor();

    /* タスクバーとスタートメニュー (override_redirect で自分の管理外にする) */
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = px_face;

    taskbar = XCreateWindow(dpy, root, 0, scr_h - TASKBAR_H, scr_w, TASKBAR_H,
                            0, CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel, &swa);
    XSelectInput(dpy, taskbar, ExposureMask | ButtonPressMask);
    XMapRaised(dpy, taskbar);

    startmenu = XCreateWindow(dpy, root, 2, scr_h - TASKBAR_H - MENU_H,
                              MENU_W, MENU_H, 0, CopyFromParent, InputOutput,
                              CopyFromParent, CWOverrideRedirect | CWBackPixel,
                              &swa);
    XSelectInput(dpy, startmenu, ExposureMask | ButtonPressMask);

    /* 既にいるウィンドウを引き取る (WM が後から起動した場合) */
    Window r2, parent, *kids = NULL;
    unsigned int nkids = 0;
    if (XQueryTree(dpy, root, &r2, &parent, &kids, &nkids)) {
        for (unsigned int i = 0; i < nkids; i++)
            if (kids[i] != taskbar && kids[i] != startmenu)
                frame_client(kids[i], 1);
        if (kids) XFree(kids);
    }

    fprintf(stderr, "[myos-wm] running on %dx%d\n", scr_w, scr_h);

    /* --- イベントループ --- */
    int dragging = 0, drag_dx = 0, drag_dy = 0;
    Client *drag_c = NULL;

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {

        case MapRequest:
            frame_client(ev.xmaprequest.window, 0);
            break;

        case ConfigureRequest: {
            XConfigureRequestEvent *e = &ev.xconfigurerequest;
            Client *c = find_by_client(e->window);
            if (!c) {
                /* まだ管理下にないものは要求どおりに */
                XWindowChanges wc;
                wc.x = e->x; wc.y = e->y;
                wc.width = e->width; wc.height = e->height;
                wc.border_width = e->border_width;
                wc.sibling = e->above; wc.stack_mode = e->detail;
                XConfigureWindow(dpy, e->window, e->value_mask, &wc);
            } else {
                /* 管理下のものは枠の中に収める。大きさの変更だけ受ける */
                if (e->value_mask & (CWWidth | CWHeight)) {
                    int nw = (e->value_mask & CWWidth)  ? e->width  : c->cw;
                    int nh = (e->value_mask & CWHeight) ? e->height : c->ch;
                    int max_cw = scr_w - 2 * BORDER;
                    int max_ch = scr_h - TASKBAR_H - BORDER - TITLE_H - BORDER;
                    if (nw > max_cw) nw = max_cw;
                    if (nh > max_ch) nh = max_ch;
                    c->cw = nw; c->ch = nh;
                    c->w = nw + 2 * BORDER;
                    c->h = nh + BORDER + TITLE_H + BORDER;
                    set_geometry(c);
                }
            }
            break;
        }

        case UnmapNotify: {
            Client *c = find_by_client(ev.xunmap.window);
            /* 自分で最小化したときの Unmap は無視する */
            if (c && !c->minimized && ev.xunmap.event != root)
                unframe(c);
            break;
        }

        case DestroyNotify: {
            Client *c = find_by_client(ev.xdestroywindow.window);
            if (c) unframe(c);
            break;
        }

        case PropertyNotify: {
            Client *c = find_by_client(ev.xproperty.window);
            if (c && (ev.xproperty.atom == XA_WM_NAME ||
                      ev.xproperty.atom == a_net_wm_name)) {
                fetch_title(c);
                draw_frame(c);
                draw_taskbar();
            }
            break;
        }

        case Expose: {
            if (ev.xexpose.count) break;
            if (ev.xexpose.window == taskbar) { draw_taskbar(); break; }
            if (ev.xexpose.window == startmenu) { draw_startmenu(); break; }
            Client *c = find_by_frame(ev.xexpose.window);
            if (c) draw_frame(c);
            break;
        }

        case ButtonPress: {
            Window w = ev.xbutton.window;

            if (w == taskbar) {
                if (ev.xbutton.x < START_W + 6) {
                    set_menu(!menu_open);
                } else {
                    int i = taskbtn_hit(ev.xbutton.x);
                    if (i >= 0) {
                        Client *c = &clients[i];
                        if (c->minimized) restore(c);
                        else raise_client(c);
                    }
                    set_menu(0);
                }
                break;
            }

            if (w == startmenu) {
                /* 項目を選んだ。今はまだ閉じるだけ */
                set_menu(0);
                break;
            }

            Client *c = find_by_frame(w);
            if (!c) { set_menu(0); break; }

            raise_client(c);
            set_menu(0);

            int mx = ev.xbutton.x, my = ev.xbutton.y;

            /* 右上のボタン */
            int hit = -1;
            for (int i = 0; i < 3; i++) {
                int bx, by;
                btn_rect(c, i, &bx, &by);
                if (mx >= bx && mx < bx + BTN_SZ &&
                    my >= by && my < by + BTN_SZ) { hit = i; break; }
            }
            if (hit == 0) { minimize(c); break; }
            if (hit == 1) { toggle_max(c); break; }
            if (hit == 2) { close_client(c); break; }

            /* タイトルバーを掴んだらドラッグ開始 */
            if (my >= BORDER && my < BORDER + TITLE_H && !c->maximized) {
                dragging = 1;
                drag_c = c;
                drag_dx = ev.xbutton.x_root - c->x;
                drag_dy = ev.xbutton.y_root - c->y;
            }
            break;
        }

        case MotionNotify: {
            if (!dragging || !drag_c) break;
            /* たまった移動イベントは間引く。追従が重くなるのを防ぐ */
            while (XCheckTypedEvent(dpy, MotionNotify, &ev)) { }
            drag_c->x = ev.xmotion.x_root - drag_dx;
            drag_c->y = ev.xmotion.y_root - drag_dy;
            if (drag_c->y < 0) drag_c->y = 0;
            if (drag_c->y > scr_h - TASKBAR_H - TITLE_H)
                drag_c->y = scr_h - TASKBAR_H - TITLE_H;
            if (drag_c->x < -(drag_c->w - 80)) drag_c->x = -(drag_c->w - 80);
            if (drag_c->x > scr_w - 80) drag_c->x = scr_w - 80;
            XMoveWindow(dpy, drag_c->frame, drag_c->x, drag_c->y);
            break;
        }

        case ButtonRelease:
            dragging = 0;
            drag_c = NULL;
            break;

        default:
            break;
        }
    }
    return 0;
}

/* ==========================================================================
 * x98.h  -  X11 の上に Windows 98 風の見た目を描くための共通部品
 *
 * ウィンドウマネージャ (myos_wm.c) とファイルマネージャ (myos_files.c) の
 * 両方から使う。static 関数だけのヘッダなので、include するだけでよい。
 *
 * Win98 らしさの肝は 2 重の立体枠 (bevel)。
 * 外側 1px と内側 1px の 2 段になっていて、これをボタン・枠・リスト領域と
 * 使い回すだけでかなりそれらしくなる。
 * ========================================================================== */
#ifndef MYOS_X98_H
#define MYOS_X98_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <string.h>

/* --- Win98 の配色 -------------------------------------------------------- */
#define RGB_DESKTOP   0x008080      /* ティール */
#define RGB_FACE      0xC0C0C0      /* ボタン面 */
#define RGB_LIGHT     0xFFFFFF      /* ハイライト */
#define RGB_SHADOW    0x808080      /* 影 */
#define RGB_DKSHADOW  0x000000      /* 濃い影 */
#define RGB_TITLE1    0x000080      /* タイトルバー (濃) */
#define RGB_TITLE2    0x1084D0      /* タイトルバー (淡) */
#define RGB_TITLETXT  0xFFFFFF
#define RGB_TEXT      0x000000
#define RGB_WHITE     0xFFFFFF
#define RGB_SELECT    0x000080      /* 選択中の背景 */
#define RGB_FOLDER    0xFFD060      /* フォルダの黄色 */

typedef struct {
    Display *dpy;
    int      screen;
    GC       gc;
    XFontStruct *font;
    int      truecolor;
    int      r_shift, g_shift, b_shift;
    int      r_bits, g_bits, b_bits;

    unsigned long desktop, face, light, shadow, dkshadow;
    unsigned long titletxt, text, white, select_bg, folder;
} X98;

static inline int x98_mask_shift(unsigned long m)
{
    int s = 0;
    if (!m) return 0;
    while (!(m & 1)) { m >>= 1; s++; }
    return s;
}

static inline int x98_mask_bits(unsigned long m)
{
    int b = 0;
    m >>= x98_mask_shift(m);
    while (m & 1) { b++; m >>= 1; }
    return b;
}

/* TrueColor のマスクからピクセル値を直接組み立てる。
 * タイトルバーのグラデーションで何百色も使うので、
 * XAllocColor で毎回確保するよりこちらのほうが素直。 */
static inline unsigned long x98_rgb(X98 *x, int r, int g, int b)
{
    if (x->truecolor) {
        return (((unsigned long)(r >> (8 - x->r_bits))) << x->r_shift)
             | (((unsigned long)(g >> (8 - x->g_bits))) << x->g_shift)
             | (((unsigned long)(b >> (8 - x->b_bits))) << x->b_shift);
    }
    XColor c;
    c.red = r * 257; c.green = g * 257; c.blue = b * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(x->dpy, DefaultColormap(x->dpy, x->screen), &c);
    return c.pixel;
}

static inline unsigned long x98_rgb24(X98 *x, unsigned int v)
{
    return x98_rgb(x, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

/* 描画用のフォントを開く。見つからなければ NULL のまま (文字は描かれない)。 */
static inline void x98_open_font(X98 *x)
{
    x->font = XLoadQueryFont(x->dpy,
                             "-*-helvetica-bold-r-normal--12-*-*-*-*-*-*-*");
    if (!x->font) x->font = XLoadQueryFont(x->dpy, "9x15bold");
    if (!x->font) x->font = XLoadQueryFont(x->dpy, "fixed");
}

static inline void x98_init(X98 *x, Display *dpy, int screen)
{
    memset(x, 0, sizeof(*x));
    x->dpy = dpy;
    x->screen = screen;
    x->gc = XCreateGC(dpy, RootWindow(dpy, screen), 0, NULL);

    Visual *vis = DefaultVisual(dpy, screen);
    x->truecolor = (vis->class == TrueColor || vis->class == DirectColor);
    if (x->truecolor) {
        x->r_shift = x98_mask_shift(vis->red_mask);
        x->g_shift = x98_mask_shift(vis->green_mask);
        x->b_shift = x98_mask_shift(vis->blue_mask);
        x->r_bits  = x98_mask_bits(vis->red_mask);
        x->g_bits  = x98_mask_bits(vis->green_mask);
        x->b_bits  = x98_mask_bits(vis->blue_mask);
    }

    x->desktop   = x98_rgb24(x, RGB_DESKTOP);
    x->face      = x98_rgb24(x, RGB_FACE);
    x->light     = x98_rgb24(x, RGB_LIGHT);
    x->shadow    = x98_rgb24(x, RGB_SHADOW);
    x->dkshadow  = x98_rgb24(x, RGB_DKSHADOW);
    x->titletxt  = x98_rgb24(x, RGB_TITLETXT);
    x->text      = x98_rgb24(x, RGB_TEXT);
    x->white     = x98_rgb24(x, RGB_WHITE);
    x->select_bg = x98_rgb24(x, RGB_SELECT);
    x->folder    = x98_rgb24(x, RGB_FOLDER);

    x98_open_font(x);
}

/* --- 基本の描画 ---------------------------------------------------------- */
static inline void x98_fill(X98 *x, Drawable d, int px, int py, int w, int h,
                     unsigned long color)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(x->dpy, x->gc, color);
    XFillRectangle(x->dpy, d, x->gc, px, py, w, h);
}

static inline void x98_hline(X98 *x, Drawable d, int px, int py, int len,
                      unsigned long c)
{
    x98_fill(x, d, px, py, len, 1, c);
}

static inline void x98_vline(X98 *x, Drawable d, int px, int py, int len,
                      unsigned long c)
{
    x98_fill(x, d, px, py, 1, len, c);
}

/* Win98 の立体枠。raised=1 で盛り上がり、0 で凹み。 */
static inline void x98_bevel(X98 *x, Drawable d, int px, int py, int w, int h,
                      int raised)
{
    unsigned long out_tl = raised ? x->light    : x->shadow;
    unsigned long out_br = raised ? x->dkshadow : x->light;
    unsigned long in_tl  = raised ? x->face     : x->dkshadow;
    unsigned long in_br  = raised ? x->shadow   : x->face;

    x98_hline(x, d, px, py, w, out_tl);
    x98_vline(x, d, px, py, h, out_tl);
    x98_hline(x, d, px, py + h - 1, w, out_br);
    x98_vline(x, d, px + w - 1, py, h, out_br);

    x98_hline(x, d, px + 1, py + 1, w - 2, in_tl);
    x98_vline(x, d, px + 1, py + 1, h - 2, in_tl);
    x98_hline(x, d, px + 1, py + h - 2, w - 2, in_br);
    x98_vline(x, d, px + w - 2, py + 1, h - 2, in_br);
}

static inline int x98_text_w(X98 *x, const char *s)
{
    if (!x->font) return (int)strlen(s) * 6;
    return XTextWidth(x->font, s, (int)strlen(s));
}

static inline int x98_text_h(X98 *x)
{
    if (!x->font) return 12;
    return x->font->ascent + x->font->descent;
}

static inline void x98_text(X98 *x, Drawable d, int px, int py, const char *s,
                     unsigned long color)
{
    if (!x->font || !s) return;
    XSetForeground(x->dpy, x->gc, color);
    XSetFont(x->dpy, x->gc, x->font->fid);
    XDrawString(x->dpy, d, x->gc, px, py + x->font->ascent, s, (int)strlen(s));
}

/* 影付きの文字。デスクトップの白抜きラベル用。 */
static inline void x98_text_sh(X98 *x, Drawable d, int px, int py, const char *s,
                        unsigned long fg, unsigned long sh)
{
    x98_text(x, d, px + 1, py + 1, s, sh);
    x98_text(x, d, px, py, s, fg);
}

static inline void x98_button(X98 *x, Drawable d, int px, int py, int w, int h,
                       const char *label, int pressed)
{
    x98_fill(x, d, px, py, w, h, x->face);
    x98_bevel(x, d, px, py, w, h, !pressed);
    int off = pressed ? 1 : 0;
    int tx = px + (w - x98_text_w(x, label)) / 2 + off;
    int ty = py + (h - x98_text_h(x)) / 2 + off;
    x98_text(x, d, tx, ty, label, x->text);
}

/* タイトルバーの横グラデーション (濃紺 -> 明るい青) */
static inline void x98_titlebar(X98 *x, Drawable d, int px, int py, int w, int h,
                         const char *title)
{
    for (int i = 0; i < w; i++) {
        int r = (0x00 * (w - i) + 0x10 * i) / w;
        int g = (0x00 * (w - i) + 0x84 * i) / w;
        int b = (0x80 * (w - i) + 0xD0 * i) / w;
        x98_vline(x, d, px + i, py, h, x98_rgb(x, r, g, b));
    }
    if (title)
        x98_text(x, d, px + 4, py + (h - x98_text_h(x)) / 2, title,
                 x->titletxt);
}

/* --- Win98 風のアイコン (小さな図形の組み合わせで描く) -------------------- */
#define X98_ICON_W 32
#define X98_ICON_H 32

/* マイコンピュータ: モニタ */
static inline void x98_icon_computer(X98 *x, Drawable d, int px, int py)
{
    x98_fill(x, d, px + 4, py + 2, 24, 18, x->face);
    x98_bevel(x, d, px + 4, py + 2, 24, 18, 1);
    x98_fill(x, d, px + 7, py + 5, 18, 11, x98_rgb24(x, 0x0000A0));
    x98_fill(x, d, px + 11, py + 20, 10, 3, x->face);
    x98_fill(x, d, px + 4, py + 23, 24, 5, x->face);
    x98_bevel(x, d, px + 4, py + 23, 24, 5, 1);
}

/* フォルダ */
static inline void x98_icon_folder(X98 *x, Drawable d, int px, int py)
{
    x98_fill(x, d, px + 3, py + 6, 11, 3, x->folder);
    x98_fill(x, d, px + 2, py + 9, 27, 17, x->folder);
    x98_hline(x, d, px + 2, py + 9, 27, x98_rgb24(x, 0xFFF0B0));
    x98_hline(x, d, px + 2, py + 25, 27, x98_rgb24(x, 0xA07000));
    x98_vline(x, d, px + 28, py + 9, 17, x98_rgb24(x, 0xA07000));
}

/* 書類 */
static inline void x98_icon_file(X98 *x, Drawable d, int px, int py)
{
    x98_fill(x, d, px + 6, py + 3, 20, 26, x->white);
    x98_bevel(x, d, px + 6, py + 3, 20, 26, 0);
    for (int i = 0; i < 5; i++)
        x98_hline(x, d, px + 9, py + 8 + i * 4, 14, x->shadow);
}

/* アプリ (ウィンドウ風) */
static inline void x98_icon_app(X98 *x, Drawable d, int px, int py)
{
    x98_fill(x, d, px + 3, py + 4, 26, 24, x->face);
    x98_bevel(x, d, px + 3, py + 4, 26, 24, 1);
    x98_fill(x, d, px + 5, py + 6, 22, 5, x98_rgb24(x, RGB_TITLE1));
    x98_fill(x, d, px + 5, py + 13, 22, 13, x->white);
}

/* 地球 (ブラウザ) */
static inline void x98_icon_globe(X98 *x, Drawable d, int px, int py)
{
    XSetForeground(x->dpy, x->gc, x98_rgb24(x, 0x2060C0));
    XFillArc(x->dpy, d, x->gc, px + 3, py + 3, 26, 26, 0, 360 * 64);
    XSetForeground(x->dpy, x->gc, x98_rgb24(x, 0x80D0FF));
    XDrawArc(x->dpy, d, x->gc, px + 3, py + 3, 26, 26, 0, 360 * 64);
    XDrawArc(x->dpy, d, x->gc, px + 11, py + 3, 10, 26, 0, 360 * 64);
    XDrawLine(x->dpy, d, x->gc, px + 3, py + 16, px + 29, py + 16);
}

/* コーヒーカップ (Java) */
static inline void x98_icon_java(X98 *x, Drawable d, int px, int py)
{
    x98_fill(x, d, px + 7, py + 12, 16, 14, x->white);
    x98_bevel(x, d, px + 7, py + 12, 16, 14, 1);
    x98_fill(x, d, px + 23, py + 15, 4, 7, x->white);
    x98_bevel(x, d, px + 23, py + 15, 4, 7, 1);
    XSetForeground(x->dpy, x->gc, x98_rgb24(x, 0xE04040));
    for (int i = 0; i < 3; i++)
        XDrawArc(x->dpy, x->gc ? d : d, x->gc,
                 px + 9 + i * 4, py + 4, 4, 8, 0, 180 * 64);
}

#endif /* MYOS_X98_H */

/* ============================================================================
 * myos_splash  -  起動中の画面を /dev/fb0 に直接描く
 *
 * なぜ要るか
 * ----------
 * カーネルを quiet にしたら、ブートローダーの表示が消えてから X が上がる
 * までの 7 秒ほど、画面に何も出なくなった。QEMU で測るとこうなる。
 *
 *   0- 7 秒   720x400   ブートローダーの表示と点
 *   7-14 秒   1024x768  真っ黒            ← ここ
 *   14 秒-    1280x800  デスクトップ
 *
 * ログが滝のように流れるのも困るが、何も出ないのも「固まった」と
 * 区別がつかない。98 はここを起動画面で埋めていた。同じことをする。
 *
 * つくり
 * ------
 * X はまだ上がっていないので Xlib は使えない。ブートローダーが VBE で
 * 用意したフレームバッファに直接書く。
 *
 * mmap ではなく write() で流し込む。DRM の fbdev エミュレーションでは
 * mmap 経由の書き込みは deferred I/O (HZ/20 遅れ) でしか画面に反映されず、
 * こちらが書いた直後に munmap して閉じると、反映される前に取り消されて
 * 何も出ない。実際それで真っ暗のままだった。write() なら
 *
 *   __FB_DEFAULT_DEFERRED_OPS_RDWR() -> fb_sys_write -> damage_range
 *
 * と、その場で damage が走る。
 *
 * 描くたびに開き直しているのは、途中で画面が挿げ替わるため。最初は
 * simpledrm が /dev/fb0 を持っているが、KMS ドライバ (bochs-drm や i915)
 * が probe を終えると解像度ごと乗っ取る。開き直せば必ず今の画面に書く。
 *
 * 文字は freetype で直接ラスタライズする。Xft と違って X を要らない。
 * フォントが無ければ図形だけで描く (文字が出ないだけで画面は出る)。
 *
 *   myos-splash            描き続ける。SIGTERM で消えて抜ける
 *   myos-splash --test     フレームバッファが使えるかだけ見て終わる
 *   myos-splash --once "文" 一枚だけ描いて終わる。終了のときに使う
 * ==========================================================================*/
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define FBDEV "/dev/fb0"

/* 帯の上に出す一行。起動と終了で変える。 */
static const char *sub_text = "Starting...";
/* 帯を出すか。終了のときは何も進んでいないので出さない。
 * 空の枠だけ置くと「0% で固まっている」ように見える。 */
static int show_bar = 1;

static volatile sig_atomic_t stop_now = 0;
static void on_term(int s) { (void)s; stop_now = 1; }

/* --- フレームバッファ ---------------------------------------------------- */
typedef struct {
    int    fd;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
} FB;

static void fb_close(FB *fb)
{
    if (fb->fd >= 0) close(fb->fd);
    fb->fd = -1;
}

static int fb_open(FB *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->fd = open(FBDEV, O_RDWR);
    if (fb->fd < 0) return -1;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->v) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->f) < 0 ||
        fb->v.bits_per_pixel < 16 || fb->v.xres == 0 || fb->v.yres == 0) {
        close(fb->fd); fb->fd = -1; return -1;
    }
    return 0;
}

/* 画面の並びに合わせて 1 画素ぶんのビットを作る。
 * 16bpp (5-6-5) と 32bpp で並びが違うので、必ず offset/length を見る。 */
static uint32_t pack(const FB *fb, int r, int g, int b)
{
    uint32_t px = 0;
    px |= (uint32_t)(r >> (8 - fb->v.red.length))   << fb->v.red.offset;
    px |= (uint32_t)(g >> (8 - fb->v.green.length)) << fb->v.green.offset;
    px |= (uint32_t)(b >> (8 - fb->v.blue.length))  << fb->v.blue.offset;
    return px;
}

/* 画面を実際に出させる。
 *
 * /dev/fb0 が出来ていても、その中身が走査されているとは限らない。実際、
 * bochs-drm が 1280x800 の fbdev を作ったあとも、画面は前の 1024x768 の
 * まま数秒間そこにいた。write() は成功しているのに何も見えない、という
 * 分かりにくい壊れ方になる。
 *
 * FBIOPUT_VSCREENINFO で fb_set_par を蹴っても駄目だった (rc=0 だが画面は
 * 変わらない)。実際に効いたのはコンソールに 1 バイト書くこと。fbcon が
 * 描きに行く過程で新しい fbdev にモードを入れ直す。
 *
 * 消去 + カーソル原点だけを送る。文字は出ないし、どのみち直後に
 * こちらが全面を描き直す。 */
static void poke_console(void)
{
    static const char clr[] = "\033[2J\033[H";
    static const char *dev[] = { "/dev/tty0", "/dev/console", NULL };
    for (int i = 0; dev[i]; i++) {
        int fd = open(dev[i], O_WRONLY | O_NOCTTY);
        if (fd < 0) continue;
        ssize_t n = write(fd, clr, sizeof(clr) - 1);
        (void)n;
        close(fd);
        return;
    }
}

/* --- 合成用のバッファ ---------------------------------------------------- */
/* 直接フレームバッファへ点を打つと、書き込みがキャッシュされない領域なので
 * 遅い。いったん普通のメモリで組み立ててから、行ごとに流し込む。 */
typedef struct {
    int w, h;
    uint8_t *rgb;               /* w*h*3 */
} Canvas;

static void cv_free(Canvas *c) { free(c->rgb); c->rgb = NULL; }

static int cv_alloc(Canvas *c, int w, int h)
{
    cv_free(c);
    c->w = w; c->h = h;
    c->rgb = calloc((size_t)w * h, 3);
    return c->rgb ? 0 : -1;
}

static void cv_px(Canvas *c, int x, int y, int r, int g, int b)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    uint8_t *p = c->rgb + ((size_t)y * c->w + x) * 3;
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
}

static void cv_fill(Canvas *c, int x, int y, int w, int h, unsigned int rgb)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            cv_px(c, x + i, y + j,
                  (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/* 8 ビットの濃さで色を乗せる。文字の縁を滑らかにするのに使う。 */
static void cv_blend(Canvas *c, int x, int y, unsigned int rgb, int a)
{
    if (a <= 0 || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    if (a > 255) a = 255;
    uint8_t *p = c->rgb + ((size_t)y * c->w + x) * 3;
    int sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    p[0] = (uint8_t)((sr * a + p[0] * (255 - a)) / 255);
    p[1] = (uint8_t)((sg * a + p[1] * (255 - a)) / 255);
    p[2] = (uint8_t)((sb * a + p[2] * (255 - a)) / 255);
}

/* 画面と同じ並び (line_length の刻み) に組んでから、一度の write で流す。 */
static int cv_blit(const Canvas *c, FB *fb, uint8_t **buf, size_t *buflen,
                   int y0, int y1)
{
    int bytes = fb->v.bits_per_pixel / 8;
    size_t stride = fb->f.line_length;
    int last = c->h < (int)fb->v.yres ? c->h : (int)fb->v.yres;
    if (y1 > last) y1 = last;
    if (y0 < 0) y0 = 0;
    if (y1 <= y0) return 0;
    int rows = y1 - y0;
    size_t need = stride * (size_t)rows;

    if (*buflen < need) {
        uint8_t *nb = realloc(*buf, need);
        if (!nb) return -1;
        *buf = nb; *buflen = need;
    }
    memset(*buf, 0, need);

    for (int y = 0; y < rows; y++) {
        uint8_t *row = *buf + (size_t)y * stride;
        const uint8_t *src = c->rgb + (size_t)(y + y0) * c->w * 3;
        for (int x = 0; x < c->w && x < (int)fb->v.xres; x++) {
            uint32_t px = pack(fb, src[0], src[1], src[2]);
            src += 3;
            switch (bytes) {
            case 2: *(uint16_t *)(row + x * 2) = (uint16_t)px; break;
            case 4: *(uint32_t *)(row + x * 4) = px;           break;
            default:
                row[x * 3]     = px & 0xFF;
                row[x * 3 + 1] = (px >> 8) & 0xFF;
                row[x * 3 + 2] = (px >> 16) & 0xFF;
                break;
            }
        }
    }

    /* 先頭から詰めて書く。yoffset がある画面はパンしていることになるが、
     * 起動中にパンしている画面は無いので考えない。 */
    size_t done = 0;
    off_t base = (off_t)y0 * (off_t)stride;
    while (done < need) {
        ssize_t n = pwrite(fb->fd, *buf + done, need - done, base + (off_t)done);
        if (n <= 0) {
            fprintf(stderr, "myos-splash: write %zu/%zu bytes: %s\n",
                    done, need, strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* --- 文字 ---------------------------------------------------------------- */
static FT_Library ftlib;
static FT_Face    ftface;
static int        font_ok = 0;

static void font_init(void)
{
    static const char *cand[] = {
        "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
        "/usr/share/fonts/truetype/vlgothic/VL-PGothic-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL
    };
    if (FT_Init_FreeType(&ftlib)) return;
    for (int i = 0; cand[i]; i++)
        if (!FT_New_Face(ftlib, cand[i], 0, &ftface)) { font_ok = 1; return; }
}

/* 中央揃えで 1 行描く。戻り値は描いた幅。font が無ければ 0 を返す。 */
static int text_center(Canvas *c, int cx, int baseline, const char *s,
                       int px, unsigned int rgb)
{
    if (!font_ok) return 0;
    if (FT_Set_Pixel_Sizes(ftface, 0, px)) return 0;

    /* 先に幅を測る。2 回まわすが、起動画面なので気にしなくてよい。 */
    int total = 0;
    for (const char *p = s; *p; p++) {
        if (FT_Load_Char(ftface, (unsigned char)*p, FT_LOAD_RENDER)) continue;
        total += (int)(ftface->glyph->advance.x >> 6);
    }
    int x = cx - total / 2;
    for (const char *p = s; *p; p++) {
        if (FT_Load_Char(ftface, (unsigned char)*p, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot g = ftface->glyph;
        for (unsigned int j = 0; j < g->bitmap.rows; j++)
            for (unsigned int i = 0; i < g->bitmap.width; i++)
                cv_blend(c, x + g->bitmap_left + (int)i,
                         baseline - g->bitmap_top + (int)j, rgb,
                         g->bitmap.buffer[j * g->bitmap.pitch + i]);
        x += (int)(g->advance.x >> 6);
    }
    return total;
}

/* --- 画面を組み立てる ---------------------------------------------------- */
/* 帯の位置。静止部分と動く部分の両方から要るので 1 か所で決める。 */
static void bar_geom(int W, int H, int *bx, int *by, int *bw, int *bh)
{
    *bw = W / 3;   if (*bw < 160) *bw = 160;
    *bh = H / 38;  if (*bh < 10)  *bh = 10;
    *bx = (W - *bw) / 2;
    *by = H - H / 4;
}

/* 背景・名前・帯の枠。起動中は変わらないので 1 回だけ描く。 */
static void compose_static(Canvas *c)
{
    int W = c->w, H = c->h;

    /* 背景。上が黒、下がわずかに青い縦のグラデーション。
     * 真っ黒だと「消えている」のと区別がつかないので、少しだけ色を置く。 */
    for (int y = 0; y < H; y++) {
        int b = 8 + (int)(56.0 * y / (H ? H : 1));
        for (int x = 0; x < W; x++) cv_px(c, x, y, 0, 0, b);
    }

    /* 名前だけ。図形の記章は置かない。
     * 最初は 4 色の四角を 2x2 に並べていたが、あれは Windows の旗
     * そのもので、配るものに載せてよい絵ではない。 */
    int name_px = H / 5; if (name_px < 32) name_px = 32;
    int base = H / 2 + name_px / 3;
    text_center(c, W / 2, base, "myOS", name_px, 0xFFFFFF);

    /* 名前の下に細い線を 1 本だけ引く。文字が宙に浮いて見えるのを防ぐ。 */
    int rw = W / 5;
    cv_fill(c, (W - rw) / 2, base + name_px / 3, rw, 1, 0x303050);

    int bx, by, bw, bh;
    bar_geom(W, H, &bx, &by, &bw, &bh);
    if (show_bar) {
        cv_fill(c, bx - 1, by - 1, bw + 2, bh + 2, 0x202040);
        cv_fill(c, bx, by, bw, bh, 0x000018);
    }

    int small = H / 40; if (small < 11) small = 11;
    text_center(c, W / 2, by - small, sub_text, small, 0xB0B0C0);
}

/* 動くのは帯だけ。98 と同じで、明るい塊が左から右へ流れる。
 * 何割終わったかは分からないので、割合は出さない。出すと嘘になる。
 *
 * clean には静止状態の帯の行が入っている。毎回そこから塗り直す。
 * 全画面を組み直すと 1M 画素ぶんの計算と 4MB の書き込みが 5 回/秒 走り、
 * 2 コアの機械では X の起動と食い合って起動が目に見えて遅くなる。 */
static void compose_bar(Canvas *c, const uint8_t *clean, int frame,
                        int *out_y0, int *out_y1)
{
    int W = c->w, H = c->h;
    int bx, by, bw, bh;
    bar_geom(W, H, &bx, &by, &bw, &bh);

    int y0 = by - 1, y1 = by + bh + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    memcpy(c->rgb + (size_t)y0 * W * 3, clean,
           (size_t)(y1 - y0) * W * 3);

    int blk = bw / 4;
    int span = bw + blk;
    int pos = (frame * (span / 22 + 1)) % span - blk;
    for (int i = 0; i < blk; i++) {
        int x = bx + pos + i;
        if (x < bx || x >= bx + bw) continue;
        /* 端は暗く、真ん中は明るい水色。98 のあの流れる帯 */
        double t = (double)i / blk;
        double f = 1.0 - (t < 0.5 ? (0.5 - t) : (t - 0.5)) * 2.0;
        int r = (int)(0x20 * f), g = (int)(0x90 * f), b = (int)(0xF0 * f);
        for (int j = 0; j < bh; j++) cv_px(c, x, by + j, r, g, b);
    }
    *out_y0 = y0;
    *out_y1 = y1;
}

int main(int argc, char **argv)
{
    int test_only = (argc > 1 && !strcmp(argv[1], "--test"));

    /* 見た目を直すのに毎回起動していられないので、画面を使わずに
     * そのまま PPM に吐ける口を付けておく。
     *   myos-splash --dump 1280x800 5 out.ppm     (5 は帯の位置) */
    if (argc > 4 && !strcmp(argv[1], "--dump")) {
        int w = 0, h = 0;
        if (sscanf(argv[2], "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) return 2;
        font_init();
        Canvas c = { 0, 0, NULL };
        if (cv_alloc(&c, w, h) < 0) return 1;
        compose_static(&c);
        {
            int bx, by, bw, bh;
            bar_geom(w, h, &bx, &by, &bw, &bh);
            int y0 = by - 1 < 0 ? 0 : by - 1;
            int y1 = by + bh + 1 > h ? h : by + bh + 1;
            uint8_t *clean = malloc((size_t)(y1 - y0) * w * 3);
            if (!clean) return 1;
            memcpy(clean, c.rgb + (size_t)y0 * w * 3,
                   (size_t)(y1 - y0) * w * 3);
            int a, b2;
            compose_bar(&c, clean, atoi(argv[3]), &a, &b2);
            free(clean);
        }
        FILE *fp = fopen(argv[4], "wb");
        if (!fp) return 1;
        fprintf(fp, "P6\n%d %d\n255\n", w, h);
        fwrite(c.rgb, 1, (size_t)w * h * 3, fp);
        fclose(fp);
        cv_free(&c);
        return 0;
    }

    /* 一枚だけ描いて抜ける。電源を切るときに使う。
     * 動く帯は出さない。もう進んでいるものが無いので、動かすと嘘になる。 */
    if (argc > 1 && !strcmp(argv[1], "--once")) {
        if (argc > 2) sub_text = argv[2];
        show_bar = 0;
        FB f1;
        if (fb_open(&f1) < 0) return 1;
        struct fb_var_screeninfo av = f1.v;
        av.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
        av.pixclock = 0;
        ioctl(f1.fd, FBIOPUT_VSCREENINFO, &av);
        poke_console();
        font_init();
        Canvas c = { 0, 0, NULL };
        if (cv_alloc(&c, (int)f1.v.xres, (int)f1.v.yres) < 0) {
            fb_close(&f1); return 1;
        }
        compose_static(&c);
        uint8_t *b = NULL; size_t bl = 0;
        cv_blit(&c, &f1, &b, &bl, 0, (int)f1.v.yres);
        free(b);
        cv_free(&c);
        fb_close(&f1);
        return 0;
    }

    FB fb;
    if (test_only) {
        if (fb_open(&fb) < 0) return 1;
        fb_close(&fb);
        return 0;
    }

    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);
    signal(SIGHUP,  SIG_IGN);

    font_init();

    Canvas cv = { 0, 0, NULL };
    int cw = 0, ch = 0;
    uint8_t *line = NULL;
    size_t linelen = 0;
    uint8_t *clean = NULL;      /* 帯の行の、何も乗っていない状態 */
    int fresh = 0;              /* 画面が変わった直後は全面を描く */

    for (int frame = 0; !stop_now; frame++) {
        if (fb_open(&fb) == 0) {
            if ((int)fb.v.xres != cw || (int)fb.v.yres != ch) {
                cw = (int)fb.v.xres; ch = (int)fb.v.yres;
                /* 画面を実際に出させる。
                 *
                 * /dev/fb0 が出来ていても、その中身が走査されているとは
                 * 限らない。実際、bochs-drm が 1280x800 の fbdev を作った
                 * あとも、画面は前の 1024x768 のまま数秒間そこにいた。
                 * 書き込みは成功しているのに何も見えない、という形になる。
                 *
                 * FBIOPUT_VSCREENINFO を今と同じ値で投げると fb_set_par が
                 * 走り、DRM 側がモードを入れ直してこのバッファを出す。
                 * 解像度が変わったときだけ叩く。毎回やるとその都度
                 * モード設定が走ってちらつく。 */
                struct fb_var_screeninfo av = fb.v;
                av.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
                av.pixclock = 0;
                ioctl(fb.fd, FBIOPUT_VSCREENINFO, &av);
                poke_console();
                /* 画面が挿げ替わるたびに 1 行だけ残す。実機で
                 * 「起動画面が出ない」を追うとき、これが無いと
                 * どの画面に書いていたのかが分からない。 */
                fprintf(stderr, "myos-splash: %s %ux%u %ubpp stride=%u (%s)\n",
                        FBDEV, fb.v.xres, fb.v.yres, fb.v.bits_per_pixel,
                        fb.f.line_length, fb.f.id);
                if (cv_alloc(&cv, cw, ch) < 0) { fb_close(&fb); break; }

                compose_static(&cv);

                /* 帯の行を、動くものが乗る前に取っておく。 */
                int bx, by, bw, bh;
                bar_geom(cw, ch, &bx, &by, &bw, &bh);
                int y0 = by - 1 < 0 ? 0 : by - 1;
                int y1 = by + bh + 1 > ch ? ch : by + bh + 1;
                free(clean);
                clean = malloc((size_t)(y1 - y0) * cw * 3);
                if (!clean) { fb_close(&fb); break; }
                memcpy(clean, cv.rgb + (size_t)y0 * cw * 3,
                       (size_t)(y1 - y0) * cw * 3);
                fresh = 1;
            }

            int y0, y1;
            compose_bar(&cv, clean, frame, &y0, &y1);
            if (fresh) {
                /* 画面が挿げ替わった直後だけ全面。以降は帯の行だけ。 */
                cv_blit(&cv, &fb, &line, &linelen, 0, ch);
                fresh = 0;
            } else {
                cv_blit(&cv, &fb, &line, &linelen, y0, y1);
            }
            fb_close(&fb);
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    free(line);
    free(clean);
    cv_free(&cv);
    return 0;
}

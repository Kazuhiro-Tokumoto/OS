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
#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "x98.h"

/* 日本語入力の入力文脈。IME が上がっていなければ NULL のまま。 */
static XIC ic;
#include "filetypes.h"
#include <X11/keysym.h>

#define WIN_W 620
#define WIN_H 526

#define PREV_X   12
#define TAB_H    26
#define PREV_Y   (12 + TAB_H)
#define PREV_W   (WIN_W - 24)
#define PREV_H   150

#define LIST_X   12
#define LIST_Y   (196 + TAB_H)
#define LIST_W   270
#define LIST_H   116
#define ROW_H    18

#define PAL_X    300
#define PAL_Y    (196 + TAB_H)
#define PAL_CELL 26
#define PAL_COLS 8
#define PAL_ROWS 2

#define SPD_Y    (334 + TAB_H)
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
    /* 書き先はユーザーの ~/.myos。/etc/myos は root のものなので触らない。
     * 配色を変えるだけで管理者権限を聞かれるのは筋が悪い。 */
    char path[512];
    myos_user_conf(path, sizeof(path), X98_THEME_CONF);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[myos-settings] cannot write %s\n", path);
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

/* --- タブ ---------------------------------------------------------------- */
enum { TAB_LOOK = 0, TAB_BG, TAB_TYPES, TAB_DISPLAY, TAB_SOUND, TAB_SYSTEM,
       TAB_UPDATE, N_TABS };
static const char *tab_name[N_TABS] = {
    "Appearance", "Background", "File Types", "Display", "Sound", "System",
    "Update"
};
static int tab = TAB_LOOK;

/* --- 音の出口 ------------------------------------------------------------
 *
 * 一覧の取得も切り替えも myos-audio に任せる。ここで pactl を直接
 * 叩かないのは、出口の見つけ方 (プロファイルが off の HDMI をどう
 * 拾うか、繋がっていないものをどう落とすか) が地味に込み入っていて、
 * 画面の都合と混ぜたくないから。あちらは単体で試せる。
 */
#define MAX_SNK 14
#define SNK_ROW 22
static struct {
    char id[192];
    char label[160];
    int  active;          /* いまここから出ている */
    int  unplugged;       /* available: no。逃げ道で出しているもの */
} snd[MAX_SNK];
static int n_snd = 0;
static int snd_all = 0;       /* 1 = 挿さっていないものも並べる */
static int snd_sel = -1;
static int snd_loaded = 0;
static char snd_msg[160] = "";

static void snd_load(void)
{
    n_snd = 0;
    snd_sel = -1;
    snd_msg[0] = 0;

    FILE *f = popen(snd_all
                    ? "/usr/local/bin/myos-audio list --all 2>/dev/null"
                    : "/usr/local/bin/myos-audio list 2>/dev/null", "r");
    if (!f) {
        snprintf(snd_msg, sizeof(snd_msg), "Could not ask the sound system.");
        snd_loaded = 1;
        return;
    }
    char line[512];
    while (n_snd < MAX_SNK && fgets(line, sizeof(line), f)) {
        /* 形式: <*|-> TAB <id> TAB <label> TAB <avail> */
        char *p1 = strchr(line, '\t');
        if (!p1) continue;
        *p1++ = 0;
        char *p2 = strchr(p1, '\t');
        if (!p2) continue;
        *p2++ = 0;
        char *p3 = strchr(p2, '\t');
        if (p3) *p3++ = 0;
        char *nl = strchr(p3 ? p3 : p2, '\n');
        if (nl) *nl = 0;

        snprintf(snd[n_snd].id, sizeof(snd[n_snd].id), "%s", p1);
        snprintf(snd[n_snd].label, sizeof(snd[n_snd].label), "%s", p2);
        snd[n_snd].active = (line[0] == '*');
        snd[n_snd].unplugged = (p3 && !strcmp(p3, "no"));
        if (snd[n_snd].active) snd_sel = n_snd;
        n_snd++;
    }
    /* 入り切らなかったことを黙らない。実機で 12 個ちょうど並んで、
     * それが「全部」なのか「打ち切られた」のか画面から読めなかった。
     * 隠した事実は必ず言う。 */
    int over = (n_snd >= MAX_SNK && fgets(line, sizeof(line), f)) ? 1 : 0;
    pclose(f);

    if (over)
        snprintf(snd_msg, sizeof(snd_msg),
                 "Only the first %d outputs are shown.", MAX_SNK);

    if (n_snd == 0)
        snprintf(snd_msg, sizeof(snd_msg),
                 snd_all ? "No sound hardware was found."
                         : "Nothing detected. Try \"Show all outputs\".");
    else if (snd_sel < 0)
        snd_sel = 0;
    snd_loaded = 1;
}

static void snd_apply(void)
{
    if (snd_sel < 0 || snd_sel >= n_snd) return;
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
             "/usr/local/bin/myos-audio set '%s' >/dev/null 2>&1",
             snd[snd_sel].id);
    /* このプログラムは SIGCHLD を SIG_IGN にしている。そのままだと
     * system() の中の waitpid が ECHILD で失敗し、成功していても
     * 必ず -1 が返る。ここだけ既定に戻して呼ぶ (他の場所と同じ作法)。 */
    void (*old_chld)(int) = signal(SIGCHLD, SIG_DFL);
    int rc = system(cmd);
    signal(SIGCHLD, old_chld);

    if (rc != 0) {
        /* 「切り替えられません」だけでは、何をすればいいのか分からない。
         * 繋がっていないと出ているものを選んだのなら、理由はほぼそれ。
         *
         * HDMI は特に紛らわしい。音の出口は画面のカードの中にあるが、
         * 「画面が繋がっているか」は画面のドライバ (nouveau / i915 /
         * amdgpu) が教える。セーフグラフィックスで起動していると
         * ドライバが載っていないので、ケーブルを挿していても
         * 「繋がっていない」と見える。 */
        if (snd[snd_sel].unplugged)
            snprintf(snd_msg, sizeof(snd_msg),
                     "Not connected. HDMI also needs the graphics driver.");
        else
            snprintf(snd_msg, sizeof(snd_msg),
                     "Could not switch to that output.");
    } else {
        snd_msg[0] = 0;
    }
}

/* --- ファイルの種類 ------------------------------------------------------ */
#define FT_LIST_X 12
#define FT_LIST_Y (TAB_H + 26)   /* タブの帯と「〜の一覧」の見出しのぶん空ける */
#define FT_LIST_W 380
/* FT_LIST_H は freetype の <freetype/ftlist.h> を指す名前と衝突する。
 * 再定義の警告が出て、本物の警告が埋もれる。名前を変える。 */
#define FT_LIST_HEIGHT 240
#define FT_ROW_H  18

static FileType ftypes[FT_MAX];
static int      n_ftypes = 0;
static int      ft_sel = 0, ft_top = 0;
static X98Edit  ed_open, ed_run;
static int      ft_field = 0;       /* 0 = 開く / 1 = 実行 */
static int      ft_dirty = 0;

static void ft_pick(int i)
{
    if (i < 0 || i >= n_ftypes) return;
    ft_sel = i;
    x98_edit_set(&ed_open, ftypes[i].open);
    x98_edit_set(&ed_run,  ftypes[i].run);
}

/* 編集中の内容を表へ戻す */
static void ft_commit(void)
{
    if (ft_sel < 0 || ft_sel >= n_ftypes) return;
    snprintf(ftypes[ft_sel].open, FT_CMD, "%s", ed_open.buf);
    snprintf(ftypes[ft_sel].run,  FT_CMD, "%s", ed_run.buf);
}

static void save_filetypes(void)
{
    ft_commit();

    char path[512];
    myos_user_conf(path, sizeof(path), "filetypes.conf");

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[myos-settings] cannot write %s\n", path);
        return;
    }
    fprintf(f, "# myOS: ファイルの関連付け (myos-settings が書き出す)\n");
    fprintf(f, "# 拡張子|説明|アイコン|開く|実行\n");
    fprintf(f, "# %%1 がファイルのパスに置き換わる。\n");
    for (int i = 0; i < n_ftypes; i++)
        fprintf(f, "%s|%s|%s|%s|%s\n", ftypes[i].exts, ftypes[i].desc,
                ftypes[i].icon, ftypes[i].open, ftypes[i].run);
    fclose(f);
    ft_dirty = 0;
}

/* --- システム ------------------------------------------------------------ */
static const struct { const char *label; const char *val; } heaps[] = {
    { "Automatic (1/4 of RAM)", "auto" },
    { "256 MB",  "256" },
    { "512 MB",  "512" },
    { "1024 MB", "1024" },
    { "2048 MB", "2048" },
    { "4096 MB", "4096" },
};
#define N_HEAPS ((int)(sizeof(heaps) / sizeof(heaps[0])))

/* 2 列に並べる。縦一列のままだと、選択肢を増やしたぶんだけ下の
 * 「Hardware」と「System settings」が押し出されて、窓の底
 * (BTN_Y) にぶつかる。2 列なら増やしても背は縮む。 */
#define HEAP_COLS  2
#define HEAP_ROWS  ((N_HEAPS + HEAP_COLS - 1) / HEAP_COLS)
#define HEAP_COL_W 200
#define HEAP_ROW_H 22
static int heap_sel = 0;

static void load_java(void)
{
    char path[512];
    myos_conf(path, sizeof(path), "java.conf");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = myos_trim(line);
        if (strncmp(s, "heap_mb", 7)) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        char *v = myos_trim(eq + 1);
        for (int i = 0; i < N_HEAPS; i++)
            if (!strcmp(v, heaps[i].val)) { heap_sel = i; break; }
        break;
    }
    fclose(f);
}

static void save_java(void)
{
    char path[512];
    myos_user_conf(path, sizeof(path), "java.conf");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# myOS: Java の走らせ方 (myos-settings が書き出す)\n");
    fprintf(f, "heap_mb = %s\n", heaps[heap_sel].val);
    fclose(f);
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

/* OK / Apply で 3 つのタブぶんまとめて書く。
 * どのタブを見ていたかで保存されるものが変わる、というのは分かりにくい。 */
static void save_all(void)
{
    save_theme();
    save_filetypes();
    save_java();
}

static void draw_buttons(void)
{
    int bx = WIN_W - (BTN_W + 8) * 3 - 4;
    x98_button(&x98, win, bx, BTN_Y, BTN_W, BTN_H, "OK", 0);
    x98_button(&x98, win, bx + BTN_W + 8, BTN_Y, BTN_W, BTN_H, "Apply", 0);
    x98_button(&x98, win, bx + (BTN_W + 8) * 2, BTN_Y, BTN_W, BTN_H,
               "Cancel", 0);
}

static void draw_tabs(void)
{
    int x = 8;
    for (int i = 0; i < N_TABS; i++) {
        int w = x98_text_w(&x98, tab_name[i]) + 24;
        int y = (i == tab) ? 6 : 9;
        int h = (i == tab) ? TAB_H - 2 : TAB_H - 5;
        x98_fill(&x98, win, x, y, w, h, x98.face);
        /* 選んでいるものだけ下線を消して、中身と地続きに見せる */
        x98_hline(&x98, win, x, y, w, x98.light);
        x98_vline(&x98, win, x, y, h, x98.light);
        x98_vline(&x98, win, x + w - 1, y, h, x98.shadow);
        x98_text(&x98, win, x + 12, y + (h - x98_text_h(&x98)) / 2,
                 tab_name[i], x98.text);
        x += w + 2;
    }
    x98_hline(&x98, win, 8, TAB_H + 4, WIN_W - 16, x98.light);
}

static void edit_row(int x, int y, int w, const char *lab, X98Edit *e, int focus)
{
    x98_text(&x98, win, x, y + 4, lab, x98.text);
    x98_edit_draw(&x98, win, x + 90, y, w, 20, e, focus);
}

static void draw_types(void)
{
    x98_text(&x98, win, FT_LIST_X, FT_LIST_Y - 16,
             "Registered file types:", x98.text);

    x98_bevel(&x98, win, FT_LIST_X, FT_LIST_Y, FT_LIST_W, FT_LIST_HEIGHT, 0);
    x98_fill(&x98, win, FT_LIST_X + 2, FT_LIST_Y + 2,
             FT_LIST_W - 4, FT_LIST_HEIGHT - 4, x98.white);

    int vis = (FT_LIST_HEIGHT - 4) / FT_ROW_H;
    for (int r = 0; r < vis; r++) {
        int i = ft_top + r;
        if (i >= n_ftypes) break;
        int ry = FT_LIST_Y + 2 + r * FT_ROW_H;
        unsigned long fg = x98.text;
        if (i == ft_sel) {
            x98_fill(&x98, win, FT_LIST_X + 2, ry, FT_LIST_W - 4, FT_ROW_H,
                     x98.select_bg);
            fg = x98.white;
        }
        x98_text(&x98, win, FT_LIST_X + 8,
                 ry + (FT_ROW_H - x98_text_h(&x98)) / 2, ftypes[i].desc, fg);
    }

    /* 選んでいる種類の中身 */
    int dy = FT_LIST_Y + FT_LIST_HEIGHT + 16;
    if (ft_sel >= 0 && ft_sel < n_ftypes) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Extensions:  %s", ftypes[ft_sel].exts);
        x98_text(&x98, win, FT_LIST_X, dy, buf, x98.text);
    }
    edit_row(FT_LIST_X, dy + 22, WIN_W - FT_LIST_X - 100, "Opens with:",
             &ed_open, ft_field == 0);
    edit_row(FT_LIST_X, dy + 50, WIN_W - FT_LIST_X - 100, "Runs with:",
             &ed_run, ft_field == 1);

    x98_text(&x98, win, FT_LIST_X, dy + 80,
             "%1 is replaced with the file. Tab switches boxes.", x98.shadow);
    x98_text(&x98, win, FT_LIST_X, dy + 96,
             "\"Runs with\" is the second verb in the right-click menu.",
             x98.shadow);
}

/* --- 画面 ---------------------------------------------------------------- */
/* 解像度は X からは変えられない。nomodeset で DRM に触らせず、
 * ブートローダーが VBE で決めたフレームバッファを使い続ける作りなので、
 * 「次に起動するときの希望」をディスクに書いて再起動する形になる。
 * (Windows 98 も色深度の変更に再起動が要った) */
static const struct { int w, h; } resolutions[] = {
    {  640,  480 }, {  800,  600 }, { 1024,  768 }, { 1152,  864 },
    { 1280,  720 }, { 1280, 1024 }, { 1440,  900 }, { 1600,  900 },
    { 1680, 1050 }, { 1920, 1080 },
};
#define N_RES ((int)(sizeof(resolutions) / sizeof(resolutions[0])))

static int res_sel = -1;            /* -1 = おまかせ */
/* GPU のドライバを使わずに起動する (nomodeset)。
 * 起動中に S を押すのと同じだが、こちらは覚える。画面が出ない機械で
 * 毎回押させるのは、逃げ道として成立していない。 */
static int res_safe = 0;
static char res_now[64] = "";       /* いま書かれている値 */
static char res_msg[128] = "";      /* 直近の結果 */

#define RES_Y (TAB_H + 92)
#define RES_ROW 20

/* いま何が選ばれているかを読む。
 *
 * 本物はペイロードテーブルだが、**あれを読むには root が要る** (生の
 * ディスクを開くので当然)。この画面は一般ユーザーで動くので覗けない。
 * そのせいで実機では「Currently set to: auto」しか出ず、選び直した
 * 直後もラジオが「おまかせ」に戻っていた。選んだのに選んだことに
 * なっていない画面は、使う人には壊れて見える。
 *
 * 読むためだけにパスワードを聞くのは筋が悪いので、myos-setres が
 * 置く控え (/etc/myos/display.conf) を見る。無ければ myos-setres を
 * 呼ぶ (root で動かしているときはこちらが通る)。 */
static void res_set_from(int w, int h)
{
    snprintf(res_now, sizeof(res_now), "%dx%d", w, h);
    for (int i = 0; i < N_RES; i++)
        if (resolutions[i].w == w && resolutions[i].h == h) {
            res_sel = i;
            return;
        }
}

static void res_load(void)
{
    res_sel = -1;
    res_safe = 0;
    snprintf(res_now, sizeof(res_now), "auto");

    FILE *c = fopen("/etc/myos/display.conf", "r");
    if (c) {
        char line[128];
        while (fgets(line, sizeof(line), c)) {
            int w, h;
            char *v = strchr(line, '=');
            if (!v) continue;
            if (!strncmp(line, "safe", 4)) {
                res_safe = (strstr(v, "yes") != NULL);
                continue;
            }
            if (sscanf(v + 1, " %dx%d", &w, &h) == 2)
                res_set_from(w, h);
            /* mode = auto はそのまま (既定が -1)。
             * safe の行が後ろにあるので、ここでは読み終えない。 */
        }
        fclose(c);
        return;
    }

    FILE *f = popen("myos-setres 2>/dev/null", "r");
    if (!f) return;
    char line[128];
    if (fgets(line, sizeof(line), f)) {
        int w, h;
        if (sscanf(line, "current: %dx%d", &w, &h) == 2)
            res_set_from(w, h);
    }
    pclose(f);
}

static void res_apply(void)
{
    char cmd[256];
    if (res_sel < 0)
        snprintf(cmd, sizeof(cmd), "myos-runas myos-setres auto");
    else
        snprintf(cmd, sizeof(cmd), "myos-runas myos-setres %d %d",
                 resolutions[res_sel].w, resolutions[res_sel].h);

    /* このプログラムは SIGCHLD を SIG_IGN にしている (子を放置しても
     * ゾンビにしないため)。ところがその状態だと system() は子を
     * 待てず、成功しても -1 を返す。ここだけ既定に戻して呼ぶ。
     * 戻さないと「保存できたのに失敗と出る」ことになる。 */
    void (*old_chld)(int) = signal(SIGCHLD, SIG_DFL);
    int rc = system(cmd);
    signal(SIGCHLD, old_chld);

    if (rc == 0) {
        snprintf(res_msg, sizeof(res_msg),
                 "Saved. The new size is used the next time you start myOS.");
        res_load();
    } else {
        snprintf(res_msg, sizeof(res_msg),
                 "Could not save. Administrator rights are needed.");
    }
}

static void res_safe_apply(void)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "myos-runas myos-setres safe %s",
             res_safe ? "on" : "off");
    void (*old_chld)(int) = signal(SIGCHLD, SIG_DFL);
    int rc = system(cmd);
    signal(SIGCHLD, old_chld);

    if (rc == 0) {
        snprintf(res_msg, sizeof(res_msg),
                 "Saved. It takes effect the next time you start myOS.");
        res_load();
    } else {
        res_safe = !res_safe;          /* 効かなかったので戻す */
        snprintf(res_msg, sizeof(res_msg),
                 "Could not save. Administrator rights are needed.");
    }
}

/* --- 背景 (壁紙) ---------------------------------------------------------
 * 98 の「画面のプロパティ」の壁紙にあたる。ファイル選択の窓は作らず、
 * 絵の置き場を覗いて一覧にする。98 も Windows フォルダの中を並べて
 * いただけなので、これで同じ使い勝手になる。
 *
 * 書き出し先は ~/.myos/desktop.conf。読むのはウィンドウマネージャで、
 * 保存したあと SIGUSR1 を送れば描き直してくれる。 */
#define BG_MAX   64
#define BG_Y     (TAB_H + 44)
#define BG_ROW   18
#define BG_VIS   9

static char bg_files[BG_MAX][256];
static char bg_shown[BG_MAX][80];
static int  bg_n = 0, bg_sel = 0, bg_top = 0;

static const char *bg_modes[] = { "center", "tile", "stretch", "fit" };
static const char *bg_mode_label[] = {
    "Center", "Tile", "Stretch", "Fit to screen"
};
#define BG_NMODE 4
static int bg_mode = 0;
static char bg_msg[128] = "";

static int bg_is_image(const char *n)
{
    const char *d = strrchr(n, '.');
    if (!d) return 0;
    d++;
    static const char *ok[] = { "png","jpg","jpeg","gif","bmp","webp",
                                "ppm","tif","tiff", NULL };
    for (int i = 0; ok[i]; i++) {
        size_t L = strlen(ok[i]);
        if (strlen(d) == L) {
            size_t j = 0;
            for (; j < L; j++)
                if (tolower((unsigned char)d[j]) != ok[i][j]) break;
            if (j == L) return 1;
        }
    }
    return 0;
}

static void bg_add(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && bg_n < BG_MAX) {
        if (e->d_name[0] == '.') continue;
        if (!bg_is_image(e->d_name)) continue;
        snprintf(bg_files[bg_n], sizeof(bg_files[bg_n]), "%s/%s",
                 dir, e->d_name);
        snprintf(bg_shown[bg_n], sizeof(bg_shown[bg_n]), "%s", e->d_name);
        bg_n++;
    }
    closedir(d);
}

static void bg_load(void)
{
    const char *home = myos_home();
    char p1[300];

    bg_n = 0;
    /* 先頭は「使わない」。単色に戻す道を必ず残す。 */
    bg_files[bg_n][0] = 0;
    snprintf(bg_shown[bg_n], sizeof(bg_shown[bg_n]), "(None)");
    bg_n++;

    bg_add("/usr/share/myos/wallpapers");
    snprintf(p1, sizeof(p1), "%s/Pictures", home); bg_add(p1);
    snprintf(p1, sizeof(p1), "%s/My Documents", home); bg_add(p1);
    bg_add(home);

    /* いまの設定を読んで、選択位置と並べ方に反映する。 */
    char conf[512], cur[256] = "";
    myos_conf(conf, sizeof(conf), "wallpaper.conf");
    if (access(conf, R_OK) != 0)      /* 1.5.1 までの置き場所 */
        myos_conf(conf, sizeof(conf), "desktop.conf");
    FILE *f = fopen(conf, "r");
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            char *v = strchr(line, '=');
            if (!v) continue;
            *v++ = 0;
            char *k = myos_trim(line);
            v = myos_trim(v);
            if (!strcmp(k, "wallpaper")) snprintf(cur, sizeof(cur), "%s", v);
            else if (!strcmp(k, "wallmode"))
                for (int i = 0; i < BG_NMODE; i++)
                    if (!strcmp(v, bg_modes[i])) bg_mode = i;
        }
        fclose(f);
    }
    bg_sel = 0;
    for (int i = 0; i < bg_n; i++)
        if (cur[0] && !strcmp(bg_files[i], cur)) { bg_sel = i; break; }
    if (bg_sel >= bg_top + BG_VIS) bg_top = bg_sel - BG_VIS + 1;
}

static void bg_apply(void)
{
    char conf[512];
    /* 壁紙だけの専用ファイル。desktop.conf に書くと、そこに並んでいる
     * デスクトップのアイコンを消してしまう (このファイルは丸ごと
     * 書き直すため)。書く人ごとに分ける。 */
    myos_user_conf(conf, sizeof(conf), "wallpaper.conf");
    FILE *f = fopen(conf, "w");
    if (!f) {
        snprintf(bg_msg, sizeof(bg_msg), "Could not save the setting.");
        return;
    }
    fprintf(f, "# myOS: デスクトップの背景\n");
    fprintf(f, "wallpaper = %s\n", bg_files[bg_sel]);
    fprintf(f, "wallmode = %s\n", bg_modes[bg_mode]);
    fclose(f);

    /* 描き直させる。設定を書いただけでは画面は変わらない。 */
    void (*old_chld)(int) = signal(SIGCHLD, SIG_DFL);
    int rc = system("pkill -USR1 -x myos-wm");
    signal(SIGCHLD, old_chld);
    (void)rc;

    snprintf(bg_msg, sizeof(bg_msg), bg_files[bg_sel][0]
             ? "Applied." : "The background is back to a plain colour.");
}

static void draw_bg(void)
{
    x98_text(&x98, win, 16, TAB_H + 20, "Wallpaper", x98.text);

    int lx = 16, lw = WIN_W - 32, lh = BG_VIS * BG_ROW + 4;
    x98_bevel(&x98, win, lx, BG_Y, lw, lh, 0);
    x98_fill(&x98, win, lx + 2, BG_Y + 2, lw - 4, lh - 4, x98.white);

    for (int i = 0; i < BG_VIS && bg_top + i < bg_n; i++) {
        int idx = bg_top + i, y = BG_Y + 2 + i * BG_ROW;
        int on = (idx == bg_sel);
        if (on) x98_fill(&x98, win, lx + 2, y, lw - 4, BG_ROW, x98.select_bg);
        x98_text(&x98, win, lx + 8, y + (BG_ROW - x98_text_h(&x98)) / 2,
                 bg_shown[idx], on ? x98.white : x98.text);
    }

    int my = BG_Y + lh + 12;
    x98_text(&x98, win, 16, my, "Display it", x98.text);
    for (int i = 0; i < BG_NMODE; i++) {
        int rx = 32 + i * 150, ry = my + 20;
        x98_bevel(&x98, win, rx, ry, 13, 13, 0);
        x98_fill(&x98, win, rx + 2, ry + 2, 9, 9, x98.white);
        if (i == bg_mode) x98_fill(&x98, win, rx + 4, ry + 4, 5, 5, x98.text);
        x98_text(&x98, win, rx + 20, ry + (13 - x98_text_h(&x98)) / 2 - 1,
                 bg_mode_label[i], x98.text);
    }

    int by = my + 50;
    x98_button(&x98, win, 32, by, 90, 22, "Apply", 0);
    x98_text(&x98, win, 132, by + 5,
             bg_msg[0] ? bg_msg
                       : "Pictures in your home folder are listed here.",
             x98.shadow);
}

static void draw_display(void)
{
    char buf[160];
    int y = TAB_H + 20;

    x98_text(&x98, win, 16, y, "Screen area", x98.text);
    snprintf(buf, sizeof(buf), "    Currently set to:  %s", res_now);
    x98_text(&x98, win, 16, y + 22, buf, x98.text);
    x98_text(&x98, win, 16, y + 40,
             "    A restart is needed before the new size is used.",
             x98.shadow);

    /* おまかせ */
    int ry = RES_Y;
    x98_bevel(&x98, win, 32, ry, 13, 13, 0);
    x98_fill(&x98, win, 34, ry + 2, 9, 9, x98.white);
    if (res_sel < 0) x98_fill(&x98, win, 36, ry + 4, 5, 5, x98.text);
    x98_text(&x98, win, 54, ry + (13 - x98_text_h(&x98)) / 2 - 1,
             "Let myOS choose (recommended)", x98.text);

    for (int i = 0; i < N_RES; i++) {
        ry = RES_Y + (i + 1) * RES_ROW;
        x98_bevel(&x98, win, 32, ry, 13, 13, 0);
        x98_fill(&x98, win, 34, ry + 2, 9, 9, x98.white);
        if (i == res_sel) x98_fill(&x98, win, 36, ry + 4, 5, 5, x98.text);
        snprintf(buf, sizeof(buf), "%d by %d pixels",
                 resolutions[i].w, resolutions[i].h);
        x98_text(&x98, win, 54, ry + (13 - x98_text_h(&x98)) / 2 - 1,
                 buf, x98.text);
    }

    ry = RES_Y + (N_RES + 1) * RES_ROW + 10;
    x98_button(&x98, win, 32, ry, 90, 22, "Apply", 0);

    if (res_msg[0])
        x98_text(&x98, win, 132, ry + 5, res_msg, x98.shadow);
    else
        x98_text(&x98, win, 132, ry + 5,
                 "If the screen cannot show the size you pick,",
                 x98.shadow);
    if (!res_msg[0])
        x98_text(&x98, win, 132, ry + 20,
                 "myOS falls back to one that works.", x98.shadow);

    /* セーフグラフィックス。
     *
     * 起動中に S を押すのと同じことを、覚えさせる。画面が出ない機械で
     * **毎回押させるのは逃げ道として成立していない**。ここで入れて
     * おけば、次からは押さなくていい。 */
    int sy = ry + 44;
    x98_text(&x98, win, 16, sy, "If the screen stays black", x98.text);

    int cy = sy + 22;
    x98_bevel(&x98, win, 32, cy, 13, 13, 0);
    x98_fill(&x98, win, 34, cy + 2, 9, 9, x98.white);
    if (res_safe) {
        XSetForeground(dpy, x98.gc, x98.text);
        XDrawLine(dpy, win, x98.gc, 35, cy + 6, 37, cy + 9);
        XDrawLine(dpy, win, x98.gc, 37, cy + 9, 42, cy + 3);
    }
    x98_text(&x98, win, 54, cy + (13 - x98_text_h(&x98)) / 2 - 1,
             "Do not use the graphics driver (safe graphics)", x98.text);
    x98_text(&x98, win, 54, cy + 20,
             "    Same as holding S while starting, but remembered.",
             x98.shadow);
    x98_text(&x98, win, 54, cy + 36,
             "    The picture always appears, but 3D becomes slow.",
             x98.shadow);
}

/* --- 更新 ----------------------------------------------------------------
 * カーネル・Debian のセキュリティ更新・アプリを 1 箇所にまとめる。
 * どれも「網に繋がっていないと何も出来ない」点は同じなので、
 * 別々の画面に散らすより、ここに並べたほうが分かりやすい。
 *
 * カーネルの入れ替えだけは危ないので、何が起きるかを先に書いておく。
 * 使っていないほうの置き場へ書いてからテーブルを差し替えるので、
 * 途中で電源が落ちても古いほうで起動する。新しいカーネルが動かなかった
 * ときは、起動中に R を押しっぱなしにすると 1 つ前で立つ。
 */
/* 3 つを別々に持つ。ひとつにまとめると、また嘘をつく。
 *
 *   up_os      入れたときの ISO の版。カーネルを入れ替えても変わらない
 *   up_kernel  いま生領域に乗っているカーネルの版
 *   up_latest  出ている版
 *
 * 押せるかどうかは up_new (myos-update の kernelnew) だけで決める。
 * 版の数字を見比べて決めてはいけない。z を上げた版はカーネルを
 * 作り直していないので、数字は進んでいても中身は同じ。押せてしまうと
 * 21MB 落として生領域を書き換えて、何も変わらないことになる。 */
static char up_os[64]        = "";
static char up_kernel[64]    = "";
static char up_latest[64]    = "";
static char up_msg[200]      = "";
static int  up_checked       = 0;
static int  up_busy          = 0;
static int  up_new           = 0;   /* 1 = カーネルを入れ替える価値がある */
static int  up_ab            = 0;   /* 1 = ルートが A/B。入れ直さずに済む */
static int  up_sysnew        = 0;   /* 1 = システム側が古い */

#define UP_BTN_W 132
#define UP_BTN_H 24

/* 押せる釦の並びを 1 箇所で決める。描くところと当たり判定で別々に
 * 数えると、片方だけ直したときにずれる (Add Programs でやった)。
 * 0 番は Check for updates で、いつでも居る。 */
#define UP_BTN_X(i) (32 + (i) * (UP_BTN_W + 10))
static void up_slots(int *kern, int *sys)
{
    int i = 1;
    *kern = (up_checked && up_new)              ? i++ : -1;
    *sys  = (up_checked && up_sysnew && up_ab)  ? i++ : -1;
}

static void up_check(void)
{
    up_busy = 1;
    snprintf(up_msg, sizeof(up_msg), "Checking...");

    FILE *f = popen("sudo -n /usr/local/bin/myos-update status 2>/dev/null", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = myos_trim(line), *v = myos_trim(eq + 1);
            if (!strcmp(k, "os"))
                snprintf(up_os, sizeof(up_os), "%s", v);
            else if (!strcmp(k, "kernel"))
                snprintf(up_kernel, sizeof(up_kernel), "%s", v);
            else if (!strcmp(k, "latest"))
                snprintf(up_latest, sizeof(up_latest), "%s", v);
            else if (!strcmp(k, "kernelnew"))
                up_new = !strcmp(v, "yes");
            else if (!strcmp(k, "system"))
                /* "A/B (/dev/sda3 / /dev/sda5、いま ...)" か "1 本 (...)"。
                 * 頭の 3 文字だけ見る。中身の書き方はいつ変わってもいい。 */
                up_ab = !strncmp(v, "A/B", 3);
        }
        pclose(f);
    }
    up_checked = 1;
    up_busy = 0;

    up_sysnew = up_latest[0] && !strchr(up_latest, '(') &&
                strcmp(up_os, up_latest);

    if (!up_latest[0] || strchr(up_latest, '('))
        snprintf(up_msg, sizeof(up_msg),
                 "Could not reach the internet. Connect first.");
    else if (up_new && up_sysnew && up_ab)
        snprintf(up_msg, sizeof(up_msg),
                 "%s is available. Update the system, then the kernel.",
                 up_latest);
    else if (up_new)
        snprintf(up_msg, sizeof(up_msg),
                 "A newer kernel is available (%s).", up_latest);
    else if (up_sysnew && up_ab)
        /* ルートが A/B なら入れ直さなくてよい。使っていないほうへ
         * 入れて、起動先を切り替えるだけで済む。 */
        snprintf(up_msg, sizeof(up_msg),
                 "%s is available. It goes to the spare root.", up_latest);
    else if (up_sysnew)
        /* ルートが 1 本の機械。入れ替える先が無いので入れ直すしかない。
         * ここで黙って「最新です」と出すと、押しても何も起きない理由が
         * 分からない。何が要るのかを書く。 */
        snprintf(up_msg, sizeof(up_msg),
                 "Kernel is current. %s changes need a reinstall.",
                 up_latest);
    else
        snprintf(up_msg, sizeof(up_msg), "myOS is up to date.");
}

static void draw_update(void)
{
    int y = TAB_H + 20;

    x98_text(&x98, win, 16, y, "myOS", x98.text);
    char b[200];
    /* 「入っているもの」を 2 行に分ける。Update が入れ替えるのは
     * カーネルだけなので、1 行にまとめると、押したあと直っていない
     * ものまで直ったように見える。 */
    snprintf(b, sizeof(b), "    System:     %s",
             up_os[0] ? up_os : "(not checked)");
    x98_text(&x98, win, 16, y + 22, b, x98.text);
    snprintf(b, sizeof(b), "    Kernel:     %s",
             up_kernel[0] ? up_kernel : "(not checked)");
    x98_text(&x98, win, 16, y + 40, b, x98.text);
    snprintf(b, sizeof(b), "    Available:  %s",
             up_latest[0] ? up_latest : "(not checked)");
    x98_text(&x98, win, 16, y + 58, b, x98.text);

    x98_button(&x98, win, UP_BTN_X(0), y + 80, UP_BTN_W, UP_BTN_H,
               up_busy ? "Working..." : "Check for updates", 0);
    int ks, ss;
    up_slots(&ks, &ss);
    if (ks >= 0)
        x98_button(&x98, win, UP_BTN_X(ks), y + 80,
                   UP_BTN_W, UP_BTN_H, "Update kernel", 0);
    if (ss >= 0)
        x98_button(&x98, win, UP_BTN_X(ss), y + 80,
                   UP_BTN_W, UP_BTN_H, "Update system", 0);

    if (up_msg[0])
        x98_text(&x98, win, 32, y + 110, up_msg, x98.shadow);

    y += 138;
    x98_text(&x98, win, 16, y, "What happens", x98.text);
    x98_text(&x98, win, 16, y + 20,
             "    The new kernel goes to the spare half of the boot area,",
             x98.shadow);
    x98_text(&x98, win, 16, y + 36,
             "    the new system to the spare root. Your files (/home) and",
             x98.shadow);
    /* 入れたアプリが消えることは、先に言っておかないと事故になる。
     * ルートを入れ替えるのだから apt で入れたものは付いてこない。
     * 「消えた」と気づくのは、次に使おうとしたときで、もう遅い。 */
    x98_text(&x98, win, 16, y + 52,
             "    accounts are kept. Programs you added need adding again.",
             x98.shadow);
    x98_text(&x98, win, 16, y + 72,
             "    If the new one does not start, hold R while booting",
             x98.text);
    x98_text(&x98, win, 16, y + 88,
             "    to go back to the previous one.", x98.text);

    y += 116;
    x98_text(&x98, win, 16, y, "Programs and security updates", x98.text);
    x98_button(&x98, win, 32, y + 20, UP_BTN_W, UP_BTN_H, "Add Programs", 0);
    x98_text(&x98, win, 32 + UP_BTN_W + 12, y + 26,
             "Browsers and other software.", x98.shadow);
}

static void draw_sound(void)
{
    int y = TAB_H + 20;

    x98_text(&x98, win, 16, y, "Where sound comes out", x98.text);
    x98_text(&x98, win, 16, y + 20,
             snd_all
             ? "    Every output is listed, plugged in or not."
             : "    Outputs with nothing plugged in are not listed.",
             x98.shadow);

    int ly = y + 44;
    x98_bevel(&x98, win, 16, ly, WIN_W - 32, MAX_SNK * SNK_ROW + 8, 1);
    x98_fill(&x98, win, 18, ly + 2, WIN_W - 36, MAX_SNK * SNK_ROW + 4,
             x98.white);

    for (int i = 0; i < n_snd; i++) {
        int ry = ly + 6 + i * SNK_ROW;
        x98_bevel(&x98, win, 26, ry, 13, 13, 0);
        x98_fill(&x98, win, 28, ry + 2, 9, 9, x98.white);
        if (i == snd_sel) x98_fill(&x98, win, 30, ry + 4, 5, 5, x98.text);

        char b[200];
        /* 逃げ道で出しているものは、そうと分かるようにしておく。
         * 黙って並べると「選んだのに鳴らない」の理由が見えない。 */
        snprintf(b, sizeof(b), "%s%s", snd[i].label,
                 snd[i].unplugged ? "  (not detected)" : "");
        x98_text(&x98, win, 48, ry + (13 - x98_text_h(&x98)) / 2 - 1,
                 b, x98.text);
    }

    if (n_snd == 0)
        x98_text(&x98, win, 30, ly + 8,
                 snd_msg[0] ? snd_msg : "Looking...", x98.shadow);

    int by = ly + MAX_SNK * SNK_ROW + 18;
    x98_button(&x98, win, 16, by, 100, 24, "Test", 0);
    x98_button(&x98, win, 124, by, 100, 24, "Refresh", 0);

    /* 逃げ道。挿さっているかの判定は当てにならないことがある
     * (ジャック検出を持たないコーデック、HDMI の ELD が来ていない、
     *  こちらの読み違い)。隠す作りには、必ず全部出す道を付けておく。 */
    x98_bevel(&x98, win, 244, by + 5, 13, 13, 0);
    x98_fill(&x98, win, 246, by + 7, 9, 9, x98.white);
    if (snd_all) {
        /* 小さいチェック。線 2 本で十分それらしく見える
         * (myos_setup.c の checkbox と同じ描き方)。 */
        XSetForeground(dpy, x98.gc, x98.text);
        XDrawLine(dpy, win, x98.gc, 247, by + 11, 249, by + 14);
        XDrawLine(dpy, win, x98.gc, 249, by + 14, 254, by + 8);
    }
    x98_text(&x98, win, 264, by + 5, "Show all outputs", x98.text);

    /* 2 行の説明と 1 行のエラーを、同じ場所で出し分ける。
     * 片方だけ差し替えると文が混ざって意味を成さなくなる。 */
    if (snd_msg[0] && n_snd > 0) {
        x98_text(&x98, win, 16, by + 32, snd_msg, x98.shadow);
    } else {
        x98_text(&x98, win, 16, by + 32,
                 "    The change takes effect at once, including sound",
                 x98.shadow);
        x98_text(&x98, win, 16, by + 48,
                 "    that is already playing.", x98.shadow);
    }
}

static void draw_system(void)
{
    char buf[256];
    int y = TAB_H + 20;

    x98_text(&x98, win, 16, y, "Account", x98.text);
    /* 環境変数は su の通り方で入っていないことがあるので、
     * 最後は uid から引く。 */
    const char *user = getenv("USER");
    if (!user || !user[0]) user = getenv("LOGNAME");
    if (!user || !user[0]) {
        struct passwd *pw = getpwuid(getuid());
        user = (pw && pw->pw_name) ? pw->pw_name : "(unknown)";
    }
    snprintf(buf, sizeof(buf), "    Logged on as:  %s", user);
    x98_text(&x98, win, 16, y + 22, buf, x98.text);
    snprintf(buf, sizeof(buf), "    Running as:    %s",
             geteuid() == 0 ? "root (administrator)" : "standard user");
    x98_text(&x98, win, 16, y + 40, buf, x98.text);
    x98_text(&x98, win, 16, y + 58,
             "    Anything system-wide asks for your password.", x98.shadow);

    y += 96;
    x98_text(&x98, win, 16, y, "Java heap size", x98.text);
    x98_text(&x98, win, 16, y + 20,
             "    How much memory Java programs may use.", x98.shadow);
    for (int i = 0; i < N_HEAPS; i++) {
        int rx = 32 + (i % HEAP_COLS) * HEAP_COL_W;
        int ry = y + 42 + (i / HEAP_COLS) * HEAP_ROW_H;
        x98_bevel(&x98, win, rx, ry, 13, 13, 0);
        x98_fill(&x98, win, rx + 2, ry + 2, 9, 9, x98.white);
        if (i == heap_sel) x98_fill(&x98, win, rx + 4, ry + 4, 5, 5, x98.text);
        x98_text(&x98, win, rx + 22, ry + (13 - x98_text_h(&x98)) / 2 - 1,
                 heaps[i].label, x98.text);
    }

    y += 42 + HEAP_ROWS * HEAP_ROW_H + 14;
    x98_text(&x98, win, 16, y, "Hardware", x98.text);
    x98_text(&x98, win, 16, y + 20,
             "    What is installed, and whether a driver is loaded for it.",
             x98.shadow);
    x98_button(&x98, win, 32, y + 38, 140, 24, "Device Manager...", 0);

    y += 78;
    x98_text(&x98, win, 16, y, "System settings", x98.text);
    x98_text(&x98, win, 16, y + 20,
             "    These settings are yours alone (~/.myos).", x98.shadow);
    x98_text(&x98, win, 16, y + 36,
             "    To change the defaults for everyone, edit /etc/myos", x98.shadow);
    x98_text(&x98, win, 16, y + 52,
             "    as an administrator.", x98.shadow);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);
    draw_tabs();
    switch (tab) {
    case TAB_LOOK:
        draw_preview();
        draw_list();
        draw_palette();
        draw_speed();
        break;
    case TAB_TYPES:  draw_types();  break;
    case TAB_BG:      draw_bg(); break;
    case TAB_DISPLAY: draw_display(); break;
    case TAB_SOUND:
        if (!snd_loaded) snd_load();
        draw_sound();
        break;
    case TAB_SYSTEM: draw_system(); break;
    case TAB_UPDATE: draw_update(); break;
    }
    draw_buttons();
}

/* タブの帯のどれを押したか。外なら -1。 */
static int tab_hit(int mx, int my)
{
    if (my < 4 || my > TAB_H + 4) return -1;
    int x = 8;
    for (int i = 0; i < N_TABS; i++) {
        int w = x98_text_w(&x98, tab_name[i]) + 24;
        if (mx >= x && mx < x + w) return i;
        x += w + 2;
    }
    return -1;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(void)
{
    signal(SIGCHLD, SIG_IGN);

    /* 日本語入力より前にロケールを立てる。X を開いたあとだと
     * Xlib が古いロケールのまま動いてしまう。 */
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-settings] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    res_load();                 /* 画面タブ: いまの設定を読んでおく */
    bg_load();                  /* 背景タブ: 絵の置き場を覗いておく */

    /* 今の設定がどのプリセットに一番近いか探しておく */
    for (int i = 0; i < N_SCHEMES; i++)
        if (schemes[i].desktop == x98.theme.desktop &&
            schemes[i].face == x98.theme.face) { cur_scheme = i; break; }

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XStoreName(dpy, win, "Settings");

    n_ftypes = ft_load(ftypes, FT_MAX);
    ft_pick(0);
    load_java();

    /* 大きさを変えられると配置が崩れるので固定にする */
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = WIN_W;
    hints.min_height = hints.max_height = WIN_H;
    XSetWMNormalHints(dpy, win, &hints);

    XMapWindow(dpy, win);

    /* 窓が出てから入力文脈を作る。窓より先に作ると
     * XNClientWindow に渡すものが無い。 */
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

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

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a_wm_delete) {
                XCloseDisplay(dpy);
                return 0;
            }
            break;

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            /* タブの切り替えが最優先 */
            int t = tab_hit(mx, my);
            if (t >= 0) {
                if (tab == TAB_TYPES) ft_commit();
                tab = t;
                redraw();
                break;
            }

            if (tab == TAB_TYPES) {
                /* 種類の一覧 */
                if (mx >= FT_LIST_X && mx < FT_LIST_X + FT_LIST_W &&
                    my >= FT_LIST_Y + 2 && my < FT_LIST_Y + FT_LIST_HEIGHT - 2) {
                    ft_commit();
                    int i = ft_top + (my - FT_LIST_Y - 2) / FT_ROW_H;
                    if (i >= 0 && i < n_ftypes) ft_pick(i);
                    redraw();
                    break;
                }
                /* ホイールで送る */
                if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                    int vis = (FT_LIST_HEIGHT - 4) / FT_ROW_H;
                    ft_top += (ev.xbutton.button == 4) ? -3 : 3;
                    if (ft_top > n_ftypes - vis) ft_top = n_ftypes - vis;
                    if (ft_top < 0) ft_top = 0;
                    redraw();
                    break;
                }
                /* 入力欄の行 */
                int dy = FT_LIST_Y + FT_LIST_HEIGHT + 16;
                if (my >= dy + 22 && my < dy + 42)      ft_field = 0;
                else if (my >= dy + 50 && my < dy + 70) ft_field = 1;
                redraw();
                break;
            }

            if (tab == TAB_BG) {
                int lh = BG_VIS * BG_ROW + 4;
                if (mx >= 16 && mx < WIN_W - 16 &&
                    my >= BG_Y + 2 && my < BG_Y + lh - 2) {
                    int i = bg_top + (my - BG_Y - 2) / BG_ROW;
                    if (i >= 0 && i < bg_n) bg_sel = i;
                    redraw();
                    break;
                }
                int mrow = BG_Y + lh + 12 + 20;
                if (my >= mrow && my < mrow + 13) {
                    for (int i = 0; i < BG_NMODE; i++) {
                        int rx = 32 + i * 150;
                        if (mx >= rx && mx < rx + 140) { bg_mode = i; break; }
                    }
                    redraw();
                    break;
                }
                int by = BG_Y + lh + 12 + 50;
                if (my >= by && my < by + 22 && mx >= 32 && mx < 122) {
                    bg_apply();
                    redraw();
                    break;
                }
                if (my < BTN_Y) break;
            }

            if (tab == TAB_DISPLAY) {
                if (my >= RES_Y && my < RES_Y + 13 && mx >= 32 && mx < 380) {
                    res_sel = -1;
                    redraw();
                    break;
                }
                for (int i = 0; i < N_RES; i++) {
                    int ry = RES_Y + (i + 1) * RES_ROW;
                    if (my >= ry && my < ry + 13 && mx >= 32 && mx < 380) {
                        res_sel = i;
                        redraw();
                        break;
                    }
                }
                int by = RES_Y + (N_RES + 1) * RES_ROW + 10;
                if (my >= by && my < by + 22 && mx >= 32 && mx < 122) {
                    res_apply();
                    redraw();
                }
                /* セーフグラフィックスのチェック。draw_display と
                 * 同じ積み方でなければならない。 */
                int scy = by + 44 + 22;
                if (my >= scy - 2 && my < scy + 15 &&
                    mx >= 32 && mx < WIN_W - 32) {
                    res_safe = !res_safe;
                    redraw(); XFlush(dpy);
                    res_safe_apply();
                    redraw();
                }
                if (my < BTN_Y) break;
            }

            if (tab == TAB_UPDATE) {
                int y = TAB_H + 20;
                int by = y + 80;
                if (my >= by && my < by + UP_BTN_H) {
                    int ks, ss, hit = -1;
                    up_slots(&ks, &ss);
                    for (int i = 0; i < 3; i++)
                        if (mx >= UP_BTN_X(i) && mx < UP_BTN_X(i) + UP_BTN_W)
                            hit = i;

                    /* 別のプロセスに任せる。ここで待つと、数分かかる
                     * ダウンロードの間ずっと固まって見える。端末を出して
                     * 進み具合が見えるようにする。 */
                    const char *sub = NULL;
                    if (hit == 0) {
                        redraw(); XFlush(dpy);
                        up_check();
                    } else if (hit >= 0 && hit == ks) {
                        sub = "apply";
                    } else if (hit >= 0 && hit == ss) {
                        sub = "apply-system";
                    }

                    if (sub) {
                        char c[200];
                        snprintf(c, sizeof(c),
                                 "sudo -n /usr/local/bin/myos-update %s; "
                                 "echo; echo 'Press Enter to close'; read x",
                                 sub);
                        up_busy = 1;
                        snprintf(up_msg, sizeof(up_msg),
                                 "A window will show the progress.");
                        pid_t p = fork();
                        if (p == 0) {
                            execl("/usr/local/bin/myos-term", "myos-term",
                                  "sh", "-c", c, (char *)NULL);
                            _exit(127);
                        }
                        up_busy = 0;
                    }
                }
                int ay = y + 138 + 116 + 20;   /* draw_update と同じ積み方 */
                if (my >= ay && my < ay + UP_BTN_H &&
                    mx >= 32 && mx < 32 + UP_BTN_W) {
                    pid_t p = fork();
                    if (p == 0) {
                        execl("/usr/local/bin/myos-getapps",
                              "myos-getapps", (char *)NULL);
                        _exit(127);
                    }
                }
                redraw();
                break;
            }

            if (tab == TAB_SOUND) {
                int ly = TAB_H + 20 + 44;
                for (int i = 0; i < n_snd; i++) {
                    int ry = ly + 6 + i * SNK_ROW;
                    if (my >= ry && my < ry + 16 &&
                        mx >= 26 && mx < WIN_W - 32) {
                        if (snd_sel != i) {
                            snd_sel = i;
                            /* 選んだ瞬間に切り替える。Apply を待たせると
                             * 「音を聞いて確かめる」が出来ない。 */
                            snd_apply();
                        }
                        redraw();
                        break;
                    }
                }
                int by = ly + MAX_SNK * SNK_ROW + 18;
                if (my >= by && my < by + 24) {
                    if (mx >= 16 && mx < 116) {
                        /* テスト音。鳴らないときは選び直せばよい。 */
                        pid_t p = fork();
                        if (p == 0) {
                            execlp("speaker-test", "speaker-test",
                                   "-c", "2", "-t", "wav", "-l", "1",
                                   (char *)NULL);
                            _exit(127);
                        }
                    } else if (mx >= 124 && mx < 224) {
                        snd_load();
                        redraw();
                    } else if (mx >= 244 && mx < 420) {
                        snd_all = !snd_all;
                        snd_load();
                        redraw();
                    }
                }
                if (my < BTN_Y) break;
            }

            if (tab == TAB_SYSTEM) {
                int y = TAB_H + 20 + 96 + 42;
                for (int i = 0; i < N_HEAPS; i++) {
                    int rx = 32 + (i % HEAP_COLS) * HEAP_COL_W;
                    int ry = y + (i / HEAP_COLS) * HEAP_ROW_H;
                    if (my >= ry && my < ry + 16 &&
                        mx >= rx && mx < rx + HEAP_COL_W - 10) {
                        heap_sel = i;
                        redraw();
                        break;
                    }
                }
                /* デバイスマネージャー。draw_system() の位置と揃えること。 */
                int dy = TAB_H + 20 + 96 + 42 + HEAP_ROWS * HEAP_ROW_H + 14 + 38;
                if (my >= dy && my < dy + 24 && mx >= 32 && mx < 32 + 140) {
                    pid_t p = fork();
                    if (p == 0) {
                        execl("/usr/local/bin/myos-devmgr",
                              "myos-devmgr", (char *)NULL);
                        _exit(127);
                    }
                }
                if (my < BTN_Y) break;
            }

            /* 以下は外観タブのときだけ */
            if (tab != TAB_LOOK && my < BTN_Y) break;

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
                    save_all();
                    XCloseDisplay(dpy);
                    return 0;
                }
                if (mx >= bx + BTN_W + 8 && mx < bx + 2 * BTN_W + 8) {
                    save_all();                                 /* Apply */
                    break;
                }
                if (mx >= bx + (BTN_W + 8) * 2) {               /* Cancel */
                    XCloseDisplay(dpy);
                    return 0;
                }
            }
            break;
        }

        case KeyPress: {
            if (tab != TAB_TYPES) break;
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);
            X98Edit *e = ft_field ? &ed_run : &ed_open;

            if (ks == XK_Tab)              ft_field ^= 1;
            else if (ks == XK_BackSpace)   { x98_edit_backspace(e); ft_dirty = 1; }
            else if (ks == XK_Delete)      { x98_edit_delete(e); ft_dirty = 1; }
            else if (ks == XK_Return || ks == XK_KP_Enter) {
                ft_commit();
                ft_dirty = 1;
            } else if (ks == XK_Up) {
                ft_commit();
                if (ft_sel > 0) ft_pick(ft_sel - 1);
            } else if (ks == XK_Down) {
                ft_commit();
                if (ft_sel < n_ftypes - 1) ft_pick(ft_sel + 1);
            } else if (n > 0 && (unsigned char)buf[0] >= 0x20) {
                buf[n] = 0;
                x98_edit_insert(e, buf, n);
                ft_dirty = 1;
            }
            /* 選んだ行が見えるように送る */
            int vis = (FT_LIST_HEIGHT - 4) / FT_ROW_H;
            if (ft_sel < ft_top) ft_top = ft_sel;
            if (ft_sel >= ft_top + vis) ft_top = ft_sel - vis + 1;
            redraw();
            break;
        }

        default:
            break;
        }
    }
    return 0;
}

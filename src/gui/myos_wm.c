/* ==========================================================================
 * myos_wm.c  -  Windows 98 風の X11 ウィンドウマネージャ + デスクトップ
 *
 * 位置づけ:
 *   Firefox のような本物のアプリを載せるために X11 を採用したので、
 *   「Win98 の見た目」を担うのはウィンドウマネージャという形になる。
 *
 *     自作ブートローダー
 *       -> Linux カーネル
 *         -> Xorg (画面を持つ)
 *           -> このウィンドウマネージャ (見た目と操作を持つ)
 *             -> Firefox / ファイルマネージャなどのアプリ
 *
 * やっていること:
 *   - デスクトップ (ティール色 + アイコン)。アイコンはダブルクリックで起動
 *   - クライアントを枠に入れ直す (reparenting) して Win98 の枠を描く
 *   - タイトルバーのドラッグで移動、最小化 / 最大化 / 閉じる
 *   - 画面下にタスクバー (スタートボタン + タスクボタン + 時計)
 *   - 時計は RTC 由来のシステム時刻をそのまま表示する
 *
 * デスクトップのアイコンは /etc/myos/desktop.conf で定義する。
 *   ラベル|アイコン名|コマンド
 *
 * ビルド:
 *   gcc -O2 -o myos-wm myos_wm.c -lX11
 *
 *   静的リンクしてはいけない。libX11 はカーソル作成で libXcursor を
 *   dlopen するため、リンク時とは別版の glibc を読み込んで落ちる。
 *   rootfs とホストで glibc の版が違う場合は rootfs の中でビルドすること
 *   (tools/build_rootfs.sh がそうしている)。
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

#include "x98.h"

/* --- Win98 の寸法 -------------------------------------------------------- */
#define MAX_CLIENTS  32
#define BORDER       4       /* フレームの太さ */
#define TITLE_H      20
#define BTN_SZ       14
#define TASKBAR_H    28
#define START_W      72
#define TASKBTN_W    160
#define MENU_W       168
#define MENU_ITEM_H  22

/* rootfs には helvetica が無く 9x15bold にフォールバックする。
 * 1 文字 9px なので、13 文字くらいは入る幅にしておく。 */
#define ICON_CELL_W  124
#define ICON_CELL_H  84
#define ICON_TOP     16
#define ICON_LEFT    12
#define DBLCLICK_MS  700     /* ダブルクリックとみなす間隔。Windows の既定は 500ms 前後。
                              * 遅めにしておかないと取りこぼす */

#define MAX_ICONS    24
#define DESKTOP_CONF "/etc/myos/desktop.conf"
#define STARTMENU_CONF "/etc/myos/startmenu.conf"
#define RESIZE_GRIP  6       /* 枠のこの幅を掴むとリサイズ */
#define MIN_W        180
#define MIN_H        100

static Display *dpy;
static int      screen;
static Window   root, desktop, taskbar, startmenu;
static Window   volwin;      /* 音量の小さい窓 */
static int      scr_w, scr_h;
static int      menu_open = 0;
static X98      x98;

static Atom a_wm_protocols, a_wm_delete, a_net_wm_name, a_utf8;

/* --- デスクトップのアイコン ---------------------------------------------- */
typedef struct {
    char label[48];
    char icon[16];
    char cmd[256];
} Icon;

static Icon icons[MAX_ICONS];
static int  n_icons = 0;
static int  sel_icon = -1;

/* スタートメニューの項目も同じ形で持つ */
static Icon menu[MAX_ICONS];
static int  n_menu = 0;

/* 起動直後の既定。/etc/myos/desktop.conf が無いときに使う。 */
static const char *default_icons[] = {
    "My Computer|computer|/usr/local/bin/myos-files /",
    "My Documents|folder|/usr/local/bin/myos-files /root",
    "Firefox|globe|/usr/bin/firefox-esr",
    NULL
};

static void add_line_to(Icon *arr, int *n, const char *line)
{
    if (*n >= MAX_ICONS) return;
    if (line[0] == '#' || line[0] == '\n' || line[0] == 0) return;

    char buf[400];
    buf[0] = 0;
    strncat(buf, line, sizeof(buf) - 1);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    if (!buf[0]) return;

    char *p1 = strchr(buf, '|');
    if (!p1) return;
    *p1++ = 0;
    char *p2 = strchr(p1, '|');
    if (!p2) return;
    *p2++ = 0;

    Icon *ic = &arr[(*n)++];
    snprintf(ic->label, sizeof(ic->label), "%s", buf);
    snprintf(ic->icon,  sizeof(ic->icon),  "%s", p1);
    snprintf(ic->cmd,   sizeof(ic->cmd),   "%s", p2);
}

static void load_list(const char *path, Icon *arr, int *n,
                      const char **fallback)
{
    *n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[400];
        while (fgets(line, sizeof(line), f)) add_line_to(arr, n, line);
        fclose(f);
    }
    if (!*n && fallback)
        for (int i = 0; fallback[i]; i++) add_line_to(arr, n, fallback[i]);
}

/* スタートメニューの既定。設定ファイルが無いときに使う。 */
static const char *default_menu[] = {
    "Programs|app|/usr/local/bin/myos-files /usr/bin",
    "Documents|folder|/usr/local/bin/myos-files /root",
    "Settings|app|/usr/local/bin/myos-settings",
    "Files|folder|/usr/local/bin/myos-files /",
    "Web|globe|/usr/bin/firefox-esr",
    "MS-DOS Prompt|app|/usr/bin/xterm -bg black -fg lightgray",
    /* systemd を使っていないので /sbin/poweroff は無い。
     * 終了の画面を出して、そこから自前の myos-poweroff を呼ぶ。 */
    "Shut Down|app|/usr/local/bin/myos-shutdown",
    NULL
};

static void load_icons(void)
{
    load_list(DESKTOP_CONF, icons, &n_icons, default_icons);
    load_list(STARTMENU_CONF, menu, &n_menu, default_menu);
}

static void draw_icon_glyph(const char *name, Drawable d, int px, int py)
{
    if      (!strcmp(name, "computer")) x98_icon_computer(&x98, d, px, py);
    else if (!strcmp(name, "folder"))   x98_icon_folder(&x98, d, px, py);
    else if (!strcmp(name, "file"))     x98_icon_file(&x98, d, px, py);
    else if (!strcmp(name, "globe"))    x98_icon_globe(&x98, d, px, py);
    else if (!strcmp(name, "java"))     x98_icon_java(&x98, d, px, py);
    else                                x98_icon_app(&x98, d, px, py);
}

static void icon_pos(int i, int *px, int *py)
{
    int rows = (scr_h - TASKBAR_H - ICON_TOP) / ICON_CELL_H;
    if (rows < 1) rows = 1;
    *px = ICON_LEFT + (i / rows) * ICON_CELL_W;
    *py = ICON_TOP  + (i % rows) * ICON_CELL_H;
}

static void draw_desktop(void)
{
    x98_fill(&x98, desktop, 0, 0, scr_w, scr_h - TASKBAR_H, x98.desktop);

    for (int i = 0; i < n_icons; i++) {
        int px, py;
        icon_pos(i, &px, &py);
        int gx = px + (ICON_CELL_W - X98_ICON_W) / 2;
        draw_icon_glyph(icons[i].icon, desktop, gx, py);

        /* セルより長いラベルは尻を落として "…" にする。
         * そのまま描くと画面の端で切れて読めなくなる。 */
        char label[sizeof(icons[i].label) + 1];
        memcpy(label, icons[i].label, sizeof(icons[i].label));
        label[sizeof(icons[i].label)] = 0;
        while (x98_text_w(&x98, label) > ICON_CELL_W - 4 &&
               strlen(label) > 4) {
            label[strlen(label) - 1] = 0;
            label[strlen(label) - 1] = '.';
            label[strlen(label) - 2] = '.';
        }

        int tw = x98_text_w(&x98, label);
        int tx = px + (ICON_CELL_W - tw) / 2;
        if (tx < px + 2) tx = px + 2;
        int ty = py + X98_ICON_H + 6;
        if (i == sel_icon) {
            /* 選択中は Win98 と同じく反転した帯を敷く */
            x98_fill(&x98, desktop, tx - 2, ty - 1, tw + 4,
                     x98_text_h(&x98) + 2, x98.select_bg);
            x98_text(&x98, desktop, tx, ty, label, x98.white);
        } else {
            x98_text_sh(&x98, desktop, tx, ty, label,
                        x98.white, x98_rgb24(&x98, 0x004040));
        }
    }
}

static int icon_hit(int mx, int my)
{
    for (int i = 0; i < n_icons; i++) {
        int px, py;
        icon_pos(i, &px, &py);
        if (mx >= px && mx < px + ICON_CELL_W &&
            my >= py && my < py + ICON_CELL_H)
            return i;
    }
    return -1;
}

/* --- プロセス起動 -------------------------------------------------------- */
static void spawn(const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* --- クライアント -------------------------------------------------------- */
typedef struct {
    Window client, frame;
    int    x, y, w, h;      /* frame の位置と大きさ */
    int    cw, ch;          /* クライアント領域 */
    int    used, minimized, maximized;
    int    old_x, old_y, old_w, old_h;
    char   title[256];
} Client;

static Client clients[MAX_CLIENTS];

/* 重なり順。zlist[0] がいちばん手前。
 * X 自体も重なり順を持っているが、Alt+Tab で「次の窓」を選ぶには
 * 使った順を自分で覚えておく必要がある。 */
static Client *zlist[MAX_CLIENTS];
static int     n_z = 0;
static Client *focused = NULL;

static void z_remove(Client *c)
{
    int j = 0;
    for (int i = 0; i < n_z; i++)
        if (zlist[i] != c) zlist[j++] = zlist[i];
    n_z = j;
}

static void z_front(Client *c)
{
    z_remove(c);
    for (int i = n_z; i > 0; i--) zlist[i] = zlist[i - 1];
    zlist[0] = c;
    n_z++;
}

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

/* --- ウィンドウの枠 ------------------------------------------------------ */
/* 右上のボタン。i=0 最小化, 1 最大化, 2 閉じる */
static void btn_rect(Client *c, int i, int *bx, int *by)
{
    int right = c->w - BORDER - 2;
    *bx = right - (3 - i) * (BTN_SZ + 2) + 2;
    *by = BORDER + (TITLE_H - BTN_SZ) / 2;
}

static void draw_frame(Client *c)
{
    x98_fill(&x98, c->frame, 0, 0, c->w, c->h, x98.face);
    x98_bevel(&x98, c->frame, 0, 0, c->w, c->h, 1);
    x98_titlebar_ex(&x98, c->frame, BORDER, BORDER,
                    c->w - 2 * BORDER, TITLE_H, c->title, c == focused);

    static const char *labels[3] = { "_", "[]", "X" };
    for (int i = 0; i < 3; i++) {
        int bx, by;
        btn_rect(c, i, &bx, &by);
        x98_button(&x98, c->frame, bx, by, BTN_SZ, BTN_SZ, labels[i], 0);
    }

    x98_bevel(&x98, c->frame, BORDER - 2, BORDER + TITLE_H - 1,
              c->w - 2 * (BORDER - 2),
              c->h - (BORDER + TITLE_H - 1) - (BORDER - 2), 0);

    /* 右下のリサイズつまみ (Win98 の斜線) */
    if (!c->maximized) {
        for (int i = 0; i < 3; i++) {
            int o = 3 + i * 4;
            for (int k = 0; k < 8; k++) {
                x98_fill(&x98, c->frame, c->w - o - k, c->h - 3 - (8 - k),
                         2, 2, x98.light);
                x98_fill(&x98, c->frame, c->w - o - k + 1,
                         c->h - 2 - (8 - k), 1, 1, x98.shadow);
            }
        }
    }
}

/* タイトルを、表示できる文字だけに詰め直す。
 *
 * 表示に使っているのは 9x15 のビットマップフォントで、ASCII しか
 * 字形を持っていない。UTF-8 のまま 1 バイトずつ描くと
 *   "Server Not Found — Mozilla Firefox"
 * の em ダッシュ (E2 80 94) が "â" + 豆腐 2 個に化ける。実際そうなった。
 *
 * 記号はよく出るものだけ ASCII に置き換え、残りは '?' にする。
 * 日本語などは字形が無いので、化けた絵より '?' のほうがまだ読める。 */
__attribute__((unused))
static void ascii_fold(char *dst, size_t dsz, const char *src)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;

    while (*p && o + 1 < dsz) {
        if (*p < 0x80) {                        /* そのまま使える */
            dst[o++] = (char)*p++;
            continue;
        }

        /* UTF-8 を 1 文字ぶん読んでコードポイントにする */
        unsigned int cp = 0;
        int len = 0;
        if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { p++; dst[o++] = '?'; continue; }  /* 壊れている */

        int ok = 1;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!ok) { p++; dst[o++] = '?'; continue; }
        p += len;

        const char *rep;
        switch (cp) {
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015: rep = "-";  break;  /* 各種ダッシュ */
        case 0x2018: case 0x2019: rep = "'";  break;               /* 曲がった引用符 */
        case 0x201C: case 0x201D: rep = "\""; break;
        case 0x2026: rep = "..."; break;                           /* 三点リーダ */
        case 0x00A0: rep = " ";   break;                           /* 非改行空白 */
        case 0x00B7: case 0x2022: rep = "*"; break;                /* 中黒 / 箇条書き */
        case 0x00D7: rep = "x";   break;
        default:     rep = "?";   break;
        }
        while (*rep && o + 1 < dsz) dst[o++] = *rep++;
    }
    dst[o] = 0;
}

static void fetch_title(Client *c)
{
    c->title[0] = 0;

    /* まず _NET_WM_NAME (UTF-8)。最近のアプリはこちらしか設定しない */
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
    if (!c->title[0]) snprintf(c->title, sizeof(c->title), "(untitled)");

    /* ascii_fold はもう掛けない。
     * Xft と日本語フォントに移したので、タイトルはそのまま出せる。
     * 潰すと日本語の窓が全部 "?" になってしまう。 */
}

/* --- タスクバー / スタートメニュー --------------------------------------- */
#define MENU_H (n_menu * MENU_ITEM_H + 8)

static int menu_hover = -1;

static void draw_startmenu(void)
{
    int w = MENU_W, h = MENU_H;
    x98_fill(&x98, startmenu, 0, 0, w, h, x98.face);
    x98_bevel(&x98, startmenu, 0, 0, w, h, 1);

    /* 左端の縦帯 (Win98 で "Windows 98" と入っているところ) */
    x98_fill(&x98, startmenu, 3, 3, 20, h - 6, x98_rgb24(&x98, RGB_TITLE1));
    int sy = h - 16;
    for (const char *p = "myOS"; *p; p++) {
        char s[2] = { *p, 0 };
        x98_text(&x98, startmenu, 8, sy, s, x98.titletxt);
        sy -= x98_text_h(&x98) + 2;
    }

    int iy = 4;
    for (int i = 0; i < n_menu; i++) {
        if (i == menu_hover) {
            x98_fill(&x98, startmenu, 26, iy, w - 30, MENU_ITEM_H,
                     x98.select_bg);
        }
        x98_text(&x98, startmenu, 32,
                 iy + (MENU_ITEM_H - x98_text_h(&x98)) / 2,
                 menu[i].label, i == menu_hover ? x98.titletxt : x98.text);
        if (i == n_menu - 2) {
            x98_hline(&x98, startmenu, 26, iy + MENU_ITEM_H - 2, w - 32,
                      x98.shadow);
            x98_hline(&x98, startmenu, 26, iy + MENU_ITEM_H - 1, w - 32,
                      x98.light);
        }
        iy += MENU_ITEM_H;
    }
}

/* 時計。カーネルが起動時に RTC (CMOS) から入れたシステム時刻をそのまま使う。
 * ブートローダー側で何かする必要はない。 */
static void clock_string(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%H:%M", &tmv);
}

/* maxw ピクセルに収まるまで削って、切ったら "..." を付ける。 */
static void fit_label(char *dst, size_t dsz, const char *src, int maxw)
{
    snprintf(dst, dsz, "%s", src);
    if (x98_text_w(&x98, dst) <= maxw) return;

    size_t n = strlen(dst);
    while (n > 0) {
        char probe[80];
        dst[--n] = 0;
        snprintf(probe, sizeof(probe), "%s...", dst);
        if (x98_text_w(&x98, probe) <= maxw) {
            snprintf(dst, dsz, "%s", probe);
            return;
        }
    }
    dst[0] = 0;
}

/* --- 通知領域 ------------------------------------------------------------
 * 時計の左に、音量とネットワークの状態を出す。
 * 98 のタスクトレイと同じ位置。押すと中身が出る。 */
#define TRAY_ICON 20
#define TRAY_W    (TRAY_ICON * 2 + 12)

static int  net_up   = 0;                /* 繋がっているか */
static char net_addr[32] = "";           /* 繋がっているなら住所 */
static int  vol_pct  = -1;               /* -1 = まだ読んでいない */
static int  vol_mute = 0;
static int  vol_open = 0;                /* 音量の窓を出しているか */

/* 繋がっているかを /sys と getifaddrs から見る。
 * コマンドを起動しないので、1 秒ごとに呼んでも負担にならない。 */
static void net_poll(void)
{
    int up = 0;
    char addr[32] = "";

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            /* lo と sit0 のような擬似デバイスは繋がったうちに入らない */
            if (!strcmp(p->ifa_name, "lo")) continue;
            if (!strncmp(p->ifa_name, "sit", 3)) continue;
            if (!(p->ifa_flags & IFF_UP)) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));
            up = 1;
            break;
        }
        freeifaddrs(ifa);
    }
    net_up = up;
    snprintf(net_addr, sizeof(net_addr), "%s", addr);
}

/* amixer から今の音量を読む。窓を開けるときだけ呼ぶ。 */
static void vol_poll(void)
{
    vol_pct = -1;
    vol_mute = 0;
    FILE *f = popen("amixer sget Master 2>/dev/null", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *b = strchr(line, '[');
        if (!b) continue;
        int v;
        if (sscanf(b, "[%d%%]", &v) == 1 && vol_pct < 0) vol_pct = v;
        if (strstr(line, "[off]")) vol_mute = 1;
    }
    pclose(f);
}

static void vol_apply(void)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "amixer sset Master %d%% %s >/dev/null 2>&1 &",
             vol_pct < 0 ? 50 : vol_pct, vol_mute ? "mute" : "unmute");
    if (system(cmd) == -1) { /* 音が変わらないだけなので黙って諦める */ }
}

/* スピーカーの絵。消音なら × を重ねる。 */
static void draw_speaker(Drawable d, int x, int y, int muted)
{
    unsigned long fg = x98.text;
    XSetForeground(dpy, x98.gc, fg);
    /* 本体 */
    XFillRectangle(dpy, d, x98.gc, x + 2, y + 5, 3, 5);
    XPoint horn[4] = { {(short)(x + 5), (short)(y + 5)},
                       {(short)(x + 9), (short)(y + 1)},
                       {(short)(x + 9), (short)(y + 14)},
                       {(short)(x + 5), (short)(y + 10)} };
    XFillPolygon(dpy, d, x98.gc, horn, 4, Convex, CoordModeOrigin);
    if (muted) {
        XSetForeground(dpy, x98.gc, x98_rgb24(&x98, 0xC00000));
        XDrawLine(dpy, d, x98.gc, x + 11, y + 4, x + 16, y + 11);
        XDrawLine(dpy, d, x98.gc, x + 16, y + 4, x + 11, y + 11);
    } else {
        /* 音が出ている印に弧を 2 本 */
        XDrawArc(dpy, d, x98.gc, x + 8, y + 3, 8, 10, -60 * 64, 120 * 64);
        XDrawArc(dpy, d, x98.gc, x + 10, y + 1, 10, 14, -60 * 64, 120 * 64);
    }
}

/* 端末が 2 台。繋がっていなければ × を重ねる。 */
static void draw_neticon(Drawable d, int x, int y, int up)
{
    XSetForeground(dpy, x98.gc, x98.text);
    XFillRectangle(dpy, d, x98.gc, x + 1, y + 2, 7, 5);
    XFillRectangle(dpy, d, x98.gc, x + 8, y + 8, 7, 5);
    XSetForeground(dpy, x98.gc, x98.shadow);
    XDrawLine(dpy, d, x98.gc, x + 4, y + 7, x + 11, y + 8);
    if (!up) {
        XSetForeground(dpy, x98.gc, x98_rgb24(&x98, 0xC00000));
        XDrawLine(dpy, d, x98.gc, x + 2, y + 3, x + 14, y + 13);
        XDrawLine(dpy, d, x98.gc, x + 14, y + 3, x + 2, y + 13);
    }
}

static void draw_taskbar(void)
{
    x98_fill(&x98, taskbar, 0, 0, scr_w, TASKBAR_H, x98.face);
    x98_hline(&x98, taskbar, 0, 0, scr_w, x98.light);

    /* スタートボタン。旗と文字が重ならないよう中央揃えにはしない */
    int sb_h = TASKBAR_H - 6;
    x98_fill(&x98, taskbar, 3, 3, START_W, sb_h, x98.face);
    x98_bevel(&x98, taskbar, 3, 3, START_W, sb_h, !menu_open);
    int off = menu_open ? 1 : 0;

    /* ボタンの記章。
     * 最初は 4 色の四角を 2x2 に並べていたが、あれは Windows の旗そのもの
     * なので、配るものには載せない。代わりに myOS の頭文字を置く。 */
    x98_text(&x98, taskbar, 9 + off,
             3 + off + (sb_h - x98_text_h(&x98)) / 2, "m",
             x98_rgb24(&x98, 0x000080));
    x98_text(&x98, taskbar, 24 + off, 3 + off + (sb_h - x98_text_h(&x98)) / 2,
             "Start", x98.text);

    x98_vline(&x98, taskbar, START_W + 8, 4, TASKBAR_H - 8, x98.shadow);
    x98_vline(&x98, taskbar, START_W + 9, 4, TASKBAR_H - 8, x98.light);

    int px = START_W + 14;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        if (px + TASKBTN_W > scr_w - 80 - TRAY_W) break;

        /* ラベルは幅 (ピクセル) で詰める。文字数で切ると、
         * 34 文字が 300px になってボタン (160px) から溢れ、
         * x98_button の中央揃えのせいで開始位置が負になり、
         * 左隣のスタートボタンに文字が重なる。実際そうなった。
         * 98 と同じく左寄せにして、切ったところに "..." を付ける。 */
        char label[64];
        fit_label(label, sizeof(label), clients[i].title, TASKBTN_W - 14);

        int pressed = !clients[i].minimized;
        int off = pressed ? 1 : 0;
        x98_fill(&x98, taskbar, px, 3, TASKBTN_W, TASKBAR_H - 6, x98.face);
        x98_bevel(&x98, taskbar, px, 3, TASKBTN_W, TASKBAR_H - 6, !pressed);
        x98_text(&x98, taskbar, px + 7 + off,
                 3 + off + (TASKBAR_H - 6 - x98_text_h(&x98)) / 2,
                 label, x98.text);

        px += TASKBTN_W + 4;
    }

    char clk[16];
    clock_string(clk, sizeof(clk));
    int cw = 62, cx = scr_w - cw - 4;

    /* 通知領域。時計と地続きの窪みにして 98 の見た目に寄せる。 */
    int tx = cx - TRAY_W - 2;
    x98_fill(&x98, taskbar, tx, 3, TRAY_W, TASKBAR_H - 6, x98.face);
    x98_bevel(&x98, taskbar, tx, 3, TRAY_W, TASKBAR_H - 6, 0);
    int iy = 3 + (TASKBAR_H - 6 - 16) / 2;
    draw_neticon(taskbar, tx + 5, iy, net_up);
    draw_speaker(taskbar, tx + 5 + TRAY_ICON, iy, vol_mute);

    x98_fill(&x98, taskbar, cx, 3, cw, TASKBAR_H - 6, x98.face);
    x98_bevel(&x98, taskbar, cx, 3, cw, TASKBAR_H - 6, 0);
    x98_text(&x98, taskbar, cx + (cw - x98_text_w(&x98, clk)) / 2,
             3 + (TASKBAR_H - 6 - x98_text_h(&x98)) / 2, clk, x98.text);
}

#define VOL_W 74
#define VOL_H 172
#define VOL_TRACK_Y 26
#define VOL_TRACK_H 92

static void draw_volwin(void)
{
    x98_fill(&x98, volwin, 0, 0, VOL_W, VOL_H, x98.face);
    x98_bevel(&x98, volwin, 0, 0, VOL_W, VOL_H, 1);

    x98_text(&x98, volwin, (VOL_W - x98_text_w(&x98, "Volume")) / 2, 8,
             "Volume", x98.text);

    /* 溝。98 のスライダは細い窪み。 */
    int tx = VOL_W / 2 - 2;
    x98_bevel(&x98, volwin, tx, VOL_TRACK_Y, 4, VOL_TRACK_H, 0);

    int v = vol_pct < 0 ? 50 : vol_pct;
    int knob = VOL_TRACK_Y + VOL_TRACK_H - 8 - (VOL_TRACK_H - 16) * v / 100;
    x98_fill(&x98, volwin, tx - 9, knob, 22, 12, x98.face);
    x98_bevel(&x98, volwin, tx - 9, knob, 22, 12, 1);

    char pc[16];
    snprintf(pc, sizeof(pc), "%d%%", v);
    x98_text(&x98, volwin, (VOL_W - x98_text_w(&x98, pc)) / 2,
             VOL_TRACK_Y + VOL_TRACK_H + 6, pc, x98.text);

    /* 消音 */
    int my = VOL_H - 26;
    x98_bevel(&x98, volwin, 8, my, 13, 13, 0);
    x98_fill(&x98, volwin, 10, my + 2, 9, 9, x98.white);
    if (vol_mute) {
        XSetForeground(dpy, x98.gc, x98.text);
        XDrawLine(dpy, volwin, x98.gc, 11, my + 6, 14, my + 9);
        XDrawLine(dpy, volwin, x98.gc, 14, my + 9, 18, my + 3);
    }
    x98_text(&x98, volwin, 26, my - 1, "Mute", x98.text);
    XFlush(dpy);
}

static void set_volwin(int open)
{
    vol_open = open;
    if (open) {
        vol_poll();
        int cx = scr_w - 62 - 4;
        int tx = cx - TRAY_W - 2;
        int wx = tx + TRAY_ICON - VOL_W / 2 + 5;
        if (wx + VOL_W > scr_w) wx = scr_w - VOL_W - 2;
        XMoveResizeWindow(dpy, volwin, wx,
                          scr_h - TASKBAR_H - VOL_H, VOL_W, VOL_H);
        XMapRaised(dpy, volwin);
        draw_volwin();
    } else {
        XUnmapWindow(dpy, volwin);
    }
}

static void volwin_click(int mx, int my)
{
    int mbox = VOL_H - 26;
    if (my >= mbox - 2 && my < mbox + 15 && mx < 70) {
        vol_mute = !vol_mute;
        vol_apply();
        draw_volwin();
        draw_taskbar();
        XFlush(dpy);
        return;
    }
    if (my >= VOL_TRACK_Y && my < VOL_TRACK_Y + VOL_TRACK_H) {
        /* 溝の中の y から決める。上が大きい。 */
        int rel = (VOL_TRACK_Y + VOL_TRACK_H - 8) - my;
        int v = rel * 100 / (VOL_TRACK_H - 16);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        vol_pct = v;
        if (v > 0) vol_mute = 0;
        vol_apply();
        draw_volwin();
        draw_taskbar();
        XFlush(dpy);
    }
}

/* トレイのどれを押したか。0 = ネットワーク / 1 = 音量 / -1 = 外 */
static int tray_hit(int mx)
{
    int cx = scr_w - 62 - 4;
    int tx = cx - TRAY_W - 2;
    if (mx < tx + 3 || mx >= tx + TRAY_W) return -1;
    return (mx < tx + 5 + TRAY_ICON) ? 0 : 1;
}

static int taskbtn_hit(int mx)
{
    int px = START_W + 14;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        if (px + TASKBTN_W > scr_w - 80 - TRAY_W) break;
        if (mx >= px && mx < px + TASKBTN_W) return i;
        px += TASKBTN_W + 4;
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
    Client *old = focused;

    z_front(c);
    XRaiseWindow(dpy, c->frame);
    XRaiseWindow(dpy, taskbar);
    if (menu_open) XRaiseWindow(dpy, startmenu);
    XSetInputFocus(dpy, c->client, RevertToPointerRoot, CurrentTime);

    focused = c;
    if (old && old != c && old->used) draw_frame(old);
    draw_frame(c);
    draw_taskbar();
}

/* Alt+Tab: 使った順で 1 つ後ろの窓を手前に出す。
 * back = 1 (Alt+Shift+Tab) なら逆回りで、一番奥の窓を手前に出す。 */
static void cycle_windows(int back)
{
    if (n_z < 2) return;
    if (back) {
        for (int i = n_z - 1; i >= 1; i--) {
            if (zlist[i]->used && !zlist[i]->minimized) {
                raise_client(zlist[i]);
                return;
            }
        }
    } else {
        for (int i = 1; i < n_z; i++) {
            if (zlist[i]->used && !zlist[i]->minimized) {
                raise_client(zlist[i]);
                return;
            }
        }
    }
    /* 全部最小化されていたら次のものを復帰させる */
    if (zlist[1]->used) {
        zlist[1]->minimized = 0;
        XMapWindow(dpy, zlist[1]->frame);
        raise_client(zlist[1]);
    }
}

static void frame_client(Window w, int adopt)
{
    if (find_by_client(w)) return;
    if (w == desktop || w == taskbar || w == startmenu) return;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;
    if (adopt && wa.map_state != IsViewable) return;

    Client *c = alloc_client();
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->client = w;

    /* Firefox は画面より大きい大きさを要求してくることがあるので、
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
                                   0, 0, x98.face);
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
    z_remove(c);
    c->used = 0;
    if (focused == c) {
        focused = NULL;
        /* 次に手前だった窓へフォーカスを移す */
        for (int i = 0; i < n_z; i++)
            if (zlist[i]->used && !zlist[i]->minimized) {
                raise_client(zlist[i]);
                break;
            }
    }
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

/* Win+左 / Win+右: 画面の左半分・右半分に寄せる (Windows のスナップ)。
 * side = -1 で左、+1 で右。 */
static void snap_client(Client *c, int side)
{
    if (!c->maximized) {
        c->old_x = c->x; c->old_y = c->y;
        c->old_w = c->w; c->old_h = c->h;
    }
    c->maximized = 0;
    c->w = scr_w / 2;
    c->h = scr_h - TASKBAR_H;
    c->x = (side < 0) ? 0 : scr_w - c->w;
    c->y = 0;
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

/* Win+D: 全部引っ込めてデスクトップを出す。もう一度押すと戻す。
 * 「自分が引っ込めたもの」だけ覚えておいて、それだけ戻す。 */
static int  showing_desktop = 0;
static char hidden_by_showdesk[MAX_CLIENTS];

static void toggle_show_desktop(void)
{
    if (!showing_desktop) {
        int any = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            hidden_by_showdesk[i] = 0;
            if (clients[i].used && !clients[i].minimized) {
                hidden_by_showdesk[i] = 1;
                minimize(&clients[i]);
                any = 1;
            }
        }
        showing_desktop = any;
    } else {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (hidden_by_showdesk[i] && clients[i].used)
                restore(&clients[i]);
            hidden_by_showdesk[i] = 0;
        }
        showing_desktop = 0;
    }
    draw_taskbar();
}

/* ルートウィンドウの矢印カーソル。
 * XCreateFontCursor は libXcursor を dlopen するので、静的リンクした
 * 実行ファイルでは落ちる。動的リンクしている限りは問題ない。 */
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

/* ファイルマネージャがデスクトップにリンクを足したとき、
 * SIGUSR1 で知らせてくる。ハンドラの中では描画せず、フラグだけ立てる。 */
static volatile sig_atomic_t reload_icons = 0;

static void on_sigusr1(int sig)
{
    (void)sig;
    reload_icons = 1;
}

/* Windows でおなじみの操作を拾えるようにする。
 *   Alt+Tab  … 窓の切り替え
 *   Alt+F4   … 手前の窓を閉じる
 *   Win キー … スタートメニュー
 * NumLock / CapsLock が入っていても効くよう、修飾の組み合わせ全部で掴む。 */
static void grab_one(KeySym ks, unsigned base)
{
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    unsigned mods[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    if (!kc) return;
    for (int i = 0; i < 4; i++)
        XGrabKey(dpy, kc, base | mods[i], root, True,
                 GrabModeAsync, GrabModeAsync);
}

static void grab_keys(void)
{
    grab_one(XK_Tab, Mod1Mask);                 /* Alt+Tab */
    grab_one(XK_Tab, Mod1Mask | ShiftMask);     /* Alt+Shift+Tab */
    grab_one(XK_F4,  Mod1Mask);                 /* Alt+F4 */
    grab_one(XK_Super_L, 0);
    grab_one(XK_Super_R, 0);

    /* Windows キーとの組み合わせ。Mod4Mask が Windows キー。 */
    grab_one(XK_e, Mod4Mask);                   /* エクスプローラ */
    grab_one(XK_d, Mod4Mask);                   /* デスクトップの表示 */
    grab_one(XK_Left,  Mod4Mask);               /* 左半分にスナップ */
    grab_one(XK_Right, Mod4Mask);               /* 右半分にスナップ */
    grab_one(XK_Up,    Mod4Mask);               /* 最大化 */
    grab_one(XK_Down,  Mod4Mask);               /* 元に戻す / 最小化 */
}

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(void)
{
    signal(SIGCHLD, SIG_IGN);       /* 子プロセスを自動で回収する */
    signal(SIGUSR1, on_sigusr1);    /* デスクトップのリンク再読み込み */

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-wm] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);
    scr_w  = DisplayWidth(dpy, screen);
    scr_h  = DisplayHeight(dpy, screen);

    x98_init(&x98, dpy, screen);
    if (!x98.font)
        fprintf(stderr, "[myos-wm] no usable font, drawing without text\n");

    a_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_net_wm_name  = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_utf8         = XInternAtom(dpy, "UTF8_STRING", False);

    /* 既に別の WM がいないか確かめる */
    XSetErrorHandler(detect_other_wm);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);
    if (other_wm_running) {
        fprintf(stderr, "[myos-wm] another window manager is already running\n");
        return 1;
    }
    XSetErrorHandler(ignore_errors);

    XSetWindowBackground(dpy, root, x98.desktop);
    XClearWindow(dpy, root);
    set_root_cursor();

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = x98.desktop;

    /* デスクトップ。いちばん下に敷いて、アイコンを描く。
     * ルートウィンドウに直接描くと他のプログラムに消されうるので、
     * 自前のウィンドウを持つほうが確実。 */
    desktop = XCreateWindow(dpy, root, 0, 0, scr_w, scr_h - TASKBAR_H,
                            0, CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel, &swa);
    XSelectInput(dpy, desktop, ExposureMask | ButtonPressMask);
    XMapWindow(dpy, desktop);
    XLowerWindow(dpy, desktop);

    swa.background_pixel = x98.face;
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

    volwin = XCreateWindow(dpy, root, 0, 0, VOL_W, VOL_H, 0, CopyFromParent,
                           InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel, &swa);
    XSelectInput(dpy, volwin, ExposureMask | ButtonPressMask);

    load_icons();
    grab_keys();

    /* 既にいるウィンドウを引き取る (WM が後から起動した場合) */
    Window r2, parent, *kids = NULL;
    unsigned int nkids = 0;
    if (XQueryTree(dpy, root, &r2, &parent, &kids, &nkids)) {
        for (unsigned int i = 0; i < nkids; i++) frame_client(kids[i], 1);
        if (kids) XFree(kids);
    }

    fprintf(stderr, "[myos-wm] running on %dx%d, %d desktop icons\n",
            scr_w, scr_h, n_icons);

    /* --- イベントループ ---
     * X のイベントだけを待つと時計が更新されないので、
     * select() で 1 秒ごとに起き、分が変わったらタスクバーを描き直す。 */
    int dragging = 0, drag_dx = 0, drag_dy = 0;
    Client *drag_c = NULL;
    int resizing = 0, rz_x0 = 0, rz_y0 = 0, rz_w0 = 0, rz_h0 = 0;
    Client *resize_c = NULL;
    long last_title_ms = 0;
    Client *last_title_c = NULL;
    long last_click_ms = 0;
    int  last_click_icon = -1;
    char last_clock[16] = "";
    int  xfd = ConnectionNumber(dpy);

    for (;;) {
        while (!XPending(dpy)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            struct timeval tv = { 1, 0 };
            select(xfd + 1, &fds, NULL, NULL, &tv);

            if (reload_icons) {
                reload_icons = 0;
                /* 設定アプリが色を変えたかもしれないので、
                 * アイコンだけでなくテーマも読み直して塗り直す。 */
                x98_theme_defaults(&x98.theme);
                x98_load_theme(&x98.theme, X98_THEME_CONF);
                x98_apply(&x98);
                load_icons();
                sel_icon = -1;
                XSetWindowBackground(dpy, root, x98.desktop);
                XSetWindowBackground(dpy, desktop, x98.desktop);
                XSetWindowBackground(dpy, taskbar, x98.face);
                XSetWindowBackground(dpy, startmenu, x98.face);
                draw_desktop();
                draw_taskbar();
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (clients[i].used) draw_frame(&clients[i]);
                XFlush(dpy);
            }

            /* 繋がったか切れたかを見る。変わったときだけ描き直す。
             * getifaddrs だけなのでコマンドは起動しない。 */
            int was_up = net_up;
            net_poll();

            char clk[16];
            clock_string(clk, sizeof(clk));
            if (strcmp(clk, last_clock) || was_up != net_up) {
                snprintf(last_clock, sizeof(last_clock), "%s", clk);
                draw_taskbar();
                XFlush(dpy);
            }
        }

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
                XWindowChanges wc;
                wc.x = e->x; wc.y = e->y;
                wc.width = e->width; wc.height = e->height;
                wc.border_width = e->border_width;
                wc.sibling = e->above; wc.stack_mode = e->detail;
                XConfigureWindow(dpy, e->window, e->value_mask, &wc);
            } else if (e->value_mask & (CWWidth | CWHeight)) {
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
            break;
        }

        case UnmapNotify: {
            Client *c = find_by_client(ev.xunmap.window);
            if (c && !c->minimized && ev.xunmap.event != root) unframe(c);
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
            if (ev.xexpose.window == desktop)   { draw_desktop(); break; }
            if (ev.xexpose.window == taskbar)   { draw_taskbar(); break; }
            if (ev.xexpose.window == startmenu) { draw_startmenu(); break; }
            if (ev.xexpose.window == volwin)    { draw_volwin();   break; }
            Client *c = find_by_frame(ev.xexpose.window);
            if (c) draw_frame(c);
            break;
        }

        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            unsigned st = ev.xkey.state;
            Client *f = (focused && focused->used) ? focused : NULL;

            if ((ks == XK_Tab) && (st & Mod1Mask)) {
                cycle_windows(!!(st & ShiftMask));
            } else if ((ks == XK_F4) && (st & Mod1Mask)) {
                if (f) close_client(f);
            } else if (ks == XK_Super_L || ks == XK_Super_R) {
                set_menu(!menu_open);
            } else if (st & Mod4Mask) {
                /* Windows キーとの組み合わせ。
                 * Windows キー単体はもう押した時点でスタートメニューを
                 * 開いてしまっているので、組み合わせが来たら閉じ直す。 */
                set_menu(0);
                switch (ks) {
                case XK_e: case XK_E:
                    spawn("/usr/local/bin/myos-files /root");
                    break;
                case XK_d: case XK_D:
                    toggle_show_desktop();
                    break;
                case XK_Left:  if (f) snap_client(f, -1); break;
                case XK_Right: if (f) snap_client(f, +1); break;
                case XK_Up:    if (f && !f->maximized) toggle_max(f); break;
                case XK_Down:
                    if (f) { if (f->maximized) toggle_max(f);
                             else minimize(f); }
                    break;
                default: break;
                }
            }
            break;
        }

        case ButtonPress: {
            Window w = ev.xbutton.window;

            /* --- デスクトップ: 選択とダブルクリック起動 --- */
            if (w == desktop) {
                set_menu(0);
                int i = icon_hit(ev.xbutton.x, ev.xbutton.y);
                long t = now_ms();
                /* クリックの取りこぼしを追えるよう、素の情報を残しておく */
                fprintf(stderr, "[myos-wm] desktop click btn=%u at %d,%d "
                        "icon=%d dt=%ldms\n", ev.xbutton.button,
                        ev.xbutton.x, ev.xbutton.y, i,
                        (long)(t - last_click_ms));
                fflush(stderr);
                if (i >= 0 && i == last_click_icon &&
                    t - last_click_ms < x98.theme.dblclick_ms) {
                    spawn(icons[i].cmd);
                    last_click_icon = -1;
                } else {
                    last_click_icon = i;
                    last_click_ms = t;
                }
                sel_icon = i;
                draw_desktop();
                break;
            }

            if (w == taskbar) {
                int tr = tray_hit(ev.xbutton.x);
                if (ev.xbutton.x < START_W + 6) {
                    set_volwin(0);
                    set_menu(!menu_open);
                } else if (tr == 1) {           /* 音量 */
                    set_menu(0);
                    set_volwin(!vol_open);
                } else if (tr == 0) {           /* ネットワーク */
                    set_menu(0);
                    set_volwin(0);
                    char msg[160];
                    if (net_up)
                        snprintf(msg, sizeof(msg),
                                 "Connected.\nAddress: %s", net_addr);
                    else
                        snprintf(msg, sizeof(msg),
                                 "Not connected.\n"
                                 "No network cable or adapter was found.");
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd),
                             "myos-alert -info 'Network' '%s' &", msg);
                    if (system(cmd) == -1) { /* 出せないだけなので黙る */ }
                } else {
                    int i = taskbtn_hit(ev.xbutton.x);
                    if (i >= 0) {
                        Client *c = &clients[i];
                        if (c->minimized) restore(c);
                        else raise_client(c);
                    }
                    set_menu(0);
                    set_volwin(0);
                }
                break;
            }

            if (w == volwin) {
                volwin_click(ev.xbutton.x, ev.xbutton.y);
                break;
            }

            if (w == startmenu) {
                int i = (ev.xbutton.y - 4) / MENU_ITEM_H;
                set_menu(0);
                if (i >= 0 && i < n_menu) spawn(menu[i].cmd);
                break;
            }

            Client *c = find_by_frame(w);
            if (!c) { set_menu(0); set_volwin(0); break; }

            raise_client(c);
            set_menu(0);
            set_volwin(0);

            int mx = ev.xbutton.x, my = ev.xbutton.y;
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

            if (my >= BORDER && my < BORDER + TITLE_H) {
                /* タイトルバーのダブルクリックで最大化 / 元に戻す */
                long t = now_ms();
                if (c == last_title_c && t - last_title_ms < x98.theme.dblclick_ms) {
                    last_title_c = NULL;
                    toggle_max(c);
                    break;
                }
                last_title_c = c;
                last_title_ms = t;
            }

            if (my >= BORDER && my < BORDER + TITLE_H && !c->maximized) {
                dragging = 1;
                drag_c = c;
                drag_dx = ev.xbutton.x_root - c->x;
                drag_dy = ev.xbutton.y_root - c->y;
                break;
            }

            /* 枠の右端 / 下端 / 右下を掴んだらリサイズ */
            if (!c->maximized) {
                int zone = 0;
                if (mx >= c->w - RESIZE_GRIP - 8) zone |= 1;   /* 右 */
                if (my >= c->h - RESIZE_GRIP - 8) zone |= 2;   /* 下 */
                if (zone) {
                    resizing = zone;
                    resize_c = c;
                    rz_x0 = ev.xbutton.x_root;
                    rz_y0 = ev.xbutton.y_root;
                    rz_w0 = c->w;
                    rz_h0 = c->h;
                }
            }
            break;
        }

        case MotionNotify: {
            if (resizing && resize_c) {
                while (XCheckTypedEvent(dpy, MotionNotify, &ev)) { }
                Client *c = resize_c;
                int nw = c->w, nh = c->h;
                if (resizing & 1) nw = rz_w0 + (ev.xmotion.x_root - rz_x0);
                if (resizing & 2) nh = rz_h0 + (ev.xmotion.y_root - rz_y0);
                if (nw < MIN_W) nw = MIN_W;
                if (nh < MIN_H) nh = MIN_H;
                if (c->x + nw > scr_w) nw = scr_w - c->x;
                if (c->y + nh > scr_h - TASKBAR_H)
                    nh = scr_h - TASKBAR_H - c->y;
                c->w = nw;
                c->h = nh;
                c->cw = nw - 2 * BORDER;
                c->ch = nh - BORDER - TITLE_H - BORDER;
                set_geometry(c);
                break;
            }
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
            resizing = 0;
            resize_c = NULL;
            break;

        default:
            break;
        }
    }
    return 0;
}

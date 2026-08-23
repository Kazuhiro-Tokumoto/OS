/* ==========================================================================
 * myos_openwith.c  -  「アプリケーションから開く」(Win98 の Open With 相当)
 *
 *   myos-openwith <パス>
 *
 * 関連付けの表に無い拡張子を開こうとしたときに出る。myos-open が呼ぶ。
 *
 * なぜ要るか
 * ----------
 * 表に無いものは firefox-esr に投げていた。ところが閲覧ソフトは
 * 1.5.2 から**ディスクに入っていない** (使う人が選んで後から入れる)。
 * つまり大半の機械では、知らない拡張子をダブルクリックすると
 * **何も起きない**。実機で /var/log/Xorg.0.log.old が開けなかったのが
 * これ。ログを見ようとして詰むのは、いちばん困る場面で詰んでいる。
 *
 * Windows は知らない拡張子でこの窓を出す。同じ形にする。何で開くかを
 * その場で選べて、必要なら次から覚える。**開けないより、選べるほうが
 * いい**という考え方は、音の出口を隠して詰んだときと同じ。
 *
 * 一覧に無いものも指せる。Windows の Open With に「参照」があるのと同じで、
 * **こちらが並べたものしか選べない窓は、Windows と同じではない**。
 * myOS にはファイル選択の窓がまだ無いので、代わりに名前を打つ欄にした。
 * apt で入れたものも、自分で置いたものも、これで指せる。
 *
 * 「次からこれで開く」を入れると ~/.myos/filetypes.conf に 1 行足す。
 * システムの表 (/etc/myos) は触らない。書けるのは自分のぶんだけ、
 * という myOS の決めのとおり。
 *
 * ビルド:
 *   gcc -O2 -o myos-openwith myos_openwith.c -lX11 -lXft
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "x98.h"
#include "filetypes.h"

#define WIN_W   400
#define BTN_W   80
#define BTN_H   24
#define PAD     14
#define ROW_H   20
#define LIST_Y  92
#define N_PROG  ((int)(sizeof(progs) / sizeof(progs[0])))

/* 選べるもの。myOS に必ず入っているものだけを並べる。
 * 入っていないものを並べると、選んだのに何も起きない窓になる。 */
static const struct {
    const char *label;   /* 画面に出す名前 */
    const char *bin;     /* 実体。無ければ一覧に出さない */
    const char *cmd;     /* filetypes.conf に書く形 (%1 がパス) */
} progs[] = {
    { "Notepad",         "/usr/local/bin/myos-notepad",
                         "myos-notepad %1" },
    { "Image Viewer",    "/usr/local/bin/myos-image",
                         "myos-image %1" },
    { "MS-DOS Prompt",   "/usr/local/bin/myos-term",
                         "myos-term less %1" },
    { "Web Browser",     "/usr/local/bin/myos-browser",
                         "myos-browser %1" },
    { "Java",            "/usr/local/bin/myos-java",
                         "myos-java -jar %1" },
};

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static XIC      ic;
static Atom     a_wm_delete;

static int   win_h = 260;         /* 一覧の数で決まる。空白を余らせない */

static const char *path = "";
static char  base[PATH_MAX];      /* 表示用のファイル名 */
static char  ext[32];             /* 小文字の拡張子。無ければ空 */

static int  avail[N_PROG];        /* 実体があるものの添字 */
static int  n_avail = 0;
static int  sel = 0;
static int  remember = 0;
/* 0=一覧 1=名前を打つ欄 2=チェック 3=OK 4=Cancel */
static int  focus = 0;
#define N_FOCUS 5

static X98Edit other;             /* 一覧に無いものを指すとき */
static char    err[128] = "";

/* --- 画面 ---------------------------------------------------------------- */
static void focus_rect(int x, int y, int w, int h)
{
    XSetForeground(dpy, x98.gc, x98.text);
    XSetLineAttributes(dpy, x98.gc, 1, LineOnOffDash, CapButt, JoinMiter);
    XDrawRectangle(dpy, win, x98.gc, x, y, w, h);
    XSetLineAttributes(dpy, x98.gc, 1, LineSolid, CapButt, JoinMiter);
}

static void redraw(void)
{
    char b[PATH_MAX + 64];

    x98_fill(&x98, win, 0, 0, WIN_W, win_h, x98.face);

    x98_text(&x98, win, PAD, PAD,
             "Click the program you want to use to open", x98.text);
    snprintf(b, sizeof(b), "\"%.200s\"", base);
    x98_text(&x98, win, PAD, PAD + 20, b, x98.text);
    x98_text(&x98, win, PAD, PAD + 44,
             "myOS does not know this kind of file yet.", x98.shadow);

    /* 一覧 */
    int lh = n_avail * ROW_H + 8;
    x98_bevel(&x98, win, PAD, LIST_Y, WIN_W - 2 * PAD, lh, 1);
    x98_fill(&x98, win, PAD + 2, LIST_Y + 2, WIN_W - 2 * PAD - 4, lh - 4,
             x98.white);
    for (int i = 0; i < n_avail; i++) {
        int ry = LIST_Y + 4 + i * ROW_H;
        if (i == sel) {
            x98_fill(&x98, win, PAD + 3, ry, WIN_W - 2 * PAD - 6, ROW_H,
                     x98.select_bg);
            if (focus == 0)
                focus_rect(PAD + 3, ry, WIN_W - 2 * PAD - 7, ROW_H - 1);
        }
        x98_text(&x98, win, PAD + 10, ry + (ROW_H - x98_text_h(&x98)) / 2,
                 progs[avail[i]].label,
                 i == sel ? x98.white : x98.text);
    }

    /* 一覧に無いものを指す欄。Windows の「参照」にあたる。 */
    int oy = LIST_Y + lh + 12;
    x98_text(&x98, win, PAD, oy, "Or type another program:", x98.text);
    x98_edit_draw(&x98, win, PAD, oy + 18, WIN_W - 2 * PAD, 20,
                  &other, focus == 1);

    /* 「次からこれで開く」。拡張子が無いものは覚えようが無いので出さない。 */
    int cy = oy + 46;
    if (ext[0]) {
        x98_bevel(&x98, win, PAD, cy, 13, 13, 0);
        x98_fill(&x98, win, PAD + 2, cy + 2, 9, 9, x98.white);
        if (remember) {
            XSetForeground(dpy, x98.gc, x98.text);
            XDrawLine(dpy, win, x98.gc, PAD + 3, cy + 6, PAD + 5, cy + 9);
            XDrawLine(dpy, win, x98.gc, PAD + 5, cy + 9, PAD + 10, cy + 3);
        }
        snprintf(b, sizeof(b), "Always use this program for .%s files", ext);
        x98_text(&x98, win, PAD + 20, cy + (13 - x98_text_h(&x98)) / 2 - 1,
                 b, x98.text);
        if (focus == 2)
            focus_rect(PAD + 17, cy - 2, x98_text_w(&x98, b) + 6, 17);
    }

    if (err[0]) x98_text(&x98, win, PAD, cy + 22, err, x98.text);

    int by = win_h - BTN_H - PAD;
    int ox = WIN_W - 2 * BTN_W - PAD - 8;
    x98_button(&x98, win, ox, by, BTN_W, BTN_H, "OK", 0);
    if (focus == 3) focus_rect(ox + 4, by + 4, BTN_W - 8, BTN_H - 8);
    x98_button(&x98, win, WIN_W - BTN_W - PAD, by, BTN_W, BTN_H, "Cancel", 0);
    if (focus == 4)
        focus_rect(WIN_W - BTN_W - PAD + 4, by + 4, BTN_W - 8, BTN_H - 8);
}

/* --- 決まったあと -------------------------------------------------------- */

/* 次からこれで開く、を覚える。ユーザーの表に 1 行足すだけ。
 * ft_load はユーザーの分を先に読むので、これが既定より優先される。 */
static void remember_choice(const char *cmd)
{
    if (!ext[0]) return;

    char path_conf[512];
    /* myos_user_conf が ~/.myos を作ってからパスを返す。 */
    myos_user_conf(path_conf, sizeof(path_conf), "filetypes.conf");

    FILE *f = fopen(path_conf, "a");
    if (!f) return;
    fprintf(f, "%s|%s File|file|%s|\n", ext, ext, cmd);
    fclose(f);
}

/* その名前のプログラムが実際にあるか。
 * 無いものを exec しても、窓が消えて何も起きないだけになる。
 * それは今回直している症状そのものなので、先に見る。 */
static int have_program(const char *name)
{
    if (strchr(name, '/')) return access(name, X_OK) == 0;

    const char *p = getenv("PATH");
    if (!p || !p[0]) p = "/usr/local/bin:/usr/bin:/bin";
    while (*p) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char full[PATH_MAX];
        if (n > 0 && n < sizeof(full) - strlen(name) - 2) {
            snprintf(full, sizeof(full), "%.*s/%s", (int)n, p, name);
            if (access(full, X_OK) == 0) return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* 開く。打った名前があればそちらが勝つ (Windows の「参照」にあたる)。
 * 戻り値 0 = 窓を閉じてよい。1 = 開けなかったので開いたまま直させる。 */
static int launch(void)
{
    char user_cmd[FT_CMD];
    const char *cmd;

    char *typed = myos_trim(other.buf);
    if (typed[0]) {
        /* 打った名前をそのまま argv[0] にする。%1 は自分で足す。
         * 引数まで打たれていたら、そこへパスを付け足す形にする。 */
        if (strstr(typed, "%1"))
            snprintf(user_cmd, sizeof(user_cmd), "%s", typed);
        else
            snprintf(user_cmd, sizeof(user_cmd), "%s %%1", typed);

        char first[PATH_MAX];
        snprintf(first, sizeof(first), "%s", typed);
        char *sp = strchr(first, ' ');
        if (sp) *sp = 0;

        if (!have_program(first)) {
            snprintf(err, sizeof(err), "\"%.60s\" was not found.", first);
            return 1;
        }
        cmd = user_cmd;
    } else {
        cmd = progs[avail[sel]].cmd;
    }

    if (remember) remember_choice(cmd);

    char store[FT_CMD + PATH_MAX + 64];
    char *av[FT_ARGV];
    int na = ft_argv(cmd, path, av, FT_ARGV, store, sizeof(store));
    if (na <= 0) {
        snprintf(err, sizeof(err), "That command could not be used.");
        return 1;
    }

    XCloseDisplay(dpy);
    execvp(av[0], av);
    _exit(127);
}

/* --- 本体 ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: myos-openwith <path>\n");
        return 2;
    }
    path = argv[1];

    const char *b = strrchr(path, '/');
    snprintf(base, sizeof(base), "%s", b ? b + 1 : path);

    /* 拡張子。先頭のドットしか無いもの (.bashrc) は拡張子とみなさない。 */
    ext[0] = 0;
    const char *dot = strrchr(base, '.');
    if (dot && dot != base && dot[1]) {
        snprintf(ext, sizeof(ext), "%s", dot + 1);
        for (char *p = ext; *p; p++) *p = (char)tolower((unsigned char)*p);
    }

    /* 実体のあるものだけを一覧に出す */
    for (int i = 0; i < N_PROG; i++)
        if (access(progs[i].bin, X_OK) == 0) avail[n_avail++] = i;
    if (n_avail == 0) {
        fprintf(stderr, "myos-openwith: no program to open with\n");
        return 1;
    }

    /* 窓の背は中身で決める。決め打ちにすると、一覧が短いときに
     * 下が間延びして「まだ何かあるのか」と読まれる。 */
    win_h = LIST_Y + (n_avail * ROW_H + 8)
            + 12 + 18 + 20            /* 「別のプログラム」の見出しと欄 */
            + 8  + (ext[0] ? 20 : 0)  /* チェック */
            + 26 + BTN_H + PAD;       /* エラー行のぶんを空けておく */

    x98_edit_set(&other, "");

    x98_im_setup_locale();
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-openwith: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, win_h, 0, x98.shadow, x98.face);
    XStoreName(dpy, win, "Open With");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = WIN_W;
    hints.min_height = hints.max_height = win_h;
    XSetWMNormalHints(dpy, win, &hints);

    XMapRaised(dpy, win);
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (XFilterEvent(&ev, win)) continue;

        switch (ev.type) {
        case Expose:
            redraw();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a_wm_delete) goto done;
            break;

        case KeyPress: {
            KeySym ks = 0;
            char kb[32];
            int kn = x98_lookup(ic, &ev.xkey, kb, sizeof(kb), &ks);

            err[0] = 0;
            if (ks == XK_Escape) goto done;
            else if (ks == XK_Return || ks == XK_KP_Enter) {
                if (focus == 4) goto done;
                else if (focus == 2) remember = !remember;
                else if (launch() == 0) goto done;
            } else if (ks == XK_Tab) {
                /* 拡張子が無いときはチェックを出していないので飛ばす。
                 * 見えないところにフォーカスが行くと、押しても何も
                 * 起きない場所で止まったように見える。 */
                int back = (ev.xkey.state & ShiftMask) ? -1 : 1;
                do {
                    focus = (focus + back + N_FOCUS) % N_FOCUS;
                } while (!ext[0] && focus == 2);
            } else if (ks == XK_space && focus != 1) {
                if (focus == 2)      remember = !remember;
                else if (focus == 3) { if (launch() == 0) goto done; }
                else if (focus == 4) goto done;
            } else if (ks == XK_Up || ks == XK_Down) {
                if (focus == 0) {
                    sel += (ks == XK_Down) ? 1 : -1;
                    if (sel < 0) sel = 0;
                    if (sel >= n_avail) sel = n_avail - 1;
                }
            } else if (ks == XK_BackSpace) {
                if (focus == 1) x98_edit_backspace(&other);
            } else if (focus == 1 && kn > 0 &&
                       (unsigned char)kb[0] >= 0x20) {
                kb[kn] = 0;
                x98_edit_insert(&other, kb, kn);
            }
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;
            int lh = n_avail * ROW_H + 8;

            for (int i = 0; i < n_avail; i++) {
                int ry = LIST_Y + 4 + i * ROW_H;
                if (my >= ry && my < ry + ROW_H &&
                    mx >= PAD && mx < WIN_W - PAD) {
                    sel = i;
                    focus = 0;
                }
            }

            int oy = LIST_Y + lh + 12;
            if (my >= oy + 18 && my < oy + 38 &&
                mx >= PAD && mx < WIN_W - PAD)
                focus = 1;

            int cy = oy + 46;
            if (ext[0] && my >= cy - 2 && my < cy + 15 &&
                mx >= PAD && mx < WIN_W - PAD) {
                remember = !remember;
                focus = 2;
            }

            int by = win_h - BTN_H - PAD;
            int ox = WIN_W - 2 * BTN_W - PAD - 8;
            if (my >= by && my < by + BTN_H) {
                if (mx >= ox && mx < ox + BTN_W) {
                    err[0] = 0;
                    if (launch() == 0) goto done;
                }
                if (mx >= WIN_W - BTN_W - PAD && mx < WIN_W - PAD) goto done;
            }
            redraw();
            break;
        }
        }
    }

done:
    XCloseDisplay(dpy);
    return 0;
}

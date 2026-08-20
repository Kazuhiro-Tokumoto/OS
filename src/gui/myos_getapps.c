/* ==========================================================================
 * myos_getapps.c  -  プログラムの追加と削除 (Windows 98 のあれ)
 *
 * 1.5.2 から、閲覧ソフトはインストールディスクに載せない。
 * 理由は 2 つある。
 *
 *   1. 場所。Firefox は squashfs に潰しても 84MiB あり、ISO の余裕
 *      36MiB を 1 つで食い潰していた。外すと 667 -> 583MiB になる。
 *   2. 選べること。1GB の機械に Firefox は重い。機械に合ったものを
 *      あとから選べたほうがいい (落としてくること自体はメモリを
 *      減らさない。減るのは "選べる" から)。
 *
 * 網が無ければ apt も閲覧ソフトも使えないので、「網に繋がってから
 * 落とす」で困る人はいない、という判断。
 *
 * root が要る操作は /usr/local/bin/myos-pkg に寄せてあり、あちらは
 * カタログ (/etc/myos/catalog.conf) に載っている id しか受け付けない。
 * ここから任意のパッケージ名を渡すことはできない。
 *
 * 入れ終わったらデスクトップのアイコンとスタートメニューに出す。
 * スタートメニューのほうは myos-refresh-menu が .desktop を拾うので
 * 次のログオンで勝手に出るが、それでは遅い。その場で反映する。
 *
 * ビルド:
 *   gcc -O2 -o myos-getapps myos_getapps.c -lX11 -lXft
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "x98.h"

#define WIN_W   560
#define WIN_H   440
#define PAD     12
#define LIST_X  PAD
#define LIST_Y  74
#define LIST_W  (WIN_W - PAD * 2)
#define LIST_H  244
#define ROW_H   34
#define BTN_W   104
#define BTN_H   24
#define SB_W    16

#define MAX_ITEM 32
#define CATALOG  "/etc/myos/catalog.conf"

typedef struct {
    char kind[16];      /* browser / extra */
    char id[32];
    char name[64];
    char pkgs[160];
    int  mb;
    char label[48];     /* デスクトップに出す名前 (空なら出さない) */
    char icon[16];
    char cmd[96];
    char desc[128];
    int  installed;
    int  picked;
} Item;

static Item items[MAX_ITEM];
static int  n_items;
static int  sel = 0, top = 0;

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static XIC      ic;
static Pixmap   cv;

static char status[200] = "";
static int  busy = 0;           /* 作業中は触らせない */
static int  need_browser = 0;   /* 閲覧ソフトが無くて呼ばれた */

/* 作業の出力を出す窓。apt は長いので、最後の数行だけ見せる。 */
#define LOG_LINES 8
static char logbuf[LOG_LINES][160];
static int  n_log;

static void logline(const char *s)
{
    if (n_log < LOG_LINES) {
        snprintf(logbuf[n_log++], sizeof(logbuf[0]), "%s", s);
    } else {
        memmove(logbuf[0], logbuf[1], sizeof(logbuf[0]) * (LOG_LINES - 1));
        snprintf(logbuf[LOG_LINES - 1], sizeof(logbuf[0]), "%s", s);
    }
}

/* --- カタログ ------------------------------------------------------------ */

static char *field(char **p)
{
    char *s = *p;
    char *bar = strchr(s, '|');
    if (bar) { *bar = 0; *p = bar + 1; }
    else     { *p = s + strlen(s); }
    return s;
}

static void load_catalog(void)
{
    n_items = 0;
    FILE *f = fopen(CATALOG, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && n_items < MAX_ITEM) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (line[0] == '#' || !line[0]) continue;

        char *p = line;
        Item *it = &items[n_items];
        memset(it, 0, sizeof(*it));
        snprintf(it->kind,  sizeof(it->kind),  "%s", field(&p));
        snprintf(it->id,    sizeof(it->id),    "%s", field(&p));
        snprintf(it->name,  sizeof(it->name),  "%s", field(&p));
        snprintf(it->pkgs,  sizeof(it->pkgs),  "%s", field(&p));
        it->mb = atoi(field(&p));
        snprintf(it->label, sizeof(it->label), "%s", field(&p));
        snprintf(it->icon,  sizeof(it->icon),  "%s", field(&p));
        snprintf(it->cmd,   sizeof(it->cmd),   "%s", field(&p));
        snprintf(it->desc,  sizeof(it->desc),  "%s", field(&p));
        if (!it->id[0]) continue;
        n_items++;
    }
    fclose(f);
}

/* --- 外部コマンド -------------------------------------------------------- */

static int run(char *const argv[])
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); close(null); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void refresh_installed(void)
{
    for (int i = 0; i < n_items; i++) {
        char *av[] = { "sudo", "-n", "/usr/local/bin/myos-pkg",
                       "installed", items[i].id, NULL };
        items[i].installed = (run(av) == 0);
    }
}

static void redraw(void);

/* コマンドを走らせ、出た行を画面へ流す。
 * apt は数分かかるので、終わるまで黙っていると固まったように見える。 */
static int run_streaming(char *const argv[])
{
    int fd[2];
    if (pipe(fd) < 0) return -1;
    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (p == 0) {
        close(fd[0]);
        dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[1]);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) { dup2(null, 0); close(null); }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);

    FILE *f = fdopen(fd[0], "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (line[0]) { logline(line); redraw(); XFlush(dpy); }
        }
        fclose(f);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* --- 入れたあとの後始末 --------------------------------------------------
 * デスクトップに出す。スタートメニューは myos-refresh-menu が
 * .desktop から拾うので、そちらを走らせるだけでよい。
 * ウィンドウマネージャは SIGUSR1 でアイコンを読み直す。 */
static void add_shortcuts(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char dir[256], path[320];
    snprintf(dir, sizeof(dir), "%s/.myos", home);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/.myos/desktop.conf", home);

    /* 既にある行は足さない。何度入れ直しても増えないように。 */
    for (int i = 0; i < n_items; i++) {
        Item *it = &items[i];
        if (!it->picked || !it->installed || !it->label[0]) continue;

        int dup = 0;
        FILE *r = fopen(path, "r");
        if (r) {
            char line[256];
            while (fgets(line, sizeof(line), r))
                if (strstr(line, it->cmd)) { dup = 1; break; }
            fclose(r);
        } else {
            /* 個人用がまだ無ければ、システムの並びを写してから足す */
            FILE *sys = fopen("/etc/myos/desktop.conf", "r");
            FILE *w = fopen(path, "w");
            if (sys && w) {
                char line[256];
                while (fgets(line, sizeof(line), sys)) fputs(line, w);
            }
            if (sys) fclose(sys);
            if (w) fclose(w);
        }
        if (dup) continue;

        FILE *a = fopen(path, "a");
        if (a) {
            fprintf(a, "%s|%s|%s\n", it->label, it->icon, it->cmd);
            fclose(a);
        }
    }

    char *rm[] = { "/usr/local/bin/myos-refresh-menu", NULL };
    run(rm);
    /* -x で名前が完全一致するものだけ。-f は自分の引数にも当たる。 */
    char *sig[] = { "pkill", "-USR1", "-x", "myos-wm", NULL };
    run(sig);
}

/* --- 入れる -------------------------------------------------------------- */

static void do_install(void)
{
    int n = 0;
    for (int i = 0; i < n_items; i++)
        if (items[i].picked && !items[i].installed) n++;
    if (!n) {
        snprintf(status, sizeof(status), "Nothing selected.");
        return;
    }

    busy = 1;
    n_log = 0;
    snprintf(status, sizeof(status), "Getting the package list...");
    redraw(); XFlush(dpy);

    char *up[] = { "sudo", "-n", "/usr/local/bin/myos-pkg", "update", NULL };
    if (run_streaming(up) != 0) {
        snprintf(status, sizeof(status),
                 "Could not reach the network. Connect first, then try again.");
        busy = 0;
        return;
    }

    /* 選んだものを 1 回で渡す。1 つずつだと apt を何度も起こすことになる。 */
    char *av[8 + MAX_ITEM];
    int k = 0;
    av[k++] = "sudo"; av[k++] = "-n";
    av[k++] = "/usr/local/bin/myos-pkg"; av[k++] = "install";
    for (int i = 0; i < n_items && k < 7 + MAX_ITEM; i++)
        if (items[i].picked && !items[i].installed) av[k++] = items[i].id;
    av[k] = NULL;

    snprintf(status, sizeof(status), "Installing... this can take a while.");
    redraw(); XFlush(dpy);

    int rc = run_streaming(av);
    refresh_installed();

    if (rc == 0) {
        add_shortcuts();
        snprintf(status, sizeof(status), "Done. New programs are on the desktop.");
    } else {
        snprintf(status, sizeof(status),
                 "Setup could not install everything (exit %d).", rc);
    }
    busy = 0;
}

/* --- 描画 ---------------------------------------------------------------- */

static void draw_check(int px, int py, int on, int dim)
{
    x98_bevel(&x98, cv, px, py, 13, 13, 0);
    x98_fill(&x98, cv, px + 2, py + 2, 9, 9,
             dim ? x98.face : x98.white);
    if (on) {
        /* 98 のチェックは折れ線。四角い点で近似する。 */
        x98_fill(&x98, cv, px + 4, py + 7, 2, 2, x98.text);
        x98_fill(&x98, cv, px + 6, py + 8, 2, 2, x98.text);
        x98_fill(&x98, cv, px + 8, py + 4, 2, 4, x98.text);
    }
}

static void draw_scrollbar(int n, int vis, int topv)
{
    if (n <= vis) return;
    int x = LIST_X + LIST_W - 2 - SB_W;
    int y = LIST_Y + 2, h = LIST_H - 4;
    x98_fill(&x98, cv, x, y, SB_W, h, x98.face);
    x98_button(&x98, cv, x, y, SB_W, SB_W, "", 0);
    x98_button(&x98, cv, x, y + h - SB_W, SB_W, SB_W, "", 0);
    int track = h - SB_W * 2;
    int th = track * vis / n;  if (th < 12) th = 12;
    int maxtop = n - vis;
    int ty = y + SB_W + (maxtop > 0 ? (track - th) * topv / maxtop : 0);
    x98_button(&x98, cv, x, ty, SB_W, th, "", 0);
}

static void draw_list(void)
{
    x98_bevel(&x98, cv, LIST_X, LIST_Y, LIST_W, LIST_H, 0);
    x98_fill(&x98, cv, LIST_X + 2, LIST_Y + 2, LIST_W - 4, LIST_H - 4, x98.white);

    int vis = (LIST_H - 4) / ROW_H;
    draw_scrollbar(n_items, vis, top);
    int w = LIST_W - 4 - (n_items > vis ? SB_W : 0);

    for (int r = 0; r < vis && top + r < n_items; r++) {
        int i = top + r;
        Item *it = &items[i];
        int y = LIST_Y + 2 + r * ROW_H;
        unsigned long fg = x98.text, sub = x98.shadow;

        if (i == sel) {
            x98_fill(&x98, cv, LIST_X + 2, y, w, ROW_H, x98.select_bg);
            fg = x98.white; sub = x98.white;
        }
        draw_check(LIST_X + 10, y + 5, it->installed || it->picked,
                   it->installed);

        char head[128];
        if (it->installed)
            snprintf(head, sizeof(head), "%s  (already installed)", it->name);
        else
            snprintf(head, sizeof(head), "%s  %d MB", it->name, it->mb);
        x98_text(&x98, cv, LIST_X + 32, y + 3, head, fg);
        x98_text(&x98, cv, LIST_X + 32, y + 17, it->desc, sub);
    }
}

static void redraw(void)
{
    x98_fill(&x98, cv, 0, 0, WIN_W, WIN_H, x98.face);

    x98_text(&x98, cv, PAD, 12,
             need_browser ? "There is no web browser installed yet."
                          : "Add programs to myOS", x98.text);
    x98_text(&x98, cv, PAD, 30,
             "These are fetched from the internet, so connect first.",
             x98.shadow);
    x98_text(&x98, cv, PAD, 48,
             "Space selects.  The disc does not carry them, to keep it small.",
             x98.shadow);

    draw_list();

    int y = LIST_Y + LIST_H + 6;
    if (busy || n_log) {
        x98_bevel(&x98, cv, PAD, y, WIN_W - PAD * 2, 66, 0);
        x98_fill(&x98, cv, PAD + 2, y + 2, WIN_W - PAD * 2 - 4, 62, x98.white);
        for (int i = 0; i < n_log; i++) {
            char cut[96];
            snprintf(cut, sizeof(cut), "%s", logbuf[i]);
            x98_text(&x98, cv, PAD + 6, y + 4 + i * 7, cut, x98.shadow);
        }
    }

    int total = 0;
    for (int i = 0; i < n_items; i++)
        if (items[i].picked && !items[i].installed) total += items[i].mb;
    char sum[160];
    if (status[0]) snprintf(sum, sizeof(sum), "%s", status);
    else if (total) snprintf(sum, sizeof(sum), "%d MB will be downloaded.", total);
    else snprintf(sum, sizeof(sum), "Nothing selected.");
    x98_text(&x98, cv, PAD, WIN_H - BTN_H - PAD - 18, sum, x98.text);

    int by = WIN_H - BTN_H - PAD;
    x98_button(&x98, cv, WIN_W - 2 * BTN_W - 20, by, BTN_W, BTN_H,
               busy ? "Working..." : "Install", 0);
    x98_button(&x98, cv, WIN_W - BTN_W - PAD, by, BTN_W, BTN_H,
               busy ? "" : "Close", 0);

    XCopyArea(dpy, cv, win, x98.gc, 0, 0, WIN_W, WIN_H, 0, 0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--need-browser")) need_browser = 1;

    x98_im_setup_locale();
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-getapps: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    load_catalog();
    if (!n_items) {
        fprintf(stderr, "myos-getapps: %s が読めません\n", CATALOG);
        return 1;
    }
    refresh_installed();

    /* 閲覧ソフトが無くて呼ばれたときは、最初の 1 つに印を付けておく。
     * 何も選ばれていない画面を出されても、何をすればいいか分からない。 */
    if (need_browser)
        for (int i = 0; i < n_items; i++)
            if (!strcmp(items[i].kind, "browser") && !items[i].installed) {
                items[i].picked = 1; sel = i; break;
            }

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XStoreName(dpy, win, "Add Programs");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &del, 1);
    XMapWindow(dpy, win);
    cv = XCreatePixmap(dpy, win, WIN_W, WIN_H, DefaultDepth(dpy, screen));
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            redraw();
            break;

        case ClientMessage:
            if (!busy && (Atom)ev.xclient.data.l[0] == del) goto done;
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);
            if (busy) break;

            if (ks == XK_Escape) goto done;
            else if (ks == XK_Up && sel > 0) sel--;
            else if (ks == XK_Down && sel < n_items - 1) sel++;
            else if ((n > 0 && buf[0] == ' ') || ks == XK_space) {
                if (!items[sel].installed)
                    items[sel].picked = !items[sel].picked;
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                status[0] = 0;
                do_install();
            }
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
            if (busy) break;
            int by = WIN_H - BTN_H - PAD;
            if (my >= by && my < by + BTN_H) {
                if (mx >= WIN_W - 2 * BTN_W - 20 && mx < WIN_W - BTN_W - 20) {
                    status[0] = 0;
                    do_install();
                } else if (mx >= WIN_W - BTN_W - PAD) {
                    goto done;
                }
            } else if (mx >= LIST_X && mx < LIST_X + LIST_W &&
                       my >= LIST_Y + 2 && my < LIST_Y + LIST_H - 2) {
                int i = top + (my - LIST_Y - 2) / ROW_H;
                if (i >= 0 && i < n_items) {
                    sel = i;
                    if (!items[i].installed) items[i].picked = !items[i].picked;
                }
            }
            redraw();
            break;
        }
        }
    }

done:
    XFreePixmap(dpy, cv);
    XCloseDisplay(dpy);
    return 0;
}

/* ==========================================================================
 * myos_taskman.c  -  タスクマネージャ (Windows 98 の Ctrl+Alt+Del)
 *
 * Windows 98 で Ctrl+Alt+Del を押すと出てくる「プログラムの強制終了」と、
 * NT 系のタスクマネージャを足して 2 で割ったもの。
 *
 *   Applications … 開いている窓の一覧。Windows と同じで
 *                  まず WM_DELETE_WINDOW で「閉じてくれ」と頼み、
 *                  効かなければ XKillClient で切る。
 *   Processes    … /proc から見えるプロセスの一覧。
 *                  CPU の割合、常駐メモリ、持ち主。
 *                  終了は SIGTERM、それでも残れば SIGKILL。
 *
 * 窓の一覧は _NET_CLIENT_LIST を使わない。自作のウィンドウマネージャは
 * その約束事を実装していないので、root の子を辿って WM_STATE の付いた
 * 窓を探す。ウィンドウマネージャが窓を枠で包み直している (reparent) ので、
 * root の子は枠のほうであり、中身はその下にいる。
 *
 * CPU の割合は「前に見たときとの差」で出す。起動直後の 1 回目は
 * 比べる相手がいないので 0 になる。それが正しい (累計を割合として
 * 出すと、ずっと動いているプロセスが常に上位に居座って役に立たない)。
 *
 * ビルド:
 *   gcc -O2 -o myos-taskman myos_taskman.c -lX11 -lXft
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "x98.h"

#define WIN_W   520
#define WIN_H   420
#define PAD     12
#define TAB_H   26
#define LIST_X  PAD
#define LIST_Y  (TAB_H + 34)
#define LIST_W  (WIN_W - PAD * 2)
#define LIST_H  262
#define ROW_H   18
#define BTN_W   104
#define BTN_H   24

#define MAX_APP  64
#define MAX_PROC 512

enum { TAB_APPS = 0, TAB_PROCS, N_TABS };
static const char *tab_name[N_TABS] = { "Applications", "Processes" };

typedef struct {
    Window win;
    char   title[128];
    long   pid;             /* -1 = 分からない */
    int    responding;      /* WM_DELETE_WINDOW を受け付けるか */
} App;

typedef struct {
    long  pid;
    char  name[64];
    char  user[32];
    long  rss_kb;
    unsigned long long cpu;     /* utime + stime (ティック) */
    double pct;                 /* 前回との差から出した割合 */
} Proc;

static App   apps[MAX_APP];
static int   n_apps;
static Proc  procs[MAX_PROC];
static int   n_procs;

/* 前回の測定。CPU の割合を出すのに要る。 */
static Proc  prev[MAX_PROC];
static int   n_prev;
static unsigned long long prev_total;

static int   tab = TAB_APPS;
static int   sel[N_TABS], top[N_TABS];
static char  msg[160];

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static XIC      ic;

static Atom a_wm_state, a_wm_protocols, a_wm_delete, a_net_name, a_net_pid,
            a_utf8;

/* 他人の窓を触るので、消えた窓に当たるのは日常茶飯事。
 * Xlib の既定のハンドラは BadWindow で exit(1) してしまうため、
 * 一覧を作り直した瞬間に相手が消えているとタスクマネージャごと落ちる
 * (End Task の直後に必ず起きた)。ここは黙って見送るのが正しい。 */
static int ignore_x_error(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    return 0;
}

/* --- 小道具 -------------------------------------------------------------- */

static int rd_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) { buf[0] = 0; return 0; }
    buf[r] = 0;
    return 1;
}

/* --- 窓を数える ----------------------------------------------------------
 * WM_STATE が付いている窓が「管理されている窓」。
 * ウィンドウマネージャが枠で包んでいるので、root の子そのものではなく
 * その下を探すことになる。深さは 4 も見れば足りる。 */
static Window find_client(Window w, int depth)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, a_wm_state, 0, 0, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) == Success) {
        if (data) XFree(data);
        if (type != None) return w;
    }
    if (depth <= 0) return None;

    Window root_r, parent, *kids = NULL;
    unsigned int nk = 0;
    if (!XQueryTree(dpy, w, &root_r, &parent, &kids, &nk)) return None;
    Window found = None;
    for (unsigned int i = 0; i < nk && found == None; i++)
        found = find_client(kids[i], depth - 1);
    if (kids) XFree(kids);
    return found;
}

static void win_title(Window w, char *out, size_t n)
{
    out[0] = 0;

    /* まず _NET_WM_NAME (UTF-8)。GTK のアプリはこちらしか出さない。 */
    Atom type;
    int fmt;
    unsigned long len, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, a_net_name, 0, 256, False, a_utf8,
                           &type, &fmt, &len, &after, &data) == Success &&
        data) {
        snprintf(out, n, "%s", (char *)data);
        XFree(data);
        if (out[0]) return;
    }

    char *nm = NULL;
    if (XFetchName(dpy, w, &nm) && nm) {
        snprintf(out, n, "%s", nm);
        XFree(nm);
        if (out[0]) return;
    }

    XClassHint ch = { NULL, NULL };
    if (XGetClassHint(dpy, w, &ch)) {
        snprintf(out, n, "%s", ch.res_class ? ch.res_class :
                               (ch.res_name ? ch.res_name : ""));
        if (ch.res_name)  XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (!out[0]) snprintf(out, n, "(untitled)");
}

static long win_pid(Window w)
{
    Atom type;
    int fmt;
    unsigned long len, after;
    unsigned char *data = NULL;
    long pid = -1;
    if (XGetWindowProperty(dpy, w, a_net_pid, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &len, &after, &data) == Success &&
        data) {
        if (len >= 1) pid = (long)*(unsigned long *)data;
        XFree(data);
    }
    return pid;
}

static int win_takes_delete(Window w)
{
    Atom *pr = NULL;
    int n = 0, yes = 0;
    if (XGetWMProtocols(dpy, w, &pr, &n)) {
        for (int i = 0; i < n; i++)
            if (pr[i] == a_wm_delete) yes = 1;
        if (pr) XFree(pr);
    }
    return yes;
}

static void scan_apps(void)
{
    n_apps = 0;
    Window root = RootWindow(dpy, screen);
    Window root_r, parent, *kids = NULL;
    unsigned int nk = 0;
    if (!XQueryTree(dpy, root, &root_r, &parent, &kids, &nk)) return;

    for (unsigned int i = 0; i < nk && n_apps < MAX_APP; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
        if (wa.override_redirect) continue;      /* メニューなどは数えない */
        if (wa.map_state != IsViewable) continue;

        Window c = find_client(kids[i], 4);
        if (c == None) continue;
        if (c == win) continue;                  /* 自分は出さない */

        App *a = &apps[n_apps];
        a->win = c;
        win_title(c, a->title, sizeof(a->title));
        a->pid = win_pid(c);
        a->responding = win_takes_delete(c);
        n_apps++;
    }
    if (kids) XFree(kids);
}

/* --- プロセスを数える ---------------------------------------------------- */

static unsigned long long total_jiffies(void)
{
    char b[256];
    if (!rd_file("/proc/stat", b, sizeof(b))) return 0;
    unsigned long long v[8] = { 0 };
    sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    unsigned long long t = 0;
    for (int i = 0; i < 8; i++) t += v[i];
    return t;
}

static void user_of(uid_t u, char *out, size_t n)
{
    struct passwd *pw = getpwuid(u);
    if (pw && pw->pw_name) snprintf(out, n, "%s", pw->pw_name);
    else                   snprintf(out, n, "%u", (unsigned)u);
}

static void scan_procs(void)
{
    /* 前回の値を退避してから数え直す */
    memcpy(prev, procs, sizeof(Proc) * (size_t)n_procs);
    n_prev = n_procs;
    unsigned long long total = total_jiffies();
    unsigned long long dtotal = (prev_total && total > prev_total)
                                ? total - prev_total : 0;

    n_procs = 0;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && n_procs < MAX_PROC) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        long pid = strtol(e->d_name, NULL, 10);
        if (pid <= 0) continue;

        char path[64], buf[2048];
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        if (!rd_file(path, buf, sizeof(buf))) continue;

        /* comm は括弧で囲まれていて、中に空白も括弧も入りうる。
         * 最後の ')' を探すのが唯一の正しい読み方。 */
        char *lp = strchr(buf, '(');
        char *rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp) continue;

        Proc *p = &procs[n_procs];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        size_t nl = (size_t)(rp - lp - 1);
        if (nl >= sizeof(p->name)) nl = sizeof(p->name) - 1;
        memcpy(p->name, lp + 1, nl);
        p->name[nl] = 0;

        /* rp + 2 から先が 3 番目の欄 (state)。utime は 14、stime は 15。 */
        unsigned long long ut = 0, st = 0;
        long rss_pages = 0;
        sscanf(rp + 2,
               "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
               "%llu %llu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
               &ut, &st, &rss_pages);
        p->cpu = ut + st;
        p->rss_kb = rss_pages * (sysconf(_SC_PAGESIZE) / 1024);

        /* 持ち主は /proc/<pid> の所有者。status を読むより速い。 */
        snprintf(path, sizeof(path), "/proc/%ld", pid);
        struct stat sb;
        if (stat(path, &sb) == 0) user_of(sb.st_uid, p->user, sizeof(p->user));
        else snprintf(p->user, sizeof(p->user), "?");

        /* 前回と比べて割合を出す */
        p->pct = 0.0;
        if (dtotal) {
            for (int i = 0; i < n_prev; i++)
                if (prev[i].pid == pid) {
                    if (p->cpu > prev[i].cpu)
                        p->pct = 100.0 * (double)(p->cpu - prev[i].cpu) /
                                 (double)dtotal;
                    break;
                }
        }
        n_procs++;
    }
    closedir(d);
    prev_total = total;

    /* CPU の重い順。同じなら常駐メモリの多い順。
     * 見たいのはたいてい「何が食っているか」なので。 */
    for (int i = 1; i < n_procs; i++) {
        Proc t = procs[i];
        int j = i - 1;
        while (j >= 0 && (procs[j].pct < t.pct ||
                          (procs[j].pct == t.pct &&
                           procs[j].rss_kb < t.rss_kb))) {
            procs[j + 1] = procs[j];
            j--;
        }
        procs[j + 1] = t;
    }
}

static void refresh(void)
{
    scan_apps();
    scan_procs();
    for (int t = 0; t < N_TABS; t++) {
        int n = (t == TAB_APPS) ? n_apps : n_procs;
        if (sel[t] >= n) sel[t] = n ? n - 1 : 0;
        if (top[t] > sel[t]) top[t] = sel[t];
    }
}

/* --- 終わらせる ---------------------------------------------------------- */

static void end_app(void)
{
    if (sel[TAB_APPS] < 0 || sel[TAB_APPS] >= n_apps) return;
    App *a = &apps[sel[TAB_APPS]];

    if (a->responding) {
        /* まず頼む。保存していない書きかけがあるアプリは
         * ここで自分の確認画面を出す。それが正しい。 */
        XEvent e;
        memset(&e, 0, sizeof(e));
        e.xclient.type = ClientMessage;
        e.xclient.window = a->win;
        e.xclient.message_type = a_wm_protocols;
        e.xclient.format = 32;
        e.xclient.data.l[0] = (long)a_wm_delete;
        e.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, a->win, False, NoEventMask, &e);
        snprintf(msg, sizeof(msg), "Asked \"%s\" to close.", a->title);
    } else {
        XKillClient(dpy, a->win);
        snprintf(msg, sizeof(msg), "Ended \"%s\".", a->title);
    }
    XFlush(dpy);
}

static void end_proc(void)
{
    if (sel[TAB_PROCS] < 0 || sel[TAB_PROCS] >= n_procs) return;
    Proc *p = &procs[sel[TAB_PROCS]];

    if (p->pid == getpid()) {
        snprintf(msg, sizeof(msg), "That is Task Manager itself.");
        return;
    }
    if (p->pid == 1) {
        /* PID 1 を殺すとカーネルパニックになる。押せてしまう場所に
         * 置いておくものではない。 */
        snprintf(msg, sizeof(msg),
                 "Ending process 1 would stop the computer.");
        return;
    }
    if (kill((pid_t)p->pid, SIGTERM) == 0)
        snprintf(msg, sizeof(msg), "Asked %s (%ld) to stop.",
                 p->name, p->pid);
    else
        snprintf(msg, sizeof(msg), "Could not stop %s (%ld): %s",
                 p->name, p->pid, strerror(errno));
}

static void kill_proc(void)
{
    if (sel[TAB_PROCS] < 0 || sel[TAB_PROCS] >= n_procs) return;
    Proc *p = &procs[sel[TAB_PROCS]];
    if (p->pid == getpid() || p->pid == 1) { end_proc(); return; }
    if (kill((pid_t)p->pid, SIGKILL) == 0)
        snprintf(msg, sizeof(msg), "Ended %s (%ld).", p->name, p->pid);
    else
        snprintf(msg, sizeof(msg), "Could not end %s (%ld): %s",
                 p->name, p->pid, strerror(errno));
}

/* --- 描画 ---------------------------------------------------------------- */

static void draw_tabs(void)
{
    int x = 8;
    for (int i = 0; i < N_TABS; i++) {
        int w = x98_text_w(&x98, tab_name[i]) + 24;
        int y = (i == tab) ? 4 : 7;
        int h = (i == tab) ? TAB_H - 2 : TAB_H - 5;
        x98_fill(&x98, win, x, y, w, h, x98.face);
        x98_hline(&x98, win, x, y, w, x98.light);
        x98_vline(&x98, win, x, y, h, x98.light);
        x98_vline(&x98, win, x + w - 1, y, h, x98.shadow);
        x98_text(&x98, win, x + 12, y + (h - x98_text_h(&x98)) / 2,
                 tab_name[i], x98.text);
        x += w + 2;
    }
    x98_hline(&x98, win, 8, TAB_H + 4, WIN_W - 16, x98.light);
}


/* --- 縦スクロールバー (Windows 98 のあれ) -------------------------------
 * プロセスは 70 も 80 もあるので、入り切らないぶんがあることを
 * 見せないと「これで全部」に見えてしまう。
 * つまみを引っ張るのは作らない。矢印と、つまみの上下を突いての
 * 1 画面送り、それとホイールで足りる。 */
#define SB_W 16

static void sb_arrow(int px, int py, int down)
{
    for (int i = 0; i < 4; i++) {
        int w = 1 + i * 2;
        int y = down ? py + 5 - i : py + i;
        x98_hline(&x98, win, px + 8 - w / 2 - 1, y, w, x98.text);
    }
}

static void draw_scrollbar(int n, int vis, int topv)
{
    if (n <= vis) return;
    int x = LIST_X + LIST_W - 2 - SB_W;
    int y = LIST_Y + 2;
    int h = LIST_H - 4;

    x98_fill(&x98, win, x, y, SB_W, h, x98_rgb24(&x98, 0xC0C0C0));
    x98_button(&x98, win, x, y, SB_W, SB_W, "", 0);
    sb_arrow(x + 4, y + 6, 0);
    x98_button(&x98, win, x, y + h - SB_W, SB_W, SB_W, "", 0);
    sb_arrow(x + 4, y + h - SB_W + 6, 1);

    int track = h - SB_W * 2;
    int th = track * vis / n;
    if (th < 12) th = 12;
    if (th > track) th = track;
    int maxtop = n - vis;
    int ty = y + SB_W + (maxtop > 0 ? (track - th) * topv / maxtop : 0);
    x98_button(&x98, win, x, ty, SB_W, th, "", 0);
}

static int sb_click(int mx, int my, int n, int vis, int *topv)
{
    if (n <= vis) return 0;
    int x = LIST_X + LIST_W - 2 - SB_W;
    if (mx < x || mx >= x + SB_W) return 0;
    int y = LIST_Y + 2, h = LIST_H - 4;
    if (my < y || my >= y + h) return 0;

    int track = h - SB_W * 2;
    int th = track * vis / n;
    if (th < 12) th = 12;
    if (th > track) th = track;
    int maxtop = n - vis;
    int ty = y + SB_W + (maxtop > 0 ? (track - th) * (*topv) / maxtop : 0);

    if (my < y + SB_W)            (*topv)--;
    else if (my >= y + h - SB_W)  (*topv)++;
    else if (my < ty)             (*topv) -= vis;
    else if (my >= ty + th)       (*topv) += vis;

    if (*topv > maxtop) *topv = maxtop;
    if (*topv < 0) *topv = 0;
    return 1;
}

/* 幅に収まるように末尾を "..." で詰める。
 * プロセス名は長いものがあり (kworker/u16:4-events_unbound など)、
 * そのままだと隣の欄に食い込む。 */
static void fit(const char *src, char *out, size_t n, int wpx)
{
    snprintf(out, n, "%s", src);
    if (x98_text_w(&x98, out) <= wpx) return;
    for (int len = (int)strlen(out); len > 0; len--) {
        out[len] = 0;
        if (len + 3 < (int)n) { strcpy(out + len, "..."); }
        if (x98_text_w(&x98, out) <= wpx) return;
        out[len] = 0;
    }
}

static void draw_list(void)
{
    x98_bevel(&x98, win, LIST_X, LIST_Y, LIST_W, LIST_H, 0);
    x98_fill(&x98, win, LIST_X + 2, LIST_Y + 2, LIST_W - 4, LIST_H - 4,
             x98.white);

    int n   = (tab == TAB_APPS) ? n_apps : n_procs;
    int vis = (LIST_H - 4) / ROW_H;
    draw_scrollbar(n, vis, top[tab]);

    for (int r = 0; r < vis && top[tab] + r < n; r++) {
        int i = top[tab] + r;
        int y = LIST_Y + 2 + r * ROW_H;
        int on = (i == sel[tab]);
        unsigned long fg = x98.text;
        if (on) {
            x98_fill(&x98, win, LIST_X + 2, y,
                     LIST_W - 4 - (n > vis ? SB_W : 0), ROW_H,
                     x98.select_bg);
            fg = x98.white;
        }
        int ty = y + (ROW_H - x98_text_h(&x98)) / 2;

        char cell[160];
        if (tab == TAB_APPS) {
            App *a = &apps[i];
            fit(a->title, cell, sizeof(cell), LIST_W - 160);
            x98_text(&x98, win, LIST_X + 10, ty, cell, fg);
            x98_text(&x98, win, LIST_X + LIST_W - 130, ty,
                     a->responding ? "Running" : "Not responding", fg);
        } else {
            Proc *p = &procs[i];
            char b[64];
            fit(p->name, cell, sizeof(cell), 172);
            x98_text(&x98, win, LIST_X + 10, ty, cell, fg);
            snprintf(b, sizeof(b), "%ld", p->pid);
            x98_text(&x98, win, LIST_X + 190, ty, b, fg);
            x98_text(&x98, win, LIST_X + 250, ty, p->user, fg);
            snprintf(b, sizeof(b), "%3.0f%%", p->pct);
            x98_text(&x98, win, LIST_X + 340, ty, b, fg);
            if (p->rss_kb >= 10240)
                snprintf(b, sizeof(b), "%ld MB", p->rss_kb / 1024);
            else
                snprintf(b, sizeof(b), "%ld KB", p->rss_kb);
            x98_text(&x98, win, LIST_X + 396, ty, b, fg);
        }
    }
}

static void draw_head(void)
{
    int y = TAB_H + 16;
    if (tab == TAB_APPS) {
        x98_text(&x98, win, LIST_X + 10, y, "Task", x98.shadow);
        x98_text(&x98, win, LIST_X + LIST_W - 130, y, "Status", x98.shadow);
    } else {
        x98_text(&x98, win, LIST_X + 10,  y, "Image name", x98.shadow);
        x98_text(&x98, win, LIST_X + 190, y, "PID",        x98.shadow);
        x98_text(&x98, win, LIST_X + 250, y, "User",       x98.shadow);
        x98_text(&x98, win, LIST_X + 340, y, "CPU",        x98.shadow);
        x98_text(&x98, win, LIST_X + 396, y, "Memory",     x98.shadow);
    }
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);
    draw_tabs();
    draw_head();
    draw_list();

    char sum[160];
    if (tab == TAB_APPS)
        snprintf(sum, sizeof(sum), "%d window%s open",
                 n_apps, n_apps == 1 ? "" : "s");
    else
        snprintf(sum, sizeof(sum), "%d process%s",
                 n_procs, n_procs == 1 ? "" : "es");
    x98_text(&x98, win, PAD, LIST_Y + LIST_H + 8, sum, x98.shadow);
    if (msg[0])
        x98_text(&x98, win, PAD, LIST_Y + LIST_H + 26, msg, x98.text);

    int by = WIN_H - BTN_H - PAD;
    x98_button(&x98, win, PAD, by, BTN_W, BTN_H,
               tab == TAB_APPS ? "End Task" : "End Process", 0);
    if (tab == TAB_PROCS)
        x98_button(&x98, win, PAD + BTN_W + 8, by, BTN_W, BTN_H,
                   "Force End", 0);
    x98_button(&x98, win, WIN_W - 2 * BTN_W - 20, by, BTN_W, BTN_H,
               "Refresh", 0);
    x98_button(&x98, win, WIN_W - BTN_W - PAD, by, BTN_W, BTN_H, "Close", 0);
}

static void scroll_into_view(void)
{
    int vis = (LIST_H - 4) / ROW_H;
    if (sel[tab] < top[tab]) top[tab] = sel[tab];
    if (sel[tab] >= top[tab] + vis) top[tab] = sel[tab] - vis + 1;
    if (top[tab] < 0) top[tab] = 0;
}

int main(void)
{
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-taskman: no display\n"); return 1; }
    XSetErrorHandler(ignore_x_error);
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    a_wm_state     = XInternAtom(dpy, "WM_STATE", False);
    a_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_net_name     = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_net_pid      = XInternAtom(dpy, "_NET_WM_PID", False);
    a_utf8         = XInternAtom(dpy, "UTF8_STRING", False);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XStoreName(dpy, win, "Task Manager");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &del, 1);
    XMapWindow(dpy, win);

    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    refresh();

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            redraw();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == del) goto done;
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);
            int cnt = (tab == TAB_APPS) ? n_apps : n_procs;
            int vis = (LIST_H - 4) / ROW_H;

            if (ks == XK_Escape) goto done;
            else if (ks == XK_Up   && sel[tab] > 0) sel[tab]--;
            else if (ks == XK_Down && sel[tab] < cnt - 1) sel[tab]++;
            else if (ks == XK_Prior) sel[tab] -= vis;
            else if (ks == XK_Next)  sel[tab] += vis;
            else if (ks == XK_Home)  sel[tab] = 0;
            else if (ks == XK_End)   sel[tab] = cnt - 1;
            else if (ks == XK_Tab) { tab = (tab + 1) % N_TABS; }
            else if (ks == XK_Left)  tab = 0;
            else if (ks == XK_Right) tab = N_TABS - 1;
            else if (ks == XK_Delete || ks == XK_Return || ks == XK_KP_Enter) {
                if (tab == TAB_APPS) end_app(); else end_proc();
                refresh();
            } else if (ks == XK_F5 ||
                       (n > 0 && (buf[0] == 'r' || buf[0] == 'R'))) {
                msg[0] = 0;
                refresh();
            }
            cnt = (tab == TAB_APPS) ? n_apps : n_procs;
            if (sel[tab] < 0) sel[tab] = 0;
            if (sel[tab] >= cnt) sel[tab] = cnt ? cnt - 1 : 0;
            scroll_into_view();
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            if (my < TAB_H + 4) {
                int x = 8;
                for (int i = 0; i < N_TABS; i++) {
                    int w = x98_text_w(&x98, tab_name[i]) + 24;
                    if (mx >= x && mx < x + w) { tab = i; break; }
                    x += w + 2;
                }
                redraw();
                break;
            }

            int by = WIN_H - BTN_H - PAD;
            if (my >= by && my < by + BTN_H) {
                if (mx >= PAD && mx < PAD + BTN_W) {
                    if (tab == TAB_APPS) end_app(); else end_proc();
                    refresh();
                } else if (tab == TAB_PROCS &&
                           mx >= PAD + BTN_W + 8 &&
                           mx < PAD + 2 * BTN_W + 8) {
                    kill_proc();
                    refresh();
                } else if (mx >= WIN_W - 2 * BTN_W - 20 &&
                           mx < WIN_W - BTN_W - 20) {
                    msg[0] = 0;
                    refresh();
                } else if (mx >= WIN_W - BTN_W - PAD) {
                    goto done;
                }
            } else if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                /* ホイール。3 行ずつ。 */
                int cnt = (tab == TAB_APPS) ? n_apps : n_procs;
                int vis = (LIST_H - 4) / ROW_H;
                top[tab] += (ev.xbutton.button == 4) ? -3 : 3;
                if (top[tab] > cnt - vis) top[tab] = cnt - vis;
                if (top[tab] < 0) top[tab] = 0;
            } else if (sb_click(mx, my,
                                (tab == TAB_APPS) ? n_apps : n_procs,
                                (LIST_H - 4) / ROW_H, &top[tab])) {
                /* スクロールバー */
            } else if (mx >= LIST_X && mx < LIST_X + LIST_W &&
                       my >= LIST_Y + 2 && my < LIST_Y + LIST_H - 2) {
                int i = top[tab] + (my - LIST_Y - 2) / ROW_H;
                int cnt = (tab == TAB_APPS) ? n_apps : n_procs;
                if (i >= 0 && i < cnt) sel[tab] = i;
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

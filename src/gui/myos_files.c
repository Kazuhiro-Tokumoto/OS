/* ==========================================================================
 * myos_files.c  -  Windows 98 風のファイルマネージャ
 *
 * X11 の普通のクライアント。枠は myos-wm が描くので、こちらは
 * 中身 (ツールバー + 一覧) だけを描く。
 *
 * やっていること:
 *   - ディレクトリの一覧表示 (フォルダが先、次にファイル、名前順)
 *   - ダブルクリックで開く
 *       ディレクトリ  -> そこへ移動
 *       .jar         -> java -jar で実行
 *       実行可能      -> そのまま実行
 *       それ以外      -> Firefox に渡す
 *   - 上へ移動 / ホーム / 更新
 *   - 右クリックした項目をデスクトップのリンクとして登録する
 *     (/etc/myos/desktop.conf に追記して myos-wm に SIGUSR1 を送る)
 *   - ホイールでスクロール
 *
 * ビルド:
 *   gcc -O2 -o myos-files myos_files.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>

#include "x98.h"

#define WIN_W       660
#define WIN_H       460
#define TOOLBAR_H   32
#define ROW_H       20
#define PAD         6
#define DBLCLICK_MS 700
#define MAX_ENTRIES 4096

/* ツールバーのボタン配置 */
#define TB_UP_X     4
#define TB_UP_W     40
#define TB_HOME_X   (TB_UP_X + TB_UP_W + 4)
#define TB_HOME_W   54
#define TB_REFR_X   (TB_HOME_X + TB_HOME_W + 4)
#define TB_REFR_W   70
#define TB_NEW_X    (TB_REFR_X + TB_REFR_W + 4)
#define TB_NEW_W    92
#define TB_DEL_X    (TB_NEW_X + TB_NEW_W + 4)
#define TB_DEL_W    64
#define TB_PATH_X   (TB_DEL_X + TB_DEL_W + 6)
#define DESKTOP_CONF "/etc/myos/desktop.conf"

typedef struct {
    char name[256];
    int  is_dir;
    int  is_exec;
    long size;
} Entry;

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

static char   cwd[PATH_MAX] = "/";
static Entry  entries[MAX_ENTRIES];
static int    n_entries = 0;
static int    top = 0;          /* 一覧の先頭に出している行 */
static int    sel = -1;
static int    win_w = WIN_W, win_h = WIN_H;
static int    confirm_delete = 0;   /* 削除の確認を出しているか */

/* --- 小さいアイコン (16x16) ---------------------------------------------- */
static void small_folder(int px, int py)
{
    x98_fill(&x98, win, px + 1, py + 3, 6, 2, x98.folder);
    x98_fill(&x98, win, px, py + 5, 15, 9, x98.folder);
    x98_hline(&x98, win, px, py + 5, 15, x98_rgb24(&x98, 0xFFF0B0));
    x98_hline(&x98, win, px, py + 13, 15, x98_rgb24(&x98, 0xA07000));
}

static void small_file(int px, int py)
{
    x98_fill(&x98, win, px + 2, py + 1, 11, 14, x98.white);
    x98_bevel(&x98, win, px + 2, py + 1, 11, 14, 0);
    for (int i = 0; i < 3; i++)
        x98_hline(&x98, win, px + 4, py + 4 + i * 3, 7, x98.shadow);
}

static void small_exec(int px, int py)
{
    x98_fill(&x98, win, px + 1, py + 2, 13, 12, x98.face);
    x98_bevel(&x98, win, px + 1, py + 2, 13, 12, 1);
    x98_fill(&x98, win, px + 3, py + 4, 9, 3, x98_rgb24(&x98, RGB_TITLE1));
}

/* --- ディレクトリの読み込み ---------------------------------------------- */
static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;  /* dir を先に */
    return strcmp(x->name, y->name);
}

static void read_dir(void)
{
    n_entries = 0;
    top = 0;
    sel = -1;

    DIR *d = opendir(cwd);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) && n_entries < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".")) continue;
        if (!strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.') continue;     /* 隠しファイルは出さない */

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", cwd, de->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        Entry *e = &entries[n_entries++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->is_dir  = S_ISDIR(st.st_mode);
        e->is_exec = !e->is_dir && (st.st_mode & S_IXUSR);
        e->size    = (long)st.st_size;
    }
    closedir(d);

    qsort(entries, n_entries, sizeof(Entry), cmp_entry);

    char title[PATH_MAX + 32];
    snprintf(title, sizeof(title), "myOS Files - %s", cwd);
    XStoreName(dpy, win, title);
}

static void go_to(const char *path)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return;
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    snprintf(cwd, sizeof(cwd), "%s", resolved);
    read_dir();
}

/* --- 起動 ---------------------------------------------------------------- */
static void spawn(const char *cmd)
{
    pid_t p = fork();
    if (p == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* シェルを通さずに実行して終わるまで待つ。
 * パスに空白や引用符が入っていても壊れない。 */
static void run_wait(const char *file, const char *a1, const char *a2)
{
    pid_t p = fork();
    if (p == 0) {
        execlp(file, file, a1, a2, (char *)NULL);
        _exit(127);
    }
    if (p > 0) {
        int st;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    }
}

static void make_new_folder(void)
{
    for (int i = 1; i < 100; i++) {
        char path[PATH_MAX];
        if (i == 1)
            snprintf(path, sizeof(path), "%s/New Folder",
                     strcmp(cwd, "/") ? cwd : "");
        else
            snprintf(path, sizeof(path), "%s/New Folder (%d)",
                     strcmp(cwd, "/") ? cwd : "", i);
        if (mkdir(path, 0755) == 0) { read_dir(); return; }
        if (errno != EEXIST) return;
    }
}

static void delete_selected(void)
{
    if (sel < 0 || sel >= n_entries) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s",
             strcmp(cwd, "/") ? cwd : "", entries[sel].name);
    /* ディレクトリは中身ごと消す。rm に任せるのがいちばん確実。 */
    run_wait("rm", "-rf", path);
    read_dir();
}

static int ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

static void open_entry(int i)
{
    if (i < 0 || i >= n_entries) return;
    Entry *e = &entries[i];

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s",
             strcmp(cwd, "/") ? cwd : "", e->name);

    if (e->is_dir) { go_to(full); return; }

    char cmd[PATH_MAX + 64];
    if (ends_with(e->name, ".jar"))
        snprintf(cmd, sizeof(cmd), "java -jar '%s'", full);
    else if (e->is_exec)
        snprintf(cmd, sizeof(cmd), "'%s'", full);
    else
        snprintf(cmd, sizeof(cmd), "firefox-esr '%s'", full);
    spawn(cmd);
}

/* 右クリックした項目をデスクトップのリンクとして登録する。
 * 設定ファイルに 1 行足して、myos-wm に SIGUSR1 を送って読み直させる。 */
static void add_to_desktop(int i)
{
    if (i < 0 || i >= n_entries) return;
    Entry *e = &entries[i];

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s",
             strcmp(cwd, "/") ? cwd : "", e->name);

    const char *icon = "app";
    char cmd[PATH_MAX + 64];
    if (e->is_dir) {
        icon = "folder";
        snprintf(cmd, sizeof(cmd), "/usr/local/bin/myos-files '%s'", full);
    } else if (ends_with(e->name, ".jar")) {
        icon = "java";
        snprintf(cmd, sizeof(cmd), "java -jar '%s'", full);
    } else if (e->is_exec) {
        snprintf(cmd, sizeof(cmd), "'%s'", full);
    } else {
        icon = "file";
        snprintf(cmd, sizeof(cmd), "firefox-esr '%s'", full);
    }

    FILE *f = fopen(DESKTOP_CONF, "a");
    if (!f) return;
    fprintf(f, "%s|%s|%s\n", e->name, icon, cmd);
    fclose(f);

    spawn("pkill -USR1 -x myos-wm");
}

/* --- 描画 ---------------------------------------------------------------- */
static int rows_visible(void)
{
    int h = win_h - TOOLBAR_H - PAD * 2 - 4;
    return h / ROW_H;
}

static void draw_toolbar(void)
{
    x98_fill(&x98, win, 0, 0, win_w, TOOLBAR_H, x98.face);

    /* ボタンの幅は文字がはみ出さないよう余裕を持たせてある。
     * 詰めすぎるとラベルが隣に食い込んで読めなくなる。 */
    x98_button(&x98, win, TB_UP_X,   4, TB_UP_W,   TOOLBAR_H - 8, "Up", 0);
    x98_button(&x98, win, TB_HOME_X, 4, TB_HOME_W, TOOLBAR_H - 8, "Home", 0);
    x98_button(&x98, win, TB_REFR_X, 4, TB_REFR_W, TOOLBAR_H - 8, "Refresh", 0);
    x98_button(&x98, win, TB_NEW_X,  4, TB_NEW_W,  TOOLBAR_H - 8, "New Folder", 0);
    x98_button(&x98, win, TB_DEL_X,  4, TB_DEL_W,  TOOLBAR_H - 8, "Delete", 0);

    int bx = TB_PATH_X;
    int bw = win_w - bx - 4;
    x98_fill(&x98, win, bx, 4, bw, TOOLBAR_H - 8, x98.white);
    x98_bevel(&x98, win, bx, 4, bw, TOOLBAR_H - 8, 0);

    if (confirm_delete && sel >= 0 && sel < n_entries) {
        /* 確認は別の窓を出さず、パス欄をそのまま使う */
        char msg[300];
        snprintf(msg, sizeof(msg), "Delete \"%s\" ?", entries[sel].name);
        x98_text(&x98, win, bx + 6,
                 4 + (TOOLBAR_H - 8 - x98_text_h(&x98)) / 2, msg, x98.text);
        x98_button(&x98, win, win_w - 104, 6, 44, TOOLBAR_H - 12, "Yes", 0);
        x98_button(&x98, win, win_w - 56, 6, 44, TOOLBAR_H - 12, "No", 0);
    } else {
        x98_text(&x98, win, bx + 4,
                 4 + (TOOLBAR_H - 8 - x98_text_h(&x98)) / 2, cwd, x98.text);
    }

    x98_hline(&x98, win, 0, TOOLBAR_H - 2, win_w, x98.shadow);
    x98_hline(&x98, win, 0, TOOLBAR_H - 1, win_w, x98.light);
}

static void draw_list(void)
{
    int lx = PAD, ly = TOOLBAR_H + PAD;
    int lw = win_w - PAD * 2;
    int lh = win_h - ly - PAD;
    if (lw < 20 || lh < 20) return;

    x98_bevel(&x98, win, lx, ly, lw, lh, 0);
    x98_fill(&x98, win, lx + 2, ly + 2, lw - 4, lh - 4, x98.white);

    int vis = rows_visible();
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= n_entries) break;
        Entry *e = &entries[i];

        int ry = ly + 2 + r * ROW_H;
        unsigned long fg = x98.text;
        if (i == sel) {
            x98_fill(&x98, win, lx + 2, ry, lw - 4, ROW_H, x98.select_bg);
            fg = x98.white;
        }

        if (e->is_dir)       small_folder(lx + 6, ry + 2);
        else if (e->is_exec) small_exec(lx + 6, ry + 2);
        else                 small_file(lx + 6, ry + 2);

        x98_text(&x98, win, lx + 28, ry + (ROW_H - x98_text_h(&x98)) / 2,
                 e->name, fg);

        if (!e->is_dir) {
            char sz[32];
            if (e->size >= 1024 * 1024)
                snprintf(sz, sizeof(sz), "%ld MB", e->size / (1024 * 1024));
            else if (e->size >= 1024)
                snprintf(sz, sizeof(sz), "%ld KB", e->size / 1024);
            else
                snprintf(sz, sizeof(sz), "%ld B", e->size);
            int tw = x98_text_w(&x98, sz);
            x98_text(&x98, win, lx + lw - 10 - tw,
                     ry + (ROW_H - x98_text_h(&x98)) / 2, sz, fg);
        }
    }

    /* 何件あるか。件数が見えると安心なので出しておく */
    char info[64];
    snprintf(info, sizeof(info), "%d items", n_entries);
    x98_text(&x98, win, lx + 6, ly + lh - x98_text_h(&x98) - 4, "", x98.text);
    (void)info;
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, win_w, win_h, x98.face);
    draw_toolbar();
    draw_list();
}

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-files] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask |
                           StructureNotifyMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XStoreName(dpy, win, "myOS Files");
    XMapWindow(dpy, win);

    go_to(argc > 1 ? argv[1] : "/");

    long last_click_ms = 0;
    int  last_click_row = -1;

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

        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int vis = rows_visible();
            if (ks == XK_Up   && sel > 0) sel--;
            else if (ks == XK_Down && sel < n_entries - 1) sel++;
            else if (ks == XK_Return) { open_entry(sel); }
            else if (ks == XK_BackSpace) {
                char up[PATH_MAX];
                snprintf(up, sizeof(up), "%s/..", cwd);
                go_to(up);
            }
            if (sel >= 0) {
                if (sel < top) top = sel;
                if (sel >= top + vis) top = sel - vis + 1;
            }
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            /* ホイール */
            if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                int vis = rows_visible();
                top += (ev.xbutton.button == 5) ? 3 : -3;
                if (top > n_entries - vis) top = n_entries - vis;
                if (top < 0) top = 0;
                redraw();
                break;
            }

            /* ツールバー */
            if (my < TOOLBAR_H) {
                if (confirm_delete) {
                    if (mx >= win_w - 104 && mx < win_w - 60)
                        delete_selected();
                    confirm_delete = 0;
                    redraw();
                    break;
                }
                if (mx >= TB_UP_X && mx < TB_UP_X + TB_UP_W) {
                    char up[PATH_MAX];
                    snprintf(up, sizeof(up), "%s/..", cwd);
                    go_to(up);
                } else if (mx >= TB_HOME_X && mx < TB_HOME_X + TB_HOME_W) {
                    go_to("/root");
                } else if (mx >= TB_REFR_X && mx < TB_REFR_X + TB_REFR_W) {
                    read_dir();
                } else if (mx >= TB_NEW_X && mx < TB_NEW_X + TB_NEW_W) {
                    make_new_folder();
                } else if (mx >= TB_DEL_X && mx < TB_DEL_X + TB_DEL_W) {
                    if (sel >= 0 && sel < n_entries) confirm_delete = 1;
                }
                redraw();
                break;
            }

            /* 一覧 */
            confirm_delete = 0;
            int ly = TOOLBAR_H + PAD + 2;
            int row = (my - ly) / ROW_H;
            int i = top + row;
            if (row < 0 || i >= n_entries) { sel = -1; redraw(); break; }

            if (ev.xbutton.button == 3) {
                /* 右クリック: デスクトップにリンクを作る */
                sel = i;
                add_to_desktop(i);
                redraw();
                break;
            }

            long t = now_ms();
            if (i == last_click_row && t - last_click_ms < DBLCLICK_MS) {
                last_click_row = -1;
                sel = i;
                open_entry(i);
            } else {
                last_click_row = i;
                last_click_ms = t;
                sel = i;
            }
            redraw();
            break;
        }

        default:
            break;
        }
    }
    return 0;
}

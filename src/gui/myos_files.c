/* ==========================================================================
 * myos_files.c  -  Windows 98 風のファイルマネージャ
 *
 * X11 の普通のクライアント。枠は myos-wm が描くので、こちらは
 * 中身 (ツールバー + 一覧) だけを描く。
 *
 * Windows でおなじみの操作をひととおり拾う:
 *   ダブルクリック / Enter    開く
 *   BackSpace                 上の階層へ
 *   F2                        名前の変更 (その場で編集)
 *   Delete                    削除 (確認あり)
 *   Ctrl+C / Ctrl+X / Ctrl+V  コピー / 切り取り / 貼り付け
 *   F5                        更新
 *   右クリック                コンテキストメニュー
 *   ホイール / 上下キー       スクロール
 *
 * 「何で開くか」はこのファイルには書かない。
 * /etc/myos/filetypes.conf と ~/.myos/filetypes.conf の表を見て
 * myos-open が決める (Windows のシェル関連付けと同じ考え方)。
 * ディレクトリだけは同じ窓で移動したいので、ここで処理する。
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
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "x98.h"

/* 日本語入力の入力文脈。IME が上がっていなければ NULL のまま。 */
static XIC ic;
#include "filetypes.h"

#define WIN_W       700
#define WIN_H       480
#define TOOLBAR_H   32
#define ROW_H       20
#define PAD         6
#define MAX_ENTRIES 4096
#define TYPE_COL_W  150   /* 「種類」の列幅 */
#define SIZE_COL_W   70   /* 「サイズ」の列幅 */

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

/* コンテキストメニュー。並びは Windows のそれに寄せてある。 */
enum {
    CTX_OPEN = 0, CTX_RUN, CTX_RUNAS, CTX_COPY, CTX_CUT, CTX_PASTE,
    CTX_RENAME, CTX_DELETE, CTX_DESKTOP, CTX_N
};
static const char *ctx_items[CTX_N] = {
    "Open", "Run", "Run as administrator...",
    "Copy", "Cut", "Paste", "Rename", "Delete", "Add to Desktop"
};
#define CTX_W 190
#define CTX_IH 20
#define CTX_H (CTX_N * CTX_IH + 6)

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
static int    top = 0;
static int    sel = -1;
static int    win_w = WIN_W, win_h = WIN_H;

static FileType ftypes[FT_MAX];
static int      n_ftypes = 0;

static int    confirm_delete = 0;
static int    ctx_open = 0, ctx_x = 0, ctx_y = 0;
static int    renaming = 0;
static X98Edit rename_edit;

/* 切り取り / コピーの控え。Windows と同じく「貼り付けるまで動かない」。 */
static char   clip_path[PATH_MAX] = "";
static int    clip_cut = 0;

static void read_dir(void);

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

/* --- 便利関数 ------------------------------------------------------------ */
/* 拡張子で何をするかは filetypes.h の表に任せたので、
 * ここに種類の判定を持たない。 */

static void full_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", strcmp(cwd, "/") ? cwd : "", name);
}

/* シェルを通さずに起動する。ファイル名に空白や引用符があっても壊れない。 */
static void spawn_argv(const char *file, const char *a1, const char *a2)
{
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (a2)      execlp(file, file, a1, a2, (char *)NULL);
        else if (a1) execlp(file, file, a1, (char *)NULL);
        else         execlp(file, file, (char *)NULL);
        _exit(127);
    }
}

static void run_wait(const char *file, const char *a1, const char *a2,
                     const char *a3)
{
    pid_t p = fork();
    if (p == 0) {
        if (a3)      execlp(file, file, a1, a2, a3, (char *)NULL);
        else         execlp(file, file, a1, a2, (char *)NULL);
        _exit(127);
    }
    if (p > 0) {
        int st;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    }
}

/* --- ディレクトリの読み込み ---------------------------------------------- */
static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcmp(x->name, y->name);
}

static void read_dir(void)
{
    n_entries = 0;
    top = 0;
    sel = -1;
    renaming = 0;
    ctx_open = 0;
    confirm_delete = 0;

    DIR *d = opendir(cwd);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) && n_entries < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.') continue;   /* 隠しファイルは出さない */

        char full[PATH_MAX];
        full_path(full, sizeof(full), de->d_name);

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

static void go_to(const char *p)
{
    char resolved[PATH_MAX];
    if (!realpath(p, resolved)) return;
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    snprintf(cwd, sizeof(cwd), "%s", resolved);
    read_dir();
}

static void go_up(void)
{
    char up[PATH_MAX];
    snprintf(up, sizeof(up), "%s/..", cwd);
    go_to(up);
}

/* --- 操作 ---------------------------------------------------------------- */
/* 開く。ディレクトリだけは同じ窓で移動したいので自分で処理し、
 * それ以外は myos-open に丸投げする。
 * verb: 0 = 既定の「開く」 / 1 = 2 つ目の動詞「実行」
 *       2 = 管理者として実行 */
static void open_entry_verb(int i, int verb)
{
    if (i < 0 || i >= n_entries) return;
    Entry *e = &entries[i];

    char full[PATH_MAX];
    full_path(full, sizeof(full), e->name);

    if (e->is_dir && verb == 0) { go_to(full); return; }

    if (verb == 2)      spawn_argv("myos-runas", "myos-open", full);
    else if (verb == 1) spawn_argv("myos-open", "-run", full);
    else                spawn_argv("myos-open", full, NULL);
}

static void open_entry(int i) { open_entry_verb(i, 0); }

/* 「実行」の動詞を持っているか。無ければメニューで薄くする。 */
static int has_run_verb(int i)
{
    if (i < 0 || i >= n_entries) return 0;
    Entry *e = &entries[i];
    if (e->is_dir) return 0;
    const FileType *t = ft_find(ftypes, n_ftypes, e->name);
    if (t && t->run[0]) return 1;
    return e->is_exec;
}

/* 一覧に出す種類の名前。表に無ければ Windows と同じ言い方にしておく。 */
static const char *type_name(const Entry *e)
{
    if (e->is_dir) return "File Folder";
    const FileType *t = ft_find(ftypes, n_ftypes, e->name);
    if (t) return t->desc;
    if (e->is_exec) return "Application";
    return "File";
}

static void make_new_folder(void)
{
    for (int i = 1; i < 100; i++) {
        char p[PATH_MAX];
        if (i == 1) full_path(p, sizeof(p), "New Folder");
        else {
            char nm[64];
            snprintf(nm, sizeof(nm), "New Folder (%d)", i);
            full_path(p, sizeof(p), nm);
        }
        if (mkdir(p, 0755) == 0) { read_dir(); return; }
        if (errno != EEXIST) return;
    }
}

static void delete_selected(void)
{
    if (sel < 0 || sel >= n_entries) return;
    char p[PATH_MAX];
    full_path(p, sizeof(p), entries[sel].name);
    /* ディレクトリは中身ごと。rm に任せるのがいちばん確実。 */
    run_wait("rm", "-rf", p, NULL);
    read_dir();
}

static void do_copy(int cut)
{
    if (sel < 0 || sel >= n_entries) return;
    full_path(clip_path, sizeof(clip_path), entries[sel].name);
    clip_cut = cut;
}

static void do_paste(void)
{
    if (!clip_path[0]) return;

    /* 同じ場所に貼るときは名前を変える (Windows と同じ振る舞い) */
    const char *base = strrchr(clip_path, '/');
    base = base ? base + 1 : clip_path;

    char dst[PATH_MAX];
    full_path(dst, sizeof(dst), base);

    struct stat st;
    if (!clip_cut && stat(dst, &st) == 0) {
        for (int i = 2; i < 100; i++) {
            char nm[300];
            snprintf(nm, sizeof(nm), "%s (%d)", base, i);
            full_path(dst, sizeof(dst), nm);
            if (stat(dst, &st) != 0) break;
        }
    }

    if (clip_cut) {
        run_wait("mv", "-f", clip_path, dst);
        clip_path[0] = 0;
    } else {
        /* -a で属性ごと。ディレクトリもそのまま扱える。 */
        run_wait("cp", "-a", clip_path, dst);
    }
    read_dir();
}

static void start_rename(void)
{
    if (sel < 0 || sel >= n_entries) return;
    x98_edit_set(&rename_edit, entries[sel].name);
    renaming = 1;
}

static void commit_rename(void)
{
    renaming = 0;
    if (sel < 0 || sel >= n_entries) return;
    if (!rename_edit.buf[0]) return;
    if (!strcmp(rename_edit.buf, entries[sel].name)) return;
    if (strchr(rename_edit.buf, '/')) return;   /* 階層は動かさない */

    char from[PATH_MAX], to[PATH_MAX];
    full_path(from, sizeof(from), entries[sel].name);
    full_path(to, sizeof(to), rename_edit.buf);
    if (rename(from, to) != 0) return;
    read_dir();
}

static void add_to_desktop(int i)
{
    if (i < 0 || i >= n_entries) return;
    Entry *e = &entries[i];

    char full[PATH_MAX];
    full_path(full, sizeof(full), e->name);

    /* アイコンは関連付けの表から取る。表に無ければ実体で決める。 */
    const char *icon;
    if (e->is_dir) {
        icon = "folder";
    } else {
        const FileType *t = ft_find(ftypes, n_ftypes, e->name);
        icon = t && t->icon[0] ? t->icon : (e->is_exec ? "app" : "file");
    }

    /* コマンドは myos-open に統一する。あとで関連付けを変えたときに、
     * デスクトップのリンクも一緒に付いてくる。 */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "myos-open '%s'", full);

    /* 追記先はユーザーの設定。/etc/myos は root のものなので触らない。 */
    char path[512];
    myos_user_conf(path, sizeof(path), "desktop.conf");

    /* まだユーザーの分が無ければ、システム既定を写してから足す。
     * そうしないと既定のアイコンが全部消えたように見える。 */
    if (access(path, F_OK) != 0) {
        FILE *src = fopen(MYOS_ETC "/desktop.conf", "r");
        FILE *dst = fopen(path, "w");
        if (src && dst) {
            char buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
                fwrite(buf, 1, n, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    }

    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s|%s|%s\n", e->name, icon, cmd);
    fclose(f);

    /* ウィンドウマネージャに設定を読み直させる */
    pid_t p = fork();
    if (p == 0) {
        execlp("pkill", "pkill", "-USR1", "-x", "myos-wm", (char *)NULL);
        _exit(127);
    }
}

/* --- 描画 ---------------------------------------------------------------- */
static int rows_visible(void)
{
    int h = win_h - TOOLBAR_H - PAD * 2 - 4;
    return h / ROW_H;
}

static int list_y(void) { return TOOLBAR_H + PAD + 2; }

static void draw_toolbar(void)
{
    x98_fill(&x98, win, 0, 0, win_w, TOOLBAR_H, x98.face);

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
        char msg[300];
        snprintf(msg, sizeof(msg), "Delete \"%.200s\" ?", entries[sel].name);
        x98_text(&x98, win, bx + 6,
                 4 + (TOOLBAR_H - 8 - x98_text_h(&x98)) / 2, msg, x98.text);
        x98_button(&x98, win, win_w - 104, 6, 44, TOOLBAR_H - 12, "Yes", 0);
        x98_button(&x98, win, win_w - 56, 6, 44, TOOLBAR_H - 12, "No", 0);
    } else {
        char info[PATH_MAX + 64];
        if (clip_path[0])
            snprintf(info, sizeof(info), "%s   [%s ready to paste]",
                     cwd, clip_cut ? "cut" : "copy");
        else
            snprintf(info, sizeof(info), "%s", cwd);
        x98_text(&x98, win, bx + 4,
                 4 + (TOOLBAR_H - 8 - x98_text_h(&x98)) / 2, info, x98.text);
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

        if (renaming && i == sel) {
            x98_edit_draw(&x98, win, lx + 26, ry, lw - 120, ROW_H,
                          &rename_edit, 1);
        } else {
            int ty = ry + (ROW_H - x98_text_h(&x98)) / 2;
            x98_text(&x98, win, lx + 28, ty, e->name, fg);

            /* 種類の列。Windows のエクスプローラの「種類」と同じ。
             * 窓が狭いときは名前と重なるので出さない。 */
            int type_x = lx + lw - 10 - SIZE_COL_W - TYPE_COL_W;
            if (type_x > lx + 28 + x98_text_w(&x98, e->name) + 12)
                x98_text(&x98, win, type_x, ty, type_name(e), fg);

            if (!e->is_dir) {
                /* 大きさの出し方は Windows のエクスプローラに寄せる。
                 *
                 * 前は 1KB 未満を「%ld B」で出していたが、実機で
                 * 「0 と出るのは壊れているのか」と読まれた。Windows は
                 * KB に切り上げて出すので、中身が 1 バイトでもあれば
                 * 1 KB になり、0 KB は**本当に空**のときだけになる。
                 * 数字の意味が 1 つに定まる。 */
                char sz[32];
                long kb = (e->size + 1023) / 1024;
                if (e->size > 0 && kb == 0) kb = 1;
                if (kb >= 10000)
                    snprintf(sz, sizeof(sz), "%ld MB", kb / 1024);
                else
                    snprintf(sz, sizeof(sz), "%ld KB", kb);
                int tw = x98_text_w(&x98, sz);
                x98_text(&x98, win, lx + lw - 10 - tw, ty, sz, fg);
            }
        }
    }
}

static void draw_ctx(void)
{
    if (!ctx_open) return;
    x98_fill(&x98, win, ctx_x, ctx_y, CTX_W, CTX_H, x98.face);
    x98_bevel(&x98, win, ctx_x, ctx_y, CTX_W, CTX_H, 1);
    for (int i = 0; i < CTX_N; i++) {
        int iy = ctx_y + 3 + i * CTX_IH;
        /* 今できないものは Windows と同じで薄く出す */
        int dim = 0;
        if (i == CTX_PASTE)      dim = !clip_path[0];
        else if (i == CTX_RUN)   dim = !has_run_verb(sel);
        else if (i == CTX_OPEN || i == CTX_RUNAS ||
                 i == CTX_COPY || i == CTX_CUT ||
                 i == CTX_RENAME || i == CTX_DELETE ||
                 i == CTX_DESKTOP)
            dim = (sel < 0);
        x98_text(&x98, win, ctx_x + 10, iy + (CTX_IH - x98_text_h(&x98)) / 2,
                 ctx_items[i], dim ? x98.shadow : x98.text);
    }
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, win_w, win_h, x98.face);
    draw_toolbar();
    draw_list();
    draw_ctx();
}

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void ensure_visible(void)
{
    int vis = rows_visible();
    if (sel < 0) return;
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;
}

static void ctx_action(int i)
{
    switch (i) {
    case CTX_OPEN:    open_entry_verb(sel, 0); break;
    case CTX_RUN:     if (has_run_verb(sel)) open_entry_verb(sel, 1); break;
    case CTX_RUNAS:   if (sel >= 0) open_entry_verb(sel, 2); break;
    case CTX_COPY:    do_copy(0); break;
    case CTX_CUT:     do_copy(1); break;
    case CTX_PASTE:   do_paste(); break;
    case CTX_RENAME:  start_rename(); break;
    case CTX_DELETE:  if (sel >= 0) confirm_delete = 1; break;
    case CTX_DESKTOP: add_to_desktop(sel); break;
    default: break;
    }
}

/* --- 本体 ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    signal(SIGCHLD, SIG_IGN);

    /* 日本語入力より前にロケールを立てる。X を開いたあとだと
     * Xlib が古いロケールのまま動いてしまう。 */
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[myos-files] cannot open display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);
    n_ftypes = ft_load(ftypes, FT_MAX);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask |
                           StructureNotifyMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XStoreName(dpy, win, "myOS Files");
    XMapWindow(dpy, win);

    /* 窓が出てから入力文脈を作る。窓より先に作ると
     * XNClientWindow に渡すものが無い。 */
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    go_to(argc > 1 ? argv[1] : "/");

    long last_click_ms = 0;
    int  last_click_row = -1;

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
            char kb[32];
            KeySym ks;
            int kn = x98_lookup(ic, &ev.xkey, kb, sizeof(kb), &ks);
            int ctrl = (ev.xkey.state & ControlMask) != 0;

            /* 名前の変更中はそちらが入力を全部取る */
            if (renaming) {
                if (ks == XK_Return || ks == XK_KP_Enter) commit_rename();
                else if (ks == XK_Escape) renaming = 0;
                else if (ks == XK_BackSpace) x98_edit_backspace(&rename_edit);
                else if (ks == XK_Delete) x98_edit_delete(&rename_edit);
                else if (ks == XK_Left && rename_edit.cur > 0)
                    rename_edit.cur = x98_u8_prev(rename_edit.buf,
                                                  rename_edit.cur);
                else if (ks == XK_Right && rename_edit.cur < rename_edit.len)
                    rename_edit.cur = x98_u8_next(rename_edit.buf,
                                                  rename_edit.cur,
                                                  rename_edit.len);
                else if (ks == XK_Home) rename_edit.cur = 0;
                else if (ks == XK_End)  rename_edit.cur = rename_edit.len;
                else if (kn > 0 && (unsigned char)kb[0] >= 0x20)
                    x98_edit_insert(&rename_edit, kb, kn);
                redraw();
                break;
            }

            if (ctx_open) { ctx_open = 0; redraw(); break; }

            if (ctrl) {
                if (ks == XK_c || ks == XK_C) do_copy(0);
                else if (ks == XK_x || ks == XK_X) do_copy(1);
                else if (ks == XK_v || ks == XK_V) do_paste();
                redraw();
                break;
            }

            switch (ks) {
            case XK_Up:        if (sel > 0) sel--; else sel = 0; break;
            case XK_Down:      if (sel < n_entries - 1) sel++; break;
            case XK_Prior:     sel -= rows_visible(); if (sel < 0) sel = 0; break;
            case XK_Next:      sel += rows_visible();
                               if (sel >= n_entries) sel = n_entries - 1; break;
            case XK_Home:      sel = 0; break;
            case XK_End:       sel = n_entries - 1; break;
            case XK_Return:
            case XK_KP_Enter:  open_entry(sel); break;
            case XK_BackSpace: go_up(); break;
            case XK_F2:        start_rename(); break;
            case XK_F5:        read_dir(); break;
            case XK_Delete:    if (sel >= 0) confirm_delete = 1; break;
            case XK_Escape:    confirm_delete = 0; break;
            default: break;
            }
            ensure_visible();
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            /* コンテキストメニューが開いていればそれを先に処理 */
            if (ctx_open) {
                if (mx >= ctx_x && mx < ctx_x + CTX_W &&
                    my >= ctx_y && my < ctx_y + CTX_H) {
                    int i = (my - ctx_y - 3) / CTX_IH;
                    ctx_open = 0;
                    if (i >= 0 && i < CTX_N) ctx_action(i);
                } else {
                    ctx_open = 0;
                }
                redraw();
                break;
            }

            if (renaming) { commit_rename(); redraw(); break; }

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
                    if (mx >= win_w - 104 && mx < win_w - 60) delete_selected();
                    confirm_delete = 0;
                } else if (mx >= TB_UP_X && mx < TB_UP_X + TB_UP_W) {
                    go_up();
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
            int row = (my - list_y()) / ROW_H;
            int i = top + row;

            if (ev.xbutton.button == 3) {
                /* 右クリック: その項目を選んでメニューを出す */
                if (row >= 0 && i < n_entries) sel = i;
                ctx_x = mx;
                ctx_y = my;
                if (ctx_x + CTX_W > win_w) ctx_x = win_w - CTX_W - 2;
                if (ctx_y + CTX_H > win_h) ctx_y = win_h - CTX_H - 2;
                ctx_open = 1;
                redraw();
                break;
            }

            if (row < 0 || i >= n_entries) { sel = -1; redraw(); break; }

            long t = now_ms();
            if (i == last_click_row && t - last_click_ms < x98.theme.dblclick_ms) {
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

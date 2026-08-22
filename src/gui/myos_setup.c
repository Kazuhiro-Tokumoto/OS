/* ==========================================================================
 * myos_setup.c  -  初回起動のセットアップ (Windows 98 のセットアップ画面)
 *
 * 初回だけ、デスクトップより先にこれが出る。
 * 青い背景に灰色のウィザードが乗った、あの画面。
 *
 * ここで決めること:
 *   1. ユーザー名          … 最初のユーザーは管理者になる (Windows と同じ)
 *   2. パスワード          … 管理者権限が要るときに聞かれるのがこれ
 *   3. 自動ログインの有無 / キーボード配列 / タイムゾーン
 *
 * root で動く。ここで作ったユーザーで、以後のデスクトップが動く。
 * 終わったら /etc/myos/setup-done を置くので、次からは出ない。
 *
 * ウィンドウマネージャがまだ居ないので override_redirect の全画面にして、
 * 自分でキーボードのフォーカスを取る。
 *
 * ビルド:
 *   gcc -O2 -o myos-setup myos_setup.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "x98.h"

/* 日本語入力の入力文脈。IME が上がっていなければ NULL のまま。 */
static XIC ic;

/* ウィザードの箱 */
#define PANEL_W 560
#define PANEL_H 380
#define TITLE_H 20
#define BTN_W   84
#define BTN_H   24

enum { ST_WELCOME = 0, ST_USER, ST_PASSWORD, ST_OPTIONS, ST_FINISH, ST_N };

static const char *step_title[ST_N] = {
    "myOS Setup",
    "User Information",
    "Password",
    "Settings",
    "Finishing Setup",
};

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static int      scr_w, scr_h;

static int      step = ST_WELCOME;
static int      applying = 0;
static char     err[128];

static X98Edit  ed_user;
static X98Edit  ed_pw1;
static X98Edit  ed_pw2;

/* --- キーボードだけで最後まで進めるようにする ----------------------------
 * インストール直後の初回起動は、マウスがまだ効かない機械がある
 * (PS/2 しか無い、USB の初期化が間に合っていない、等)。
 * そこで「自動ログインするか」を選べないと、パスワードを毎回聞かれる
 * 設定に変えられないまま先へ進むことになる。
 *
 * Windows のウィザードと同じ形にする。Tab で送り、Space で選び、
 * 矢印で値を変え、Enter は既定のボタン (Next) を押す。
 * 今どこに居るかは点線の枠で示す。 */
enum {
    W_USER = 0, W_PW1, W_PW2,
    W_AUTOLOGIN, W_KBD, W_TZ,
    W_BACK, W_NEXT, W_CANCEL
};

static const int f_welcome[]  = { W_NEXT, W_CANCEL };
static const int f_user[]     = { W_USER, W_BACK, W_NEXT, W_CANCEL };
static const int f_password[] = { W_PW1, W_PW2, W_BACK, W_NEXT, W_CANCEL };
static const int f_options[]  = { W_AUTOLOGIN, W_KBD, W_TZ,
                                  W_BACK, W_NEXT, W_CANCEL };
static const int f_finish[]   = { W_BACK, W_NEXT, W_CANCEL };

static int focus_ix = 0;

static const int *focus_list(int st, int *n)
{
    switch (st) {
    case ST_USER:     *n = 4; return f_user;
    case ST_PASSWORD: *n = 5; return f_password;
    case ST_OPTIONS:  *n = 6; return f_options;
    case ST_FINISH:   *n = 3; return f_finish;
    default:          *n = 2; return f_welcome;
    }
}

static int cur_focus(void)
{
    int n;
    const int *l = focus_list(step, &n);
    if (focus_ix < 0) focus_ix = 0;
    if (focus_ix >= n) focus_ix = n - 1;
    return l[focus_ix];
}

/* そのページに入ったときに、最初に触りたいところへ置く。 */
static void focus_reset(void)
{
    int n;
    const int *l = focus_list(step, &n);
    focus_ix = 0;
    for (int i = 0; i < n; i++)
        if (l[i] != W_BACK && l[i] != W_NEXT && l[i] != W_CANCEL) {
            focus_ix = i;
            return;
        }
    /* 入力するものが無いページ (Welcome / Finish) は Next に置く。 */
    for (int i = 0; i < n; i++)
        if (l[i] == W_NEXT) { focus_ix = i; return; }
}

static void focus_move(int d)
{
    int n;
    focus_list(step, &n);
    focus_ix = (focus_ix + d % n + n) % n;
}

static void focus_set(int widget)
{
    int n;
    const int *l = focus_list(step, &n);
    for (int i = 0; i < n; i++)
        if (l[i] == widget) { focus_ix = i; return; }
}

static int      autologin = 1;
static int      kbd = 0;            /* 0 = jp, 1 = us */
static int      tz  = 0;

static const char *kbd_name[2] = { "Japanese (jp)", "English (us)" };
static const char *kbd_code[2] = { "jp", "us" };

static const char *tz_name[] = {
    "Asia/Tokyo", "UTC", "Asia/Seoul", "Asia/Shanghai",
    "Europe/London", "Europe/Berlin",
    "America/New_York", "America/Los_Angeles",
};
#define TZ_N ((int)(sizeof(tz_name) / sizeof(tz_name[0])))

/* --- 画面の位置 ---------------------------------------------------------- */
static int px(void) { return (scr_w - PANEL_W) / 2; }
static int py(void) { return (scr_h - PANEL_H) / 2; }

/* ボタンの位置。描くときと押されたか調べるときで別々に計算していたので、
 * 片方だけ直すとずれる。1 か所にまとめる。 */
static void btn_geom(int which, int *x, int *y)
{
    static const int slot[3] = { 3, 2, 1 };     /* Back, Next, Cancel */
    static const int pad[3]  = { 30, 22, 14 };
    *x = px() + PANEL_W - slot[which] * BTN_W - pad[which];
    *y = py() + PANEL_H - BTN_H - 14;
}

/* --- 外部コマンド -------------------------------------------------------- */
/* シェルを通さずに実行して終わるまで待つ。戻り値は終了ステータス。
 *
 * useradd や passwd は /usr/sbin に居る。PID 1 から来た環境だと
 * PATH にそこが入っていないことがあるので、見つからなければ
 * sbin を直接あたる。 */
static int run(char *const argv[])
{
    static const char *sbin[] = { "/usr/sbin/", "/sbin/", NULL };

    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execvp(argv[0], argv);
        if (errno == ENOENT && argv[0][0] != '/') {
            char path[256];
            for (int i = 0; sbin[i]; i++) {
                snprintf(path, sizeof(path), "%s%s", sbin[i], argv[0]);
                execv(path, argv);
            }
        }
        _exit(127);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* パスワードは chpasswd の標準入力に渡す。
 * コマンドラインに載せると ps で丸見えになるため。 */
static int set_password(const char *user, const char *pass)
{
    int fd[2];
    if (pipe(fd) != 0) return -1;

    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (p == 0) {
        close(fd[1]);
        dup2(fd[0], 0);
        close(fd[0]);
        execlp("chpasswd", "chpasswd", (char *)NULL);
        execl("/usr/sbin/chpasswd", "chpasswd", (char *)NULL);
        _exit(127);
    }
    close(fd[0]);
    dprintf(fd[1], "%s:%s\n", user, pass);
    close(fd[1]);

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* --- 入力の検査 ---------------------------------------------------------- */
/* ユーザー名は useradd が通る形に限る。ここを緩めると
 * 変な名前でホームディレクトリが作れず、起動しなくなる。 */
static int valid_user(const char *s)
{
    if (!s[0] || strlen(s) > 31) return 0;
    if (!islower((unsigned char)s[0])) return 0;
    for (const char *p = s; *p; p++)
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) &&
            *p != '_' && *p != '-')
            return 0;
    /* 既にある名前は避ける (root や debian の既定ユーザーとぶつかる) */
    char path[256];
    snprintf(path, sizeof(path), "/home/%s", s);
    if (access(path, F_OK) == 0) return 0;
    if (!strcmp(s, "root")) return 0;
    return 1;
}

/* --- 適用 ---------------------------------------------------------------- */
static int apply_all(void)
{
    char *ua[] = { "useradd", "-m", "-s", "/bin/bash",
                   "-G", "sudo,audio,video,plugdev,cdrom,dialout",
                   ed_user.buf, NULL };
    int rc = run(ua);
    if (rc != 0) {
        /* 何で失敗したかが分からないと直しようが無いので、
         * useradd の終了コードをそのまま出す。
         * 127 = そもそも見つからない / 9 = 名前が使われている など。 */
        snprintf(err, sizeof(err),
                 "Could not create the user account (useradd exit %d).", rc);
        return 0;
    }
    rc = set_password(ed_user.buf, ed_pw1.buf);
    if (rc != 0) {
        snprintf(err, sizeof(err),
                 "Could not set the password (chpasswd exit %d).", rc);
        return 0;
    }

    /* root は直接ログインさせない。管理者の仕事は sudo 経由でやる。
     * (パスワード無しの root が残っていると、そこが素通しになる) */
    char *lk[] = { "passwd", "-l", "root", NULL };
    run(lk);

    /* タイムゾーン */
    char zone[128];
    snprintf(zone, sizeof(zone), "/usr/share/zoneinfo/%s", tz_name[tz]);
    char *ln[] = { "ln", "-sf", zone, "/etc/localtime", NULL };
    run(ln);
    FILE *f = fopen("/etc/timezone", "w");
    if (f) { fprintf(f, "%s\n", tz_name[tz]); fclose(f); }

    /* 時計を読み直す。
     *
     * myos-init は起動の早い段階で hwclock --hctosys を呼ぶが、
     * 初回起動のときはまだ /etc/localtime が無い。無いと libc は UTC と
     * みなすので、本体の時計に入っているローカル時刻を UTC として
     * 読んでしまい、日本だと 9 時間進んだ時刻になる。
     * (次の起動では直るが、初回だけずれたままになる)
     *
     * タイムゾーンが決まったこの場で読み直せば、その場で合う。 */
    char *hc[] = { "hwclock", "--hctosys", NULL };
    run(hc);

    /* キーボード配列。
     *
     * ここは 3 つに書く。1 つでは足りないことを実際に踏んだ。
     *
     *   /etc/default/keyboard      設定の置き場。あとから読み直す用。
     *                              X はこれを直接は見ない
     *                              (見るのは console-setup で、入れていない)。
     *   /etc/X11/xorg.conf.d/      X が次に起動したときに効く。
     *                              このディレクトリは Debian の既定では
     *                              存在しないので、先に作る。
     *                              作らずに fopen していたせいで書けておらず、
     *                              jp を選んでも us のままだった。
     *                              (@ を押すと [ が出る、という形で出る)
     *   setxkbmap                  いま動いている X に即座に効かせる。
     *                              これが無いと、初回セットアップの直後の
     *                              セッションだけ配列が古いままになる。
     *                              fcitx5 は起動時に X の配列を引き継ぐので、
     *                              fcitx5 より先にここで正しておく必要もある。
     */
    f = fopen("/etc/default/keyboard", "w");
    if (f) {
        fprintf(f, "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"%s\"\n"
                   "XKBVARIANT=\"\"\nXKBOPTIONS=\"\"\nBACKSPACE=\"guess\"\n",
                kbd_code[kbd]);
        fclose(f);
    }

    char *mkd[] = { "mkdir", "-p", "/etc/X11/xorg.conf.d", NULL };
    run(mkd);

    f = fopen("/etc/X11/xorg.conf.d/10-keyboard.conf", "w");
    if (f) {
        fprintf(f,
            "Section \"InputClass\"\n"
            "    Identifier \"myos keyboard\"\n"
            "    MatchIsKeyboard \"on\"\n"
            "    Option \"XkbLayout\" \"%s\"\n"
            "    Option \"XkbModel\" \"pc105\"\n"
            "EndSection\n", kbd_code[kbd]);
        fclose(f);
    } else {
        snprintf(err, sizeof(err),
                 "Could not write the keyboard layout "
                 "(/etc/X11/xorg.conf.d/10-keyboard.conf).");
        return 0;
    }

    char *skm[] = { "setxkbmap", "-model", "pc105",
                    "-layout", (char *)kbd_code[kbd], NULL };
    run(skm);

    /* 誰でログインするか / 自動ログインするか。
     * myos-init がこれを読んでセッションを起こす。 */
    f = fopen("/etc/myos/login.conf", "w");
    if (!f) {
        snprintf(err, sizeof(err), "Could not write /etc/myos/login.conf.");
        return 0;
    }
    fprintf(f, "# myOS: 誰でデスクトップを動かすか\n");
    fprintf(f, "user      = %s\n", ed_user.buf);
    fprintf(f, "autologin = %d\n", autologin ? 1 : 0);
    fclose(f);

    f = fopen("/etc/myos/setup-done", "w");
    if (f) { fprintf(f, "%s\n", ed_user.buf); fclose(f); }
    return 1;
}

/* --- 描画 ---------------------------------------------------------------- */
static void label(int x, int y, const char *s)
{
    x98_text(&x98, win, px() + x, py() + y, s, x98.text);
}

static void edit_box(int x, int y, int w, X98Edit *e, int focus, int mask)
{
    int bx = px() + x, by = py() + y;
    x98_bevel(&x98, win, bx, by, w, 20, 0);
    x98_fill(&x98, win, bx + 2, by + 2, w - 4, 16, x98.white);

    const char *show = e->buf;
    char stars[X98_EDIT_MAX];
    if (mask) {
        int n = e->len < (int)sizeof(stars) - 1 ? e->len : (int)sizeof(stars) - 1;
        for (int i = 0; i < n; i++) stars[i] = '*';
        stars[n] = 0;
        show = stars;
    }
    int ty = by + (20 - x98_text_h(&x98)) / 2;
    x98_text(&x98, win, bx + 4, ty, show, x98.text);
    if (focus)
        x98_vline(&x98, win, bx + 4 + x98_text_w(&x98, show), by + 3, 14,
                  x98.text);
}

/* 98 の「今ここ」の点線。1 画素おきに点を打つだけ。 */
static void focus_rect(int x, int y, int w, int h)
{
    XSetForeground(dpy, x98.gc, x98.text);
    for (int i = 0; i < w; i += 2) {
        XDrawPoint(dpy, win, x98.gc, x + i, y);
        XDrawPoint(dpy, win, x98.gc, x + i, y + h - 1);
    }
    for (int i = 0; i < h; i += 2) {
        XDrawPoint(dpy, win, x98.gc, x, y + i);
        XDrawPoint(dpy, win, x98.gc, x + w - 1, y + i);
    }
}

static void checkbox(int x, int y, int on, const char *text, int focused)
{
    int bx = px() + x, by = py() + y;
    x98_bevel(&x98, win, bx, by, 13, 13, 0);
    x98_fill(&x98, win, bx + 2, by + 2, 9, 9, x98.white);
    if (on) {
        /* 小さいチェック。線 2 本で十分それらしく見える。 */
        XSetForeground(dpy, x98.gc, x98.text);
        XDrawLine(dpy, win, x98.gc, bx + 3, by + 6, bx + 5, by + 9);
        XDrawLine(dpy, win, x98.gc, bx + 5, by + 9, bx + 10, by + 3);
    }
    x98_text(&x98, win, bx + 20, by + (13 - x98_text_h(&x98)) / 2 - 1,
             text, x98.text);
    if (focused)
        focus_rect(bx + 17, by - 2, x98_text_w(&x98, text) + 6, 17);
}

static void radio(int x, int y, int on, const char *text, int focused)
{
    int bx = px() + x, by = py() + y;
    x98_bevel(&x98, win, bx, by, 13, 13, 0);
    x98_fill(&x98, win, bx + 2, by + 2, 9, 9, x98.white);
    if (on) x98_fill(&x98, win, bx + 4, by + 4, 5, 5, x98.text);
    x98_text(&x98, win, bx + 20, by + (13 - x98_text_h(&x98)) / 2 - 1,
             text, x98.text);
    if (focused)
        focus_rect(bx + 17, by - 2, x98_text_w(&x98, text) + 6, 17);
}

static void draw_page(void)
{
    switch (step) {
    case ST_WELCOME:
        label(24, 56, "Welcome to myOS.");
        label(24, 88, "Setup will prepare this computer for you.");
        label(24, 116, "It takes about a minute. You will be asked for:");
        label(24, 144, "  - a user name");
        label(24, 162, "  - a password");
        label(24, 180, "  - your keyboard layout and time zone");
        label(24, 220, "Press Next to continue.");
        label(24, 250, "Mouse not working? Tab moves, Space selects,");
        label(24, 268, "Enter is Next. The whole setup works from the");
        label(24, 286, "keyboard alone.");
        break;

    case ST_USER:
        label(24, 50, "Who will use this computer?");
        label(24, 78, "This account can install software and change");
        label(24, 96, "system settings.");
        label(24, 134, "User name:");
        edit_box(120, 130, 260, &ed_user, cur_focus() == W_USER, 0);
        label(24, 162, "Lower-case letters, digits, - and _ only.");
        break;

    case ST_PASSWORD:
        label(24, 50, "Choose a password.");
        label(24, 74, "You will be asked for it whenever something needs");
        label(24, 92, "administrator privileges.");
        label(24, 130, "Password:");
        edit_box(140, 126, 240, &ed_pw1, cur_focus() == W_PW1, 1);
        label(24, 160, "Confirm:");
        edit_box(140, 156, 240, &ed_pw2, cur_focus() == W_PW2, 1);
        label(24, 190, "Tab moves between the boxes and the buttons.");
        break;

    case ST_OPTIONS: {
        int f = cur_focus();

        label(24, 46, "Logon");
        checkbox(36, 68, autologin, "Log on automatically at startup",
                 f == W_AUTOLOGIN);
        label(36, 90, "(off = ask for the password every time)");

        label(24, 122, "Keyboard layout");
        radio(36, 144, kbd == 0, kbd_name[0], f == W_KBD && kbd == 0);
        radio(36, 164, kbd == 1, kbd_name[1], f == W_KBD && kbd == 1);

        label(24, 196, "Time zone");
        {
            int bx = px() + 36, by = py() + 216;
            x98_bevel(&x98, win, bx, by, 260, 20, 0);
            x98_fill(&x98, win, bx + 2, by + 2, 256, 16, x98.white);
            x98_text(&x98, win, bx + 6,
                     by + (20 - x98_text_h(&x98)) / 2, tz_name[tz], x98.text);
            if (f == W_TZ) focus_rect(bx + 2, by + 2, 256, 16);
        }
        /* マウスが効かない機械があるので、操作を必ず画面に出す。
         * 「選べない」と思われるのが一番まずい。 */
        label(24, 250, "Tab = move   Space = select   Up/Down = change value");
        label(24, 268, "Enter = Next     The clock comes from the hardware.");
        break;
    }

    case ST_FINISH: {
        char buf[160];
        label(24, 50, applying ? "Setting things up, please wait..."
                               : "Setup is ready to apply these settings.");
        snprintf(buf, sizeof(buf), "User name  :  %s", ed_user.buf);
        label(24, 90, buf);
        snprintf(buf, sizeof(buf), "Logon      :  %s",
                 autologin ? "automatic" : "ask for password");
        label(24, 112, buf);
        snprintf(buf, sizeof(buf), "Keyboard   :  %s", kbd_name[kbd]);
        label(24, 134, buf);
        snprintf(buf, sizeof(buf), "Time zone  :  %s", tz_name[tz]);
        label(24, 156, buf);
        label(24, 196, "Press Finish to apply and start the desktop.");
        break;
    }
    }

    if (err[0]) label(24, PANEL_H - 70, err);
}

static void redraw(void)
{
    /* 背景。Win98 のセットアップのあの青。 */
    x98_fill(&x98, win, 0, 0, scr_w, scr_h, x98_rgb24(&x98, 0x000080));
    x98_text(&x98, win, 20, 16, "myOS Setup", x98.white);
    x98_hline(&x98, win, 0, 36, scr_w, x98_rgb24(&x98, 0x1084D0));

    int bx = px(), by = py();
    x98_fill(&x98, win, bx, by, PANEL_W, PANEL_H, x98.face);
    x98_bevel(&x98, win, bx, by, PANEL_W, PANEL_H, 1);
    x98_titlebar(&x98, win, bx + 3, by + 3, PANEL_W - 6, TITLE_H,
                 step_title[step]);

    draw_page();

    /* ボタン。Windows のウィザードと同じ並び。 */
    int f = cur_focus();
    int byy = by + PANEL_H - BTN_H - 14;
    int gx, gy;
    if (step > ST_WELCOME) {
        btn_geom(0, &gx, &gy);
        x98_button(&x98, win, gx, gy, BTN_W, BTN_H, "< Back", 0);
        if (f == W_BACK) focus_rect(gx + 4, gy + 4, BTN_W - 8, BTN_H - 8);
    }
    btn_geom(1, &gx, &gy);
    x98_button(&x98, win, gx, gy, BTN_W, BTN_H,
               step == ST_FINISH ? "Finish" : "Next >", 0);
    if (f == W_NEXT) focus_rect(gx + 4, gy + 4, BTN_W - 8, BTN_H - 8);
    btn_geom(2, &gx, &gy);
    x98_button(&x98, win, gx, gy, BTN_W, BTN_H, "Cancel", 0);
    if (f == W_CANCEL) focus_rect(gx + 4, gy + 4, BTN_W - 8, BTN_H - 8);

    /* ページ番号。長さの見当が付くだけで気分が違う。 */
    char pg[32];
    snprintf(pg, sizeof(pg), "Step %d of %d", step + 1, ST_N);
    x98_text(&x98, win, bx + 14, byy + 6, pg, x98.shadow);
}

/* --- 進む / 戻る --------------------------------------------------------- */
static int can_advance(void)
{
    err[0] = 0;
    switch (step) {
    case ST_USER:
        if (!valid_user(ed_user.buf)) {
            snprintf(err, sizeof(err),
                     "That user name cannot be used. Try another one.");
            return 0;
        }
        return 1;
    case ST_PASSWORD:
        if (ed_pw1.len < 4) {
            snprintf(err, sizeof(err),
                     "The password must be at least 4 characters.");
            return 0;
        }
        if (strcmp(ed_pw1.buf, ed_pw2.buf)) {
            snprintf(err, sizeof(err), "The two passwords do not match.");
            return 0;
        }
        return 1;
    default:
        return 1;
    }
}

static int do_next(void)
{
    if (!can_advance()) return 0;

    if (step == ST_FINISH) {
        applying = 1;
        redraw();
        XFlush(dpy);
        if (apply_all()) return 1;     /* 終わり */
        applying = 0;
        return 0;
    }
    step++;
    focus_reset();
    return 0;
}

/* 戻る。ページの内容は残したまま。 */
static void do_back(void)
{
    if (step <= ST_WELCOME) return;
    step--;
    err[0] = 0;
    focus_reset();
}

int main(void)
{
    /* 日本語入力より前にロケールを立てる。X を開いたあとだと
     * Xlib が古いロケールのまま動いてしまう。 */
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-setup: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, screen);
    scr_h = DisplayHeight(dpy, screen);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    /* ウィンドウマネージャがまだ居ないので、枠の要らない全画面にする。 */
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = x98_rgb24(&x98, 0x000080);
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;
    win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, scr_w, scr_h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
    XStoreName(dpy, win, "myOS Setup");
    XMapRaised(dpy, win);

    /* 窓が出てから入力文脈を作る。窓より先に作ると
     * XNClientWindow に渡すものが無い。 */
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    /* 自分でフォーカスを取る。誰も回してくれないので。 */
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    x98_edit_set(&ed_user, "");
    x98_edit_set(&ed_pw1, "");
    x98_edit_set(&ed_pw2, "");
    focus_reset();

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        /* IME が使う鍵はここで吸われる。忘れると
         * かなも漢字も一生入ってこない。 */
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
            redraw();
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);
            int f = cur_focus();
            int shift = (ev.xkey.state & ShiftMask) != 0;
            X98Edit *e = NULL;

            if (f == W_USER)     e = &ed_user;
            else if (f == W_PW1) e = &ed_pw1;
            else if (f == W_PW2) e = &ed_pw2;
            if (ks == XK_Tab) {
                focus_move(shift ? -1 : 1);
            } else if (ks == XK_Escape) {
                /* ESC では抜けない。
                 *
                 * ここを抜けると使う人が作られないまま、root のまま
                 * デスクトップが出る。直せる画面が出るほうがよい、と
                 * 思って逃げ道を用意してあるのだが、**ESC は事故で
                 * 押せる**。指が滑っただけで root の机に着いてしまう。
                 *
                 * 逃げ道は Cancel のボタンに残してある。あれは狙って
                 * 押すものなので、事故では踏まない。代償の重い出口は、
                 * それなりの手間の先に置く。 */
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                /* Enter は既定のボタン。ただしフォーカスが Back や Cancel に
                 * 乗っているときは、そちらを押したことにする。 */
                if (f == W_BACK)        do_back();
                else if (f == W_CANCEL) goto done;
                else if (do_next())     goto done;
            } else if (ks == XK_space) {
                /* Space は「今いるところ」を操作する。ただし入力欄では
                 * ただの空白なので、そちらを優先する。 */
                if (f == W_AUTOLOGIN)   autologin = !autologin;
                else if (f == W_KBD)    kbd ^= 1;
                else if (f == W_BACK)   do_back();
                else if (f == W_CANCEL) goto done;
                else if (f == W_NEXT) { if (do_next()) goto done; }
                else if (e) x98_edit_insert(e, " ", 1);
            } else if (ks == XK_Up || ks == XK_Down ||
                       ks == XK_Left || ks == XK_Right) {
                int back = (ks == XK_Up || ks == XK_Left);
                if (f == W_KBD) {
                    kbd = back ? 0 : 1;
                } else if (f == W_TZ) {
                    if (back) { if (tz > 0) tz--; }
                    else      { if (tz < TZ_N - 1) tz++; }
                } else if (f == W_AUTOLOGIN) {
                    /* チェック 1 個だけなので、上下でも入れ替えてよい。 */
                    autologin = !autologin;
                } else {
                    /* 値を持たないところでは、上下をページ送りに使う。
                     * ボタンの上で左右を押したときも移動でよい。 */
                    focus_move(back ? -1 : 1);
                }
            } else if (ks == XK_BackSpace) {
                if (e) x98_edit_backspace(e);
            } else if (e && n > 0 && (unsigned char)buf[0] >= 0x20) {
                buf[n] = 0;
                x98_edit_insert(e, buf, n);
            }
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;
            int bx = px(), by = py();
            int gx, gy;

            btn_geom(1, &gx, &gy);
            if (my >= gy && my < gy + BTN_H) {
                int hit = -1;
                for (int i = 0; i < 3; i++) {
                    int tx, ty;
                    btn_geom(i, &tx, &ty);
                    if (mx >= tx && mx < tx + BTN_W) { hit = i; break; }
                }
                if (hit == 0 && step > ST_WELCOME) {
                    focus_set(W_BACK);
                    do_back();
                } else if (hit == 1) {
                    focus_set(W_NEXT);
                    if (do_next()) goto done;
                } else if (hit == 2) {
                    /* Cancel。設定を書かずに抜ける。
                     * setup-done を置かないので、次の起動でまた出る。 */
                    goto done;
                }
            } else if (step == ST_OPTIONS) {
                int rx = mx - bx, ry = my - by;
                if (rx >= 36 && rx < 300) {
                    if (ry >= 68 && ry < 84) {
                        autologin = !autologin; focus_set(W_AUTOLOGIN);
                    } else if (ry >= 144 && ry < 160) {
                        kbd = 0; focus_set(W_KBD);
                    } else if (ry >= 164 && ry < 180) {
                        kbd = 1; focus_set(W_KBD);
                    } else if (ry >= 216 && ry < 236) {
                        focus_set(W_TZ);
                    }
                }
            } else if (step == ST_PASSWORD) {
                int ry = my - by;
                if (ry >= 126 && ry < 146)      focus_set(W_PW1);
                else if (ry >= 156 && ry < 176) focus_set(W_PW2);
            }
            redraw();
            break;
        }
        }
    }

done:
    /* パスワードを持ったまま終わらない */
    memset(ed_pw1.buf, 0, sizeof(ed_pw1.buf));
    memset(ed_pw2.buf, 0, sizeof(ed_pw2.buf));
    XCloseDisplay(dpy);
    return 0;
}

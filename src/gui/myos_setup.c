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
static int      field = 0;          /* そのページの何番目の入力欄か */

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

    /* キーボード配列。X はこのファイルではなく xorg.conf.d を見るので、
     * そちらにも書いておく。 */
    f = fopen("/etc/default/keyboard", "w");
    if (f) {
        fprintf(f, "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"%s\"\n"
                   "XKBVARIANT=\"\"\nXKBOPTIONS=\"\"\nBACKSPACE=\"guess\"\n",
                kbd_code[kbd]);
        fclose(f);
    }
    f = fopen("/etc/X11/xorg.conf.d/10-keyboard.conf", "w");
    if (f) {
        fprintf(f,
            "Section \"InputClass\"\n"
            "    Identifier \"myos keyboard\"\n"
            "    MatchIsKeyboard \"on\"\n"
            "    Option \"XkbLayout\" \"%s\"\n"
            "EndSection\n", kbd_code[kbd]);
        fclose(f);
    }

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

static void checkbox(int x, int y, int on, const char *text)
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
}

static void radio(int x, int y, int on, const char *text)
{
    int bx = px() + x, by = py() + y;
    x98_bevel(&x98, win, bx, by, 13, 13, 0);
    x98_fill(&x98, win, bx + 2, by + 2, 9, 9, x98.white);
    if (on) x98_fill(&x98, win, bx + 4, by + 4, 5, 5, x98.text);
    x98_text(&x98, win, bx + 20, by + (13 - x98_text_h(&x98)) / 2 - 1,
             text, x98.text);
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
        break;

    case ST_USER:
        label(24, 50, "Who will use this computer?");
        label(24, 78, "This account can install software and change");
        label(24, 96, "system settings.");
        label(24, 134, "User name:");
        edit_box(120, 130, 260, &ed_user, 1, 0);
        label(24, 162, "Lower-case letters, digits, - and _ only.");
        break;

    case ST_PASSWORD:
        label(24, 50, "Choose a password.");
        label(24, 74, "You will be asked for it whenever something needs");
        label(24, 92, "administrator privileges.");
        label(24, 130, "Password:");
        edit_box(140, 126, 240, &ed_pw1, field == 0, 1);
        label(24, 160, "Confirm:");
        edit_box(140, 156, 240, &ed_pw2, field == 1, 1);
        label(24, 190, "Tab switches between the two boxes.");
        break;

    case ST_OPTIONS:
        label(24, 46, "Logon");
        checkbox(36, 68, autologin, "Log on automatically at startup");
        label(36, 90, "(off = ask for the password every time)");

        label(24, 122, "Keyboard layout");
        radio(36, 144, kbd == 0, kbd_name[0]);
        radio(36, 164, kbd == 1, kbd_name[1]);

        label(24, 196, "Time zone   (up / down keys)");
        {
            int bx = px() + 36, by = py() + 216;
            x98_bevel(&x98, win, bx, by, 260, 20, 0);
            x98_fill(&x98, win, bx + 2, by + 2, 256, 16, x98.white);
            x98_text(&x98, win, bx + 6,
                     by + (20 - x98_text_h(&x98)) / 2, tz_name[tz], x98.text);
        }
        label(24, 250, "The clock is read from this computer's hardware.");
        break;

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
    int byy = by + PANEL_H - BTN_H - 14;
    if (step > ST_WELCOME)
        x98_button(&x98, win, bx + PANEL_W - 3 * BTN_W - 30, byy,
                   BTN_W, BTN_H, "< Back", 0);
    x98_button(&x98, win, bx + PANEL_W - 2 * BTN_W - 22, byy, BTN_W, BTN_H,
               step == ST_FINISH ? "Finish" : "Next >", 0);
    x98_button(&x98, win, bx + PANEL_W - BTN_W - 14, byy, BTN_W, BTN_H,
               "Cancel", 0);

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
    field = 0;
    return 0;
}

int main(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-setup: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, screen);
    scr_h = DisplayHeight(dpy, screen);
    x98_init(&x98, dpy, screen);

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

    /* 自分でフォーカスを取る。誰も回してくれないので。 */
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    x98_edit_set(&ed_user, "");
    x98_edit_set(&ed_pw1, "");
    x98_edit_set(&ed_pw2, "");

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
            redraw();
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
            X98Edit *e = NULL;

            if (step == ST_USER) e = &ed_user;
            else if (step == ST_PASSWORD) e = field ? &ed_pw2 : &ed_pw1;

            if (ks == XK_Return || ks == XK_KP_Enter) {
                if (do_next()) goto done;
            } else if (ks == XK_Tab) {
                if (step == ST_PASSWORD) field ^= 1;
            } else if (ks == XK_BackSpace) {
                if (e) x98_edit_backspace(e);
            } else if (step == ST_OPTIONS) {
                if (ks == XK_Up)        { if (tz > 0) tz--; }
                else if (ks == XK_Down) { if (tz < TZ_N - 1) tz++; }
                else if (ks == XK_space) autologin = !autologin;
                else if (ks == XK_Left || ks == XK_Right) kbd ^= 1;
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
            int byy = by + PANEL_H - BTN_H - 14;

            if (my >= byy && my < byy + BTN_H) {
                if (step > ST_WELCOME &&
                    mx >= bx + PANEL_W - 3 * BTN_W - 30 &&
                    mx <  bx + PANEL_W - 3 * BTN_W - 30 + BTN_W) {
                    step--;
                    field = 0;
                    err[0] = 0;
                } else if (mx >= bx + PANEL_W - 2 * BTN_W - 22 &&
                           mx <  bx + PANEL_W - 2 * BTN_W - 22 + BTN_W) {
                    if (do_next()) goto done;
                } else if (mx >= bx + PANEL_W - BTN_W - 14 &&
                           mx <  bx + PANEL_W - 14) {
                    /* Cancel。設定を書かずに抜ける。
                     * setup-done を置かないので、次の起動でまた出る。 */
                    goto done;
                }
            } else if (step == ST_OPTIONS) {
                int rx = mx - bx, ry = my - by;
                if (rx >= 36 && rx < 300) {
                    if (ry >= 68 && ry < 84)        autologin = !autologin;
                    else if (ry >= 144 && ry < 160) kbd = 0;
                    else if (ry >= 164 && ry < 180) kbd = 1;
                }
            } else if (step == ST_PASSWORD) {
                int ry = my - by;
                if (ry >= 126 && ry < 146)      field = 0;
                else if (ry >= 156 && ry < 176) field = 1;
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

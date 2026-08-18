/* ==========================================================================
 * myos_login.c  -  ログオン画面 (Windows 98 の「Windows へようこそ」)
 *
 * 自動ログインを切ってあるときだけ、デスクトップより先に出る。
 * 通ったらユーザー名を標準出力に 1 行書いて終わる。
 * セッションのスクリプトがそれを受け取って、そのユーザーで
 * デスクトップを起動する。
 *
 * 照合の仕方:
 *   /etc/shadow のハッシュを取り出し、crypt() に同じ塩で掛け直して比べる。
 *   PAM は使わない。X から使うには話が大きすぎるのと、
 *   ここでやりたいのは「本人か」の一点だけなので。
 *   /etc/shadow は root しか読めないので、この画面も root で動かす。
 *
 * 弾く条件:
 *   - ハッシュが空 (パスワード無し) や "!"/"*" で始まる (ロック中) 場合は
 *     どんな入力でも通さない。ここを通すと素通しになる。
 *
 * ビルド:
 *   gcc -O2 -o myos-login myos_login.c -lX11 -lcrypt
 * ========================================================================== */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crypt.h>
#include <shadow.h>

#include "x98.h"
#include "myosconf.h"

#define PANEL_W 400
#define PANEL_H 210
#define TITLE_H 20
#define BTN_W   80
#define BTN_H   24

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static int      scr_w, scr_h;

static X98Edit  ed_user;
static X98Edit  ed_pw;
static int      field = 0;
static int      failed = 0;

static int px(void) { return (scr_w - PANEL_W) / 2; }
static int py(void) { return (scr_h - PANEL_H) / 2; }

/* login.conf から既定のユーザー名を拾う。無ければ空のまま。 */
static void default_user(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(MYOS_ETC "/login.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = myos_trim(line);
        if (strncmp(s, "user", 4)) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        snprintf(out, n, "%s", myos_trim(eq + 1));
        break;
    }
    fclose(f);
}

/* パスワードが合っているか。合っていれば 1。 */
static int check_password(const char *user, const char *pass)
{
    if (!user[0]) return 0;

    struct spwd *sp = getspnam(user);
    if (!sp || !sp->sp_pwdp) return 0;

    const char *hash = sp->sp_pwdp;

    /* パスワード無し / ロック中のアカウントは通さない */
    if (!hash[0] || hash[0] == '!' || hash[0] == '*') return 0;

    struct crypt_data cd;
    memset(&cd, 0, sizeof(cd));
    const char *got = crypt_r(pass, hash, &cd);
    int ok = got && !strcmp(got, hash);
    memset(&cd, 0, sizeof(cd));
    return ok;
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
    x98_text(&x98, win, bx + 4, by + (20 - x98_text_h(&x98)) / 2, show,
             x98.text);
    if (focus)
        x98_vline(&x98, win, bx + 4 + x98_text_w(&x98, show), by + 3, 14,
                  x98.text);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, scr_w, scr_h, x98.desktop);

    int bx = px(), by = py();
    x98_fill(&x98, win, bx, by, PANEL_W, PANEL_H, x98.face);
    x98_bevel(&x98, win, bx, by, PANEL_W, PANEL_H, 1);
    x98_titlebar(&x98, win, bx + 3, by + 3, PANEL_W - 6, TITLE_H,
                 "Welcome to myOS");

    x98_text(&x98, win, bx + 20, by + 40,
             "Type your user name and password to log on.", x98.text);

    x98_text(&x98, win, bx + 20, by + 76, "User name:", x98.text);
    edit_box(120, 72, 240, &ed_user, field == 0, 0);
    x98_text(&x98, win, bx + 20, by + 106, "Password:", x98.text);
    edit_box(120, 102, 240, &ed_pw, field == 1, 1);

    if (failed)
        x98_text(&x98, win, bx + 20, by + 134,
                 "The user name or password is incorrect.", x98.text);

    int byy = by + PANEL_H - BTN_H - 16;
    x98_button(&x98, win, bx + PANEL_W - 2 * BTN_W - 26, byy, BTN_W, BTN_H,
               "OK", 0);
    x98_button(&x98, win, bx + PANEL_W - BTN_W - 16, byy, BTN_W, BTN_H,
               "Shut Down", 0);
}

/* 通ったらユーザー名を出して終わる。呼び出し側がそれを使う。 */
static void succeed(void)
{
    printf("%s\n", ed_user.buf);
    fflush(stdout);
    memset(ed_pw.buf, 0, sizeof(ed_pw.buf));
    XCloseDisplay(dpy);
    exit(0);
}

static void try_login(void)
{
    if (check_password(ed_user.buf, ed_pw.buf)) succeed();
    failed = 1;
    field = 1;
    x98_edit_set(&ed_pw, "");
}

int main(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-login: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, screen);
    scr_h = DisplayHeight(dpy, screen);
    x98_init(&x98, dpy, screen);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = x98.desktop;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;
    win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, scr_w, scr_h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
    XStoreName(dpy, win, "myOS Logon");
    XMapRaised(dpy, win);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    char du[64];
    default_user(du, sizeof(du));
    x98_edit_set(&ed_user, du);
    x98_edit_set(&ed_pw, "");
    field = du[0] ? 1 : 0;      /* 名前が入っていればパスワードから */

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
            X98Edit *e = field ? &ed_pw : &ed_user;

            if (ks == XK_Return || ks == XK_KP_Enter) {
                if (field == 0 && ed_user.len) field = 1;
                else try_login();
            } else if (ks == XK_Tab) {
                field ^= 1;
            } else if (ks == XK_BackSpace) {
                x98_edit_backspace(e);
            } else if (n > 0 && (unsigned char)buf[0] >= 0x20) {
                buf[n] = 0;
                x98_edit_insert(e, buf, n);
            }
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x - px(), my = ev.xbutton.y - py();
            int byy = PANEL_H - BTN_H - 16;

            if (my >= byy && my < byy + BTN_H) {
                if (mx >= PANEL_W - 2 * BTN_W - 26 &&
                    mx <  PANEL_W - 2 * BTN_W - 26 + BTN_W) {
                    try_login();
                } else if (mx >= PANEL_W - BTN_W - 16 &&
                           mx <  PANEL_W - 16) {
                    /* 電源を切る。ログオンしていないので、
                     * ここから出来ることはこれくらいしかない。 */
                    execlp("poweroff", "poweroff", (char *)NULL);
                    _exit(0);
                }
            } else if (my >= 72 && my < 92) {
                field = 0;
            } else if (my >= 102 && my < 122) {
                field = 1;
            }
            redraw();
            break;
        }
        }
    }
}

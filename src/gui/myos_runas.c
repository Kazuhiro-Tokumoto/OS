/* ==========================================================================
 * myos_runas.c  -  管理者として実行する (Windows の UAC / 「別のユーザーとして実行」)
 *
 *   myos-runas <コマンド> [引数...]
 *
 * Win98 風の見た目で「この操作には管理者権限が必要です」と出し、
 * パスワードを受け取って sudo 経由でコマンドを起動する。
 *
 * 実際の可否判定は自分でやらない。sudo に任せる。
 *   - sudo グループに居ない人は、正しいパスワードを入れても弾かれる
 *   - パスワードの照合も sudo (PAM) がやる
 * 自作の判定を挟むと、そこがそのまま穴になるため。
 *
 * パスワードは sudo -S の標準入力へ渡し、渡し終えたらすぐ 0 で潰す。
 * コマンドラインには絶対に載せない (ps で丸見えになるため)。
 *
 * ビルド:
 *   gcc -O2 -o myos-runas myos_runas.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include "x98.h"

/* 日本語入力の入力文脈。IME が上がっていなければ NULL のまま。 */
static XIC ic;

#define WIN_W 420
#define WIN_H 190

#define BTN_W  76
#define BTN_H  23

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static Atom     a_wm_delete;

static X98Edit  pw;
static char     cmdline[1024];
static char     msg[160];
static int      failed = 0;

/* パスワードは伏せて描く。長さだけ分かるように「*」を並べる。 */
static void draw_password(int px, int py, int w, int h)
{
    x98_bevel(&x98, win, px, py, w, h, 0);
    x98_fill(&x98, win, px + 2, py + 2, w - 4, h - 4, x98.white);

    char stars[X98_EDIT_MAX];
    int n = pw.len < (int)sizeof(stars) - 1 ? pw.len : (int)sizeof(stars) - 1;
    for (int i = 0; i < n; i++) stars[i] = '*';
    stars[n] = 0;

    int ty = py + (h - x98_text_h(&x98)) / 2;
    x98_text(&x98, win, px + 4, ty, stars, x98.text);

    int cx = px + 4 + x98_text_w(&x98, stars);
    x98_vline(&x98, win, cx, py + 3, h - 6, x98.text);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);

    x98_text(&x98, win, 14, 14, "This operation requires administrator",
             x98.text);
    x98_text(&x98, win, 14, 30, "privileges.", x98.text);

    x98_text(&x98, win, 14, 56, msg, x98.text);

    x98_text(&x98, win, 14, 88, "Password:", x98.text);
    draw_password(90, 84, WIN_W - 90 - 14, 20);

    if (failed)
        x98_text(&x98, win, 14, 112,
                 "Wrong password, or you are not an administrator.",
                 x98.text);

    x98_button(&x98, win, WIN_W - 2 * BTN_W - 24, WIN_H - BTN_H - 14,
               BTN_W, BTN_H, "OK", 0);
    x98_button(&x98, win, WIN_W - BTN_W - 14, WIN_H - BTN_H - 14,
               BTN_W, BTN_H, "Cancel", 0);
}

/* sudo にパスワードを食わせて実行する。
 * 成功したら二度と戻らない (このプロセスは用済み)。 */
static int try_run(char **argv)
{
    int fd[2];
    if (pipe(fd) != 0) return 0;

    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return 0; }

    if (p == 0) {
        close(fd[1]);
        dup2(fd[0], 0);
        close(fd[0]);
        /* -S: パスワードを標準入力から読む
         * -k: 前回の認証を使い回さない。毎回きちんと聞く
         * -p: sudo 自身のプロンプトは出さない (画面に出る先が無い) */
        execvp("sudo", argv);
        _exit(127);
    }

    close(fd[0]);
    ssize_t ign;
    ign = write(fd[1], pw.buf, pw.len);
    ign = write(fd[1], "\n", 1);
    (void)ign;
    close(fd[1]);

    /* 渡し終えたら手元からは消す */
    memset(pw.buf, 0, sizeof(pw.buf));
    pw.len = pw.cur = 0;

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }

    /* sudo は認証に失敗すると 1 で終わる。
     * 認証が通れば、あとは起動したコマンド次第なので成功とみなす。 */
    return WIFEXITED(st) && WEXITSTATUS(st) != 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: myos-runas <command> [args...]\n");
        return 2;
    }

    /* もともと root なら聞く意味が無い。そのまま実行する。 */
    if (geteuid() == 0) {
        execvp(argv[1], argv + 1);
        perror("myos-runas");
        return 127;
    }

    cmdline[0] = 0;
    for (int i = 1; i < argc; i++) {
        strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 2);
        if (i + 1 < argc)
            strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 2);
    }
    snprintf(msg, sizeof(msg), "%.150s", cmdline);

    /* sudo に渡す argv を先に組み立てておく */
    char *sargv[64];
    int   sn = 0;
    sargv[sn++] = "sudo";
    sargv[sn++] = "-S";
    sargv[sn++] = "-k";
    sargv[sn++] = "-p";
    sargv[sn++] = "";
    for (int i = 1; i < argc && sn < 62; i++) sargv[sn++] = argv[i];
    sargv[sn] = NULL;

    /* 日本語入力より前にロケールを立てる。X を開いたあとだと
     * Xlib が古いロケールのまま動いてしまう。 */
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-runas: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, x98.shadow, x98.face);
    XStoreName(dpy, win, "Administrator Password");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    /* 大きさを固定する。入力欄しか無いので伸ばす意味が無い。 */
    XSizeHints hints;
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = WIN_W;
    hints.min_height = hints.max_height = WIN_H;
    XSetWMNormalHints(dpy, win, &hints);

    XMapRaised(dpy, win);

    /* 窓が出てから入力文脈を作る。窓より先に作ると
     * XNClientWindow に渡すものが無い。 */
    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    x98_edit_set(&pw, "");

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        /* IME が使う鍵はここで吸われる。忘れると
         * かなも漢字も一生入ってこない。 */
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            redraw();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a_wm_delete) goto out;
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);

            if (ks == XK_Return || ks == XK_KP_Enter) {
                if (try_run(sargv)) goto out;
                failed = 1;
                x98_edit_set(&pw, "");
            } else if (ks == XK_Escape) {
                goto out;
            } else if (ks == XK_BackSpace) {
                x98_edit_backspace(&pw);
            } else if (n > 0 && (unsigned char)buf[0] >= 0x20) {
                buf[n] = 0;
                x98_edit_insert(&pw, buf, n);
            }
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;
            int by = WIN_H - BTN_H - 14;
            if (my >= by && my < by + BTN_H) {
                int ok_x = WIN_W - 2 * BTN_W - 24;
                if (mx >= ok_x && mx < ok_x + BTN_W) {
                    if (try_run(sargv)) goto out;
                    failed = 1;
                    x98_edit_set(&pw, "");
                    redraw();
                } else if (mx >= WIN_W - BTN_W - 14) {
                    goto out;
                }
            }
            break;
        }
        }
    }

out:
    memset(pw.buf, 0, sizeof(pw.buf));
    XCloseDisplay(dpy);
    return 0;
}

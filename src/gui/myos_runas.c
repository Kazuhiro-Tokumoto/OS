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
#include <fcntl.h>

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

/* 失敗の理由。ここを 1 つの旗にしていたのが元の間違いで、
 * 「パスワードが違う」と「コマンドが転んだ」が同じ扱いだった。
 * 実機で myos-setres が転んだとき、正しいパスワードを入れているのに
 * 「Wrong password」と出て、本当のエラーがどこにも出なかった。 */
enum { FAIL_NONE = 0, FAIL_AUTH, FAIL_CMD };
static char cmd_err[160] = "";

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);

    x98_text(&x98, win, 14, 14, "This operation requires administrator",
             x98.text);
    x98_text(&x98, win, 14, 30, "privileges.", x98.text);

    x98_text(&x98, win, 14, 56, msg, x98.text);

    x98_text(&x98, win, 14, 88, "Password:", x98.text);
    draw_password(90, 84, WIN_W - 90 - 14, 20);

    if (failed == FAIL_AUTH)
        x98_text(&x98, win, 14, 112,
                 "Wrong password, or you are not an administrator.",
                 x98.text);
    else if (failed == FAIL_CMD) {
        /* 認証は通っている。転んだのはコマンドのほう。
         * その場で理由が読めないと、使う人は打つ手が無い。 */
        x98_text(&x98, win, 14, 112, "The command could not run:", x98.text);
        x98_text(&x98, win, 14, 128, cmd_err, x98.text);
    }

    x98_button(&x98, win, WIN_W - 2 * BTN_W - 24, WIN_H - BTN_H - 14,
               BTN_W, BTN_H, "OK", 0);
    x98_button(&x98, win, WIN_W - BTN_W - 14, WIN_H - BTN_H - 14,
               BTN_W, BTN_H, "Cancel", 0);
}

/* 認証と実行を 2 段に分ける。
 *
 * 1 段にまとめると区別が付かない。sudo は認証に失敗しても 1 で終わるし、
 * 起動したコマンドが 1 で終わってもやはり 1 が返る。実機でこれを踏んだ:
 * 正しいパスワードを入れているのに "Wrong password" と出て、本当の理由
 * (myos-setres: /dev/root がありません) がどこにも出なかった。
 *
 *   1 段目  sudo -S -k -v <なし>   認証だけ。ここの失敗は「パスワードが違う」
 *   2 段目  sudo -S -k <cmd>       実行。ここの失敗はコマンドの都合
 *
 * 両方にパスワードを渡す。時刻印 (sudo が覚えている「さっき認証した」)
 * には頼らない。-k を付けた sudo は認証しても時刻印を更新しないため。
 *
 *   man sudo: -k ... will not update the user's cached credentials
 *
 * ここを 2 段目 -n (聞かない) にしていたら、1 段目が通ったのに
 * 2 段目が「sudo: パスワードが必要です」で必ず転んだ。実機で踏んだ。
 *
 * -k を外せば時刻印は残るが、そうすると「前に認証してから 15 分以内」の
 * 間はどんな文字を打っても sudo -v が素通りする。パスワードを聞く画面が
 * 何も守らなくなるので、-k は外さない。渡す手間のほうを取る。
 */
static int authenticate(void)
{
    int fd[2];
    if (pipe(fd) != 0) return 0;

    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return 0; }

    if (p == 0) {
        close(fd[1]);
        dup2(fd[0], 0);
        close(fd[0]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, 2); close(null); }
        char *av[] = { "sudo", "-S", "-k", "-v", "-p", "", NULL };
        execvp("sudo", av);
        _exit(127);
    }

    close(fd[0]);
    ssize_t ign;
    ign = write(fd[1], pw.buf, pw.len);
    ign = write(fd[1], "\n", 1);
    (void)ign;
    close(fd[1]);

    /* ここではまだ消せない。2 段目にも同じものを渡す (時刻印に頼らない)。
     * 消すのは try_run の最後。 */

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* 認証済みの前提で実行する。転んだら、その理由を err に汲んでくる。
 * 画面に出すのはここで拾った 1 行。使う人が次に何をすればいいかは、
 * 大抵そこに書いてある。 */
static int run_command(char **argv, char *err, size_t errsz)
{
    err[0] = 0;

    int ep[2];
    if (pipe(ep) != 0) return 0;
    int ip[2];
    if (pipe(ip) != 0) { close(ep[0]); close(ep[1]); return 0; }

    pid_t p = fork();
    if (p < 0) {
        close(ep[0]); close(ep[1]); close(ip[0]); close(ip[1]);
        return 0;
    }

    if (p == 0) {
        close(ep[0]);
        dup2(ep[1], 2);
        close(ep[1]);
        close(ip[1]);
        dup2(ip[0], 0);
        close(ip[0]);
        execvp("sudo", argv);
        _exit(127);
    }

    close(ep[1]);

    /* sudo -S はここから 1 行読む。読み終えたぶんから先は起動された
     * コマンドの標準入力になるので、閉じて EOF を見せる。 */
    close(ip[0]);
    ssize_t ign;
    ign = write(ip[1], pw.buf, pw.len);
    ign = write(ip[1], "\n", 1);
    (void)ign;
    close(ip[1]);
    char buf[1024];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof(buf) - 1 &&
           (r = read(ep[0], buf + n, sizeof(buf) - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = 0;
    close(ep[0]);

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 1;

    /* 最後の中身のある行を採る。sudo の前置きより、コマンド自身が
     * 最後に言い残したことのほうが役に立つ。 */
    char *last = NULL;
    for (char *ln = strtok(buf, "\n"); ln; ln = strtok(NULL, "\n")) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (*ln) last = ln;
    }
    if (last) snprintf(err, errsz, "%.*s", (int)errsz - 1, last);
    else      snprintf(err, errsz, "exit code %d",
                       WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}

/* 認証 -> 実行。成功したら 1。失敗したら failed に理由を立てる。 */
static int try_run(char **argv)
{
    int ok = 0;
    if (!authenticate()) {
        failed = FAIL_AUTH;
    } else if (!run_command(argv, cmd_err, sizeof(cmd_err))) {
        failed = FAIL_CMD;
    } else {
        ok = 1;
    }

    /* 2 段とも渡し終えた。手元からは消す。 */
    memset(pw.buf, 0, sizeof(pw.buf));
    pw.len = pw.cur = 0;
    return ok;
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
    /* 認証は authenticate() が済ませているが、-k のせいで時刻印は
     * 残っていない。ここでもパスワードを渡す (run_command が流す)。 */
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

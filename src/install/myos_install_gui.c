/* ==========================================================================
 * myos_install_gui.c  -  インストーラの後半 (Windows 98 の GUI ウィザード)
 *
 * 実物の Windows 98 のセットアップは、青い背景の上に情報バーが乗り、
 * そこに 5 段階の進み具合と「estimated time remaining」が出ていた。
 * その形をそのまま真似る。
 *
 *   1. Preparing to run myOS Setup
 *   2. Collecting information about your computer
 *   3. Copying myOS files to your computer
 *   4. Restarting your computer
 *   5. Setting up hardware and finalizing settings
 *
 * 5 番目は再起動後の myos-setup が担当する (98 も再起動後に
 * ユーザー名や地域を聞いていた)。ここは 1〜4 をやる。
 *
 * 前半 (myos-install-text) が /tmp/myos-install.conf に
 * 書き込み先を残しているので、それを読んで進める。
 *
 * ビルド:
 *   gcc -O2 -o myos-install-gui myos_install_gui.c -lX11
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/statvfs.h>

#include "x98.h"

/* 情報バーは左。98 と同じ配置。 */
#define BAR_W    300
#define BAR_PAD   24
#define STEP_H    46

#define CONF_PATH "/tmp/myos-install.conf"
/* メディア上の名前。ISO の中では myos.squashfs という名前で置いてある。
 * (以前は payload.squashfs という別名を張ろうとしていたが、
 *  メディアは読み取り専用なのでリンクを作れない) */
#define SQUASH    "/run/myos/myos.squashfs"
#define TARGET    "/mnt/target"
#define FWINIT    "/tmp/myos-fw.cpio.gz"

enum { ST_PREPARE = 0, ST_COLLECT, ST_COPY, ST_RESTART, ST_N };

static const char *step_name[ST_N] = {
    "Preparing to run myOS Setup",
    "Collecting information about your computer",
    "Copying myOS files to your computer",
    "Restarting your computer",
};

/* コピー中に右側へ出す文句。98 も似たようなことをしていた。 */
static const char *blurb[][2] = {
    { "A desktop you already know",
      "Windows 98 looks, with today's shortcuts." },
    { "Everything is already installed",
      "Browser, Java, and an antivirus, ready to go." },
    { "Your files stay yours",
      "Downloads are scanned the moment they arrive." },
    { "Built to run anywhere",
      "Every driver is compiled into the kernel." },
};
#define N_BLURB ((int)(sizeof(blurb) / sizeof(blurb[0])))

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static int      scr_w, scr_h;

static int    cur_step = ST_PREPARE;
static int    percent  = 0;
static char   status[160] = "";
static int    blurb_i = 0;
static time_t started;
static int    eta_sec = 0;
static int    failed = 0;
/* unsquashfs が最後に吐いた文句。コピーに失敗したときに出す。 */
static char   copy_err[128] = "";

static char   root_dev[64] = "";
static char   disk_dev[64] = "";
static char   boot_lba[32] = "2048";

/* --- 描画 ---------------------------------------------------------------- */
static void draw_steps(void)
{
    int x = BAR_PAD, y = 90;

    x98_text(&x98, win, x, 44, "myOS Setup", x98.white);

    for (int i = 0; i < ST_N; i++) {
        int sy = y + i * STEP_H;
        unsigned long fg = x98.white;

        /* 済んだものにはチェック、今のものには矢印を付ける。
         * 98 の情報バーと同じで、どこまで進んだかが一目で分かる。 */
        if (i < cur_step) {
            XSetForeground(dpy, x98.gc, x98.white);
            XDrawLine(dpy, win, x98.gc, x, sy + 6, x + 4, sy + 11);
            XDrawLine(dpy, win, x98.gc, x + 4, sy + 11, x + 12, sy - 1);
        } else if (i == cur_step) {
            XPoint p[3] = { {(short)x, (short)(sy - 1)},
                            {(short)x, (short)(sy + 11)},
                            {(short)(x + 10), (short)(sy + 5)} };
            XSetForeground(dpy, x98.gc, x98.white);
            XFillPolygon(dpy, win, x98.gc, p, 3, Convex, CoordModeOrigin);
        } else {
            /* まだのものは薄く */
            fg = x98_rgb24(&x98, 0x8090C0);
        }

        /* 文が長いので 2 行に折る */
        const char *s = step_name[i];
        char l1[64], l2[64];
        l1[0] = l2[0] = 0;
        if (x98_text_w(&x98, s) > BAR_W - 40) {
            const char *sp = strchr(s + 20, ' ');
            if (sp) {
                size_t n = (size_t)(sp - s);
                if (n > sizeof(l1) - 1) n = sizeof(l1) - 1;
                memcpy(l1, s, n);
                l1[n] = 0;
                snprintf(l2, sizeof(l2), "%s", sp + 1);
            }
        }
        if (l1[0]) {
            x98_text(&x98, win, x + 20, sy, l1, fg);
            x98_text(&x98, win, x + 20, sy + 15, l2, fg);
        } else {
            x98_text(&x98, win, x + 20, sy, s, fg);
        }
    }

    /* 残り時間。98 のあれ。 */
    char t[64];
    if (eta_sec > 0) {
        int m = (eta_sec + 59) / 60;
        snprintf(t, sizeof(t), "Estimated time remaining: %d minute%s",
                 m, m == 1 ? "" : "s");
    } else {
        snprintf(t, sizeof(t), "Estimated time remaining: calculating...");
    }
    x98_text(&x98, win, x, y + ST_N * STEP_H + 24, t, x98.white);
}

static void draw_right(void)
{
    int x = BAR_W + 60;
    int w = scr_w - x - 60;
    if (w < 200) return;

    /* 宣伝の枠。コピー中だけ出す。 */
    if (cur_step == ST_COPY) {
        int by = scr_h / 2 - 120;
        x98_fill(&x98, win, x, by, w, 150, x98.face);
        x98_bevel(&x98, win, x, by, w, 150, 1);
        x98_text(&x98, win, x + 20, by + 30, blurb[blurb_i][0], x98.text);
        x98_text(&x98, win, x + 20, by + 56, blurb[blurb_i][1], x98.shadow);
    }

    /* 進捗バー */
    int py = scr_h / 2 + 60;
    x98_text(&x98, win, x, py - 26, status, x98.white);

    x98_bevel(&x98, win, x, py, w, 24, 0);
    x98_fill(&x98, win, x + 2, py + 2, w - 4, 20, x98.face);
    int fillw = (w - 6) * percent / 100;
    /* 98 の進捗バーは細かいブロックの並びだった */
    for (int i = 0; i * 10 < fillw; i++)
        x98_fill(&x98, win, x + 3 + i * 10, py + 3, 8, 18,
                 x98_rgb24(&x98, 0x000080));

    char pc[32];
    snprintf(pc, sizeof(pc), "%d%%", percent);
    x98_text(&x98, win, x + w / 2 - 12, py + 32, pc, x98.white);

    if (failed) {
        x98_text(&x98, win, x, py + 70,
                 "Setup could not complete. See the console for details.",
                 x98_rgb24(&x98, 0xFFFF80));
    }
}

static void redraw(void)
{
    /* 98 のセットアップのあの青。 */
    x98_fill(&x98, win, 0, 0, scr_w, scr_h, x98_rgb24(&x98, 0x000080));
    /* 左の情報バーは少し濃い帯で区切る */
    x98_fill(&x98, win, 0, 0, BAR_W, scr_h, x98_rgb24(&x98, 0x000060));
    x98_vline(&x98, win, BAR_W, 0, scr_h, x98_rgb24(&x98, 0x1084D0));

    draw_steps();
    draw_right();
    XFlush(dpy);
}

/* 進捗を出しつつ、X のイベントも捌く。
 * 長い処理の途中でも画面が固まらないようにするため。 */
static void tick(int pct, const char *msg)
{
    if (pct >= 0) percent = pct;
    /* ここで必ず 0〜100 に収める。
     * 計算側でも丸めてはいるが、表示の直前に一度も見ていないと、
     * どこかで桁が飛んだときにそのまま画面へ出てしまう。
     * 実際「一瞬だけ 1000000%」が出た。
     * 進捗の数字は嘘をつかないことより、ありえない値を出さないことが先。 */
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    if (msg) snprintf(status, sizeof(status), "%s", msg);

    /* 経過から残りを見積もる。98 も実際そんなものだった。 */
    if (percent > 3) {
        time_t now = time(NULL);
        double elapsed = difftime(now, started);
        eta_sec = (int)(elapsed * (100.0 - percent) / percent);
    }
    blurb_i = (int)(difftime(time(NULL), started) / 12) % N_BLURB;

    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }
    redraw();
}

/* --- 設定の読み込み ------------------------------------------------------ */
static void load_conf(void)
{
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = myos_trim(line), *v = myos_trim(eq + 1);
        if (!strcmp(k, "root")) snprintf(root_dev, sizeof(root_dev), "%s", v);
        else if (!strcmp(k, "disk")) snprintf(disk_dev, sizeof(disk_dev), "%s", v);
        else if (!strcmp(k, "bootlba")) snprintf(boot_lba, sizeof(boot_lba), "%s", v);
    }
    fclose(f);
}

/* --- 外部コマンド -------------------------------------------------------- */
static int run(char *const argv[])
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* squashfs に入っている中身の合計バイト数。
 * unsquashfs 4.5 には -percentage が無く (4.6 で入った)、
 * 進捗を数字で言わせることができない。
 * なので「入れ先がどれだけ埋まったか」で進捗を出す。その分母。
 * -lls はメタデータだけを読むので 1 秒もかからない。 */
static long long payload_bytes(void)
{
    FILE *fp = popen("unsquashfs -lls " SQUASH " 2>/dev/null | "
                     "awk '$1 ~ /^-/ { s += $3 } END { print s+0 }'", "r");
    if (!fp) return 0;
    char buf[64] = "";
    if (!fgets(buf, sizeof(buf), fp)) buf[0] = 0;
    pclose(fp);
    return strtoll(buf, NULL, 10);
}

/* パーティションの使用量。統計は 1 ブロック単位に丸まるが、
 * 進捗の目安としてはそれで足りる。 */
static long long fs_used(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) return -1;
    /* f_blocks と f_bfree は符号無し。書き込みの最中に読むと
     * 前後がずれて f_bfree のほうが大きく見えることがある。
     * そのまま引くと 1.8e19 に回り込み、その後の掛け算で
     * 符号付きの範囲も超えて、進捗が無茶な値になる。 */
    if (v.f_bfree > v.f_blocks) return -1;
    unsigned long long used = (unsigned long long)(v.f_blocks - v.f_bfree);
    unsigned long long fr   = (unsigned long long)v.f_frsize;
    if (fr == 0) return -1;
    /* 掛ける前に溢れないか見る */
    if (used > (unsigned long long)LLONG_MAX / fr) return -1;
    return (long long)(used * fr);
}

/* 展開する。走らせている間も画面を動かし続ける。 */
static int copy_payload(void)
{
    long long total = payload_bytes();
    long long base  = fs_used(TARGET);
    if (base < 0) base = 0;

    int fd[2];
    if (pipe(fd) != 0) return 0;

    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return 0; }
    if (p == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[1]);
        /* -n で unsquashfs 自身の進捗バーを止める。
         * こちらで進捗を出すので要らないし、\r まみれの行が
         * エラー欄に流れ込むのも困る。 */
        execlp("unsquashfs", "unsquashfs", "-n", "-f", "-d", TARGET,
               SQUASH, (char *)NULL);
        /* execlp が戻ってきたということは unsquashfs が無い。
         * 呼び出し元は終了コードしか見ないので、ここで理由を残しておく。 */
        fprintf(stderr, "unsquashfs: %s\n", strerror(errno));
        _exit(127);
    }
    close(fd[1]);

    /* 子の言い分を読みつつ、0.5 秒ごとに進捗を測り直す。
     * poll を挟まないと、無口なまま数分走るあいだ画面が固まる。 */
    struct pollfd pf = { .fd = fd[0], .events = POLLIN };
    char buf[512];
    int done = 0;
    for (;;) {
        int r = poll(&pf, 1, 500);
        if (r > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(fd[0], buf, sizeof(buf) - 1);
            if (n <= 0) { done = 1; }
            else {
                buf[n] = 0;
                /* 最後の 1 行だけ覚えておいて、失敗したときに出す。 */
                char *nl = strrchr(buf, '\n');
                if (nl) *nl = 0;
                char *last = strrchr(buf, '\n');
                last = last ? last + 1 : buf;
                if (*last) snprintf(copy_err, sizeof(copy_err), "%s", last);
            }
        }

        long long now = (total > 0) ? fs_used(TARGET) : -1;
        if (now > base) {
            /* 割ってから掛ける。先に 100 を掛けると、
             * 読み取りが一度でも大きく振れたときに桁が溢れる。 */
            double frac = (double)(now - base) / (double)total;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            /* コピーは全体の 10% 〜 85% を占める扱いにする */
            tick(10 + (int)(frac * 75.0),
                 "Copying myOS files to your computer...");
        } else {
            tick(-1, "Copying myOS files to your computer...");
        }

        if (done) break;
    }
    close(fd[0]);

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        copy_err[0] = 0;        /* 成功したので、途中の出力は捨てる */
        tick(85, "Copying myOS files to your computer...");
        return 1;
    }
    return 0;
}

/* --- 手順 ---------------------------------------------------------------- */
static int do_install(void)
{
    char *mk[]  = { "mkdir", "-p", TARGET, NULL };
    char *mnt[] = { "mount", root_dev, TARGET, NULL };
    char *umt[] = { "umount", TARGET, NULL };

    /* 1. 準備 */
    cur_step = ST_PREPARE;
    tick(2, "Preparing to run Setup...");
    run(mk);
    if (run(mnt) != 0) {
        failed = 1;
        tick(-1, "Could not mount the target partition.");
        return 0;
    }

    /* 2. 情報を集める */
    cur_step = ST_COLLECT;
    tick(6, "Collecting information about your computer...");
    sleep(1);

    /* 3. コピー */
    cur_step = ST_COPY;
    tick(10, "Copying myOS files to your computer...");
    if (!copy_payload()) {
        failed = 1;
        if (copy_err[0]) {
            char m[192];
            snprintf(m, sizeof(m), "Could not copy the files: %s", copy_err);
            tick(-1, m);
        } else {
            tick(-1, "Could not copy the files.");
        }
        run(umt);
        return 0;
    }

    /* 起動に要るものを書く。
     * fstab はインストール先のパーティション名に合わせて作り直す。
     * イメージに入っていたものは作成時のもので、ここでは合わない。 */
    tick(88, "Writing the startup files...");
    char path[256];
    snprintf(path, sizeof(path), "%s/etc/fstab", TARGET);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%-12s /      ext4  defaults  0 1\n", root_dev);
        fprintf(f, "proc         /proc  proc  defaults  0 0\n");
        fprintf(f, "sysfs        /sys   sysfs defaults  0 0\n");
        fclose(f);
    }

    /* 初回セットアップをもう一度やらせる。
     * 配布イメージを作ったときの設定が残っていると、
     * 入れた人ではなく作った人のユーザーで起動してしまう。 */
    snprintf(path, sizeof(path), "%s/etc/myos/setup-done", TARGET);
    unlink(path);
    snprintf(path, sizeof(path), "%s/etc/myos/login.conf", TARGET);
    unlink(path);

    /* GPU のファームウェアを initramfs に詰める。
     *
     * .ko を全て =y にしてあるので、PCI の probe はルートのマウントより
     * 1.1 秒早く走る。そのとき /lib/firmware はまだ存在しないので、
     * ファームを要るドライバは probe の時点で必ず失敗する。radeon と
     * amdgpu は VBE の画面を先に取り上げてから転ぶので、画面が真っ暗に
     * なる。initramfs なら rootfs_initcall で展開されるので間に合う。
     *
     * 作れなくても入れたものは起動する (ファームが要らない GPU なら
     * 何も困らない) ので、ここで失敗しても止めない。 */
    tick(90, "Preparing graphics drivers...");
    char *fwi[] = { "myos-mkfwinit", TARGET, FWINIT, NULL };
    int have_fw = (run(fwi) == 0) && access(FWINIT, R_OK) == 0;

    /* 4. ブートローダーを書いて再起動 */
    cur_step = ST_RESTART;
    tick(92, "Installing the boot loader...");

    char *bl[] = { "myos-writeboot", disk_dev, root_dev, boot_lba,
                   have_fw ? (char *)FWINIT : NULL, NULL };
    if (run(bl) != 0) {
        failed = 1;
        tick(-1, "Could not install the boot loader.");
        run(umt);
        return 0;
    }

    tick(98, "Finishing...");
    run(umt);
    tick(100, "Setup is complete. The computer will restart.");
    return 1;
}

int main(void)
{
    load_conf();
    if (!root_dev[0]) {
        fprintf(stderr, "myos-install-gui: %s missing or incomplete\n",
                CONF_PATH);
        return 2;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-install-gui: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, screen);
    scr_h = DisplayHeight(dpy, screen);
    x98_init(&x98, dpy, screen);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = x98_rgb24(&x98, 0x000080);
    swa.event_mask = ExposureMask;
    win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, scr_w, scr_h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
    XStoreName(dpy, win, "myOS Setup");
    XMapRaised(dpy, win);

    started = time(NULL);
    tick(0, "Starting Setup...");

    int ok = do_install();

    if (ok) {
        /* 起動した媒体を調べておく。再起動の前に開けるため。
         * 入れたままだと次の起動でまたインストーラが出てくる。 */
        char medium[64] = "";
        FILE *mf = fopen("/proc/mounts", "r");
        if (mf) {
            char dev[64], mp[128];
            while (fscanf(mf, "%63s %127s %*[^\n]", dev, mp) == 2)
                if (!strcmp(mp, "/myos-medium")) {
                    snprintf(medium, sizeof(medium), "%s", dev);
                    break;
                }
            fclose(mf);
        }

        /* 少し見せてから再起動。いきなり落ちると
         * 終わったのか失敗したのか分からない。
         * 媒体が開かなかったときのために、抜く案内も出しておく。
         *
         * 言い方は媒体で変える。USB から入れた人に「ディスクを出せ」と
         * 言っても通じない。名前で見分けるのは文言だけの話なので
         * これで足りる (実際に開けるかどうかは myos-poweroff 側で
         * CDROM_GET_CAPABILITY を見て決める)。 */
        const char *what = "installation media";
        if (!strncmp(medium, "/dev/sr", 7))      what = "disc";
        else if (!strncmp(medium, "/dev/sd", 7)) what = "USB drive";

        for (int i = 10; i > 0; i--) {
            char m[140];
            snprintf(m, sizeof(m),
                     "Setup is complete. Remove the %s. "
                     "Restarting in %d second%s...",
                     what, i, i == 1 ? "" : "s");
            tick(100, m);
            sleep(1);
        }

        /* reboot コマンドは無い (systemd を使っていないので入っていない)。
         * ここを "reboot -f" にしていたため、10 秒数えたあと何も起きず、
         * X だけが終わって画面にログが散らかった状態で止まっていた。
         * 電源まわりは自前の myos-poweroff に一本化してある。 */
        char *rb[] = { "/usr/local/bin/myos-poweroff", "-r",
                       medium[0] ? "-e" : NULL, medium[0] ? medium : NULL,
                       NULL };
        run(rb);
    } else {
        /* 失敗したら消さずに残す。何が起きたか見えないと直せない。 */
        for (;;) {
            tick(-1, NULL);
            sleep(2);
        }
    }

    XCloseDisplay(dpy);
    return ok ? 0 : 1;
}

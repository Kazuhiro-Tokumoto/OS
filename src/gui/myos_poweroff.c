/* ==========================================================================
 * myos_poweroff.c  -  電源を切る / 再起動する
 *
 *   myos-poweroff              電源を切る
 *   myos-poweroff -r           再起動する
 *   myos-poweroff -r -e <機器> 再起動する前に光学ドライブを開ける
 *
 * systemd を使っていないので poweroff も shutdown も入っていない。
 * (メニューには「Shut Down」があるのに /sbin/poweroff が無く、
 *  押しても何も起きない状態になっていた)
 *
 * PID 1 が自作のシェルスクリプトなので、後始末もここでやる。
 * 要 root。sudoers で NOPASSWD にしてある。
 * 電源を切るのに認証を求めても意味が無い。電源ボタンを押せる人は
 * どのみち切れる。
 *
 * ビルド:
 *   gcc -O2 -o myos-poweroff myos_poweroff.c
 * ========================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>

/* myos-init と取り決めた印。片方だけ直すと噛み合わなくなる。 */
#define SHUTDOWN_FLAG "/run/myos-shutdown"

/* 自分の親をたどって並べる。
 *
 * 数えるときにこれを除く。理由は sudo。このプログラムは sudo 経由で
 * 起動されるので、親の sudo は「子 (自分) が終わるまで」終われない。
 * 自分は SIGTERM を無視して電源を切る役なので、**永久に終わらない**。
 * 除かないと「1 個残っている」が消えず、毎回上限まで待つことになる。
 *
 * 毎回たどり直すのが肝心。上の代 (WM や X) が先に終わると、その時点で
 * 親子関係が PID 1 に付け替わり、この一覧から自然に消える。
 * 最初に一度だけ数えて固定すると、終わったはずの X を待たなくなる。 */
static int is_ancestor(pid_t target)
{
    pid_t p = getppid();
    for (int hop = 0; p > 1 && hop < 32; hop++) {
        if (p == target) return 1;
        char path[64], line[256];
        snprintf(path, sizeof(path), "/proc/%d/status", (int)p);
        FILE *f = fopen(path, "r");
        if (!f) return 0;
        pid_t up = 0;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "PPid:", 5)) { up = (pid_t)atoi(line + 5); break; }
        fclose(f);
        if (up <= 0) return 0;
        p = up;
    }
    return 0;
}

/* まだ終わっていないユーザーの処理を数える。
 *
 * カーネルスレッドは数えない。あれは SIGTERM で終わらないし、
 * 終わってもらっても困る。cmdline が空かどうかで見分ける
 * (カーネルスレッドは argv を持たない)。
 * 自分自身と PID 1、それに自分の親たちも除く (上のコメント参照)。 */
static int procs_alive(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    pid_t me = getpid();
    pid_t parent = getppid();
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 1 || pid == me || pid == parent) continue;
        if (is_ancestor(pid)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;                 /* もう消えた */
        char buf[1];
        ssize_t got = read(fd, buf, 1);
        close(fd);
        if (got > 0) n++;                     /* cmdline がある = ユーザーの処理 */
    }
    closedir(d);
    return n;
}

/* 全部終わるか、上限に達するまで待つ。戻り値は残った数。 */
static int wait_for_exit(int max_ms)
{
    int waited = 0;
    int left;
    while ((left = procs_alive()) > 0 && waited < max_ms) {
        usleep(100 * 1000);
        waited += 100;
    }
    return left;
}

int main(int argc, char **argv)
{
    int cmd = RB_POWER_OFF;
    const char *eject_dev = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) cmd = RB_AUTOBOOT;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) eject_dev = argv[++i];
    }

    /* 1. まず自分に飛んでくる合図を無視する。
     *
     *    kill(-1) は PID 1 と自分自身を除くので、自分は撃たれない
     *    ——と思っていたが、これでは足りなかった。
     *    このプログラムは sudo 経由で起動される。kill(-1) の
     *    SIGTERM は sudo にも飛び、sudo は受け取った合図を子
     *    (つまりこのプログラム) に転送する。結果、自分で自分を
     *    撃って、電源を切る手前で死んでいた。
     *    X だけが落ちて機械は生きたまま、という中途半端な状態になる。
     *    実際に起動して確かめるまで気付かなかった。
     *
     *    親の集団からも抜けておく。集団宛ての合図を拾わないように。 */
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    setsid();

    /* 1.5 「これは正常な終了だ」という印を置く。
     *
     *     この後 X を落とすと、PID 1 (myos-init) の xinit が返る。
     *     myos-init はそれを「X が落ちた = 異常」とみなして
     *     Xorg のログ全文をコンソールに吐く作りになっている。
     *     電源を切るたびに画面がログで埋まっていたのはこれが原因。
     *     印があれば黙って終わってもらう。
     *
     *     印は kill より先に置くこと。後だと間に合わない。 */
    { int fd = open(SHUTDOWN_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) { ssize_t n = write(fd, cmd == RB_AUTOBOOT ? "r\n" : "p\n", 2);
                     (void)n; close(fd); } }

    /* 2. 動いているものに終わる機会を与える。
     *    ここで X も WM も落ちる。先に落としておかないと、
     *    書きかけのファイルを抱えたまま電源が切れる。
     *
     *    **2 秒固定で待っていたのが間違いだった。**
     *    Firefox は SIGTERM を受けてから、セッション・履歴・cookie を
     *    書き出して profile の錠を外すまでに、ハードディスクだと
     *    10 秒近くかかる。2 秒で SIGKILL していたので、
     *    **毎回 "正常に終了しませんでした" になっていた**。
     *    Minecraft のランチャーがログイン情報を忘れるのも同じ理由。
     *    「電源をブチ切りしたから」ではなく、こちらが待たなかったから。
     *
     *    数えて待つ。全部終われば即座に進むので、何も動いていない
     *    ときは今までより速い。終わらない相手が居ても上限で切り上げる。 */
    kill(-1, SIGTERM);

    /* まず X が退くのを待つ。画面を持っている間は下の splash が描けない。
     * 長くは待たない。X は SIGTERM ですぐ落ちる。 */
    wait_for_exit(1500);

    /* 2.5 終了の画面を出す。
     *     X が居なくなった今なら /dev/fb0 に直接描ける。逆に、これより
     *     前に描いても X が画面を持っているので見えない。
     *     SIGKILL より前に出す。後にすると黒い時間が 1 秒余分に伸びる。
     *     一枚描いて終わるだけなので、この後の sync を待たせない。 */
    { pid_t p = fork();
      if (p == 0) {
          execl("/usr/local/bin/myos-splash", "myos-splash", "--once",
                cmd == RB_AUTOBOOT ? "Restarting..." : "Shutting down...",
                (char *)NULL);
          _exit(127);
      }
      if (p > 0) { int st; while (waitpid(p, &st, 0) < 0 && errno == EINTR) { } }
    }

    /* 画面を出したので、ここからは落ち着いて待てる。
     * 残っているものが書き終えるまで、最大 15 秒。
     * 待っている間ずっと "Shutting down..." が出ているので、
     * 固まったようには見えない。 */
    int left = wait_for_exit(15000);
    if (left > 0)
        fprintf(stderr, "myos-poweroff: %d 個が終わらないので強制します\n",
                left);

    /* 上限まで粘っても終わらないものは、ここで確実に止める。 */
    kill(-1, SIGKILL);

    /* 描いたそばから電源が落ちると、出したことにならない。
     * 実測すると、描いてから reboot まで 0.5 秒しかなく、画面には
     * ほとんど映らなかった。少し置く。この間に sync も進む。
     * (上の待ちで既に時間が経っている場合は、そのぶん短くてよいが、
     *  SIGKILL した直後の後始末に少しは要る) */
    sleep(1);

    /* 2.7 光学ドライブを開ける。
     *
     *     インストールが終わったあとに使う。入れたままだと、次の起動で
     *     また CD から立ち上がってインストーラが出てくる。
     *
     *     ただし確実には開かない。live の根っこは CD 上の squashfs なので、
     *     動いている今この瞬間もその機器は使われており、カーネルは
     *     EBUSY を返すことがある。開けば儲けもの、開かなければ画面の
     *     案内で抜いてもらう、という扱いにする。だから失敗しても
     *     止めないし、騒がない。 */
    if (eject_dev) {
        /* 先に外しておくと開く見込みが上がる。遅延で外すので、
         * まだ使っている者が居ても呼び出しは戻ってくる。 */
        umount2("/myos-medium", MNT_DETACH);
        int fd = open(eject_dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            /* 光学ドライブかどうかを先に確かめる。
             *
             * USB から起動した場合、ここに来るのは USB メモリの機器名に
             * なる。トレイを開ける命令を投げる相手ではないし、機種に
             * よっては START STOP UNIT と解釈して机上から消える。
             * CDROM_GET_CAPABILITY は光学ドライブでなければ ENOTTY を
             * 返すので、それを門番にする (eject コマンドと同じやり方)。 */
            if (ioctl(fd, CDROM_GET_CAPABILITY, NULL) >= 0)
                ioctl(fd, CDROMEJECT);
            close(fd);
        }
    }

    /* 2.8 合わせた時刻を本体の時計へ書き戻す。
     *
     *     chronyd が網から合わせても、それは動いている間だけの話で、
     *     電源を切れば消える。本体の時計は放っておくと月に何分もずれ、
     *     ずれた時計は「証明書がまだ有効でない」で HTTPS が全部落ちる
     *     という、原因の分かりにくい壊れ方をする。
     *
     *     --systohc は /etc/adjtime を見るので、myOS の決め (ローカル時刻)
     *     どおりに書く。chronyd 側で rtcsync を使わないのはこのため。
     *     あちらは必ず UTC で書くので、両方やると食い違う。
     *
     *     読み取り専用にし直す前にやること。あとだと書けない。 */
    {
        pid_t p = fork();
        if (p == 0) {
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) { dup2(null, 1); dup2(null, 2); close(null); }
            execl("/usr/sbin/hwclock", "hwclock", "--systohc", (char *)NULL);
            execl("/sbin/hwclock", "hwclock", "--systohc", (char *)NULL);
            _exit(127);
        }
        if (p > 0) {
            int st;
            while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
        }
    }

    /* 3. 書き込みを閉じる。
     *    sync だけでは足りない。読み取り専用にし直すところまでやると
     *    ext4 のジャーナルも畳まれ、次の起動で fsck に回らない。
     *    失敗しても続ける。切れないより、多少荒くても切れるほうがいい。 */
    sync();
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) != 0)
        fprintf(stderr, "myos-poweroff: remount ro: %s\n", strerror(errno));
    sync();

    reboot(cmd);

    /* ここに来たら失敗している */
    fprintf(stderr, "myos-poweroff: %s\n", strerror(errno));
    return 1;
}

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

/* myos-init と取り決めた印。片方だけ直すと噛み合わなくなる。 */
#define SHUTDOWN_FLAG "/run/myos-shutdown"

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
     *    書きかけのファイルを抱えたまま電源が切れる。 */
    kill(-1, SIGTERM);
    sleep(2);

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

    /* 残っているものを確実に止める。終了の画面を描いたあとにやるのは、
     * 画面が出るまでの黒い時間を短くするため。SIGTERM から 2 秒あれば
     * X は落ちているので、この時点でもう描ける。 */
    kill(-1, SIGKILL);

    /* 描いたそばから電源が落ちると、出したことにならない。
     * 実測すると、描いてから reboot まで 0.5 秒しかなく、画面には
     * ほとんど映らなかった。少し置く。この間に sync も進む。 */
    sleep(2);

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
            ioctl(fd, CDROMEJECT);
            close(fd);
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

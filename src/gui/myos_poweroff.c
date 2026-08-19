/* ==========================================================================
 * myos_poweroff.c  -  電源を切る / 再起動する
 *
 *   myos-poweroff        電源を切る
 *   myos-poweroff -r     再起動する
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
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int cmd = RB_POWER_OFF;
    if (argc > 1 && !strcmp(argv[1], "-r")) cmd = RB_AUTOBOOT;

    /* 1. 動いているものに終わる機会を与える。
     *    kill(-1) は PID 1 と自分自身には飛ばない (Linux の仕様)。
     *    ここで X も WM も落ちる。先に落としておかないと、
     *    書きかけのファイルを抱えたまま電源が切れる。 */
    kill(-1, SIGTERM);
    sleep(2);
    kill(-1, SIGKILL);
    sleep(1);

    /* 2. 書き込みを閉じる。
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

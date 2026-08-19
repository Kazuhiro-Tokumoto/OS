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

    /* 2. 動いているものに終わる機会を与える。
     *    ここで X も WM も落ちる。先に落としておかないと、
     *    書きかけのファイルを抱えたまま電源が切れる。 */
    kill(-1, SIGTERM);
    sleep(2);
    kill(-1, SIGKILL);
    sleep(1);

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

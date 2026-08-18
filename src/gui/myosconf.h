/* ==========================================================================
 * myosconf.h  -  myOS の設定ファイルの置き場所
 *
 * 2 段構えにしてある。
 *
 *   /etc/myos/<name>       システム既定。root しか書けない
 *   ~/.myos/<name>         ユーザーごとの上書き。本人が書ける
 *
 * 読むときはユーザーのものを優先し、無ければシステム既定に落ちる。
 * 書くときは必ずユーザー側へ書く。
 *
 * こうしておくと「壁紙や配色を変えるのに管理者権限が要らない」。
 * 逆にシステム全体を変えるものは /etc/myos に書くので昇格が要る。
 * ========================================================================== */
#ifndef MYOSCONF_H
#define MYOSCONF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>

#define MYOS_ETC "/etc/myos"

static inline const char *myos_home(void)
{
    const char *h = getenv("HOME");
    if (h && h[0]) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
    return "/root";
}

/* 書き込み先。~/.myos を作ってからそのパスを返す。 */
static inline void myos_user_conf(char *out, size_t n, const char *name)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.myos", myos_home());
    mkdir(dir, 0755);
    snprintf(out, n, "%s/%s", dir, name);
}

/* 読み込み先。ユーザーのものがあればそれ、無ければシステム既定。 */
static inline void myos_conf(char *out, size_t n, const char *name)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/.myos/%s", myos_home(), name);
    if (access(p, R_OK) == 0) { snprintf(out, n, "%s", p); return; }
    snprintf(out, n, "%s/%s", MYOS_ETC, name);
}

/* 管理者かどうか。sudo を通せる (= sudo グループに居る) 人を管理者とみなす。
 * 実際に効くかは myos-runas 側で sudo に判定させるので、
 * ここは「昇格のダイアログを出す価値があるか」の目安。 */
static inline int myos_is_root(void) { return geteuid() == 0; }

/* 行の前後の空白を落とす。設定ファイルを読むところで共通に使う。 */
static inline char *myos_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    return s;
}

#endif /* MYOSCONF_H */

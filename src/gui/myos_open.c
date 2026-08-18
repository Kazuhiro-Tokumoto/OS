/* ==========================================================================
 * myos_open.c  -  myOS の「開く」の一本化 (Windows のシェル関連付け相当)
 *
 *   myos-open <パス>          … 既定の動作で開く (ダブルクリック相当)
 *   myos-open -run <パス>     … 2 つ目の動詞 (「実行」) で開く
 *   myos-open -query <パス>   … 何で開くかを表示するだけ (デバッグ用)
 *
 * 関連付けの表は /etc/myos/filetypes.conf と ~/.myos/filetypes.conf。
 * ファイルマネージャもデスクトップもここを通すので、
 * 「どのファイルを何で開くか」が 1 箇所で決まる。
 *
 * 判定の順番:
 *   1. ディレクトリ          -> ファイルマネージャ
 *   2. 拡張子が表にある      -> その コマンド
 *   3. 実行属性が付いている  -> そのまま実行
 *   4. それ以外              -> Firefox に渡す (Windows の「不明なファイル」)
 *
 * X には一切触らないので、端末からでもスクリプトからでも使える。
 * ========================================================================== */
#include "filetypes.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

static void run_argv(char **argv)
{
    execvp(argv[0], argv);
    fprintf(stderr, "myos-open: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

static void run2(const char *file, const char *a1)
{
    char *argv[3];
    argv[0] = (char *)file;
    argv[1] = (char *)a1;
    argv[2] = NULL;
    run_argv(argv);
}

int main(int argc, char **argv)
{
    int want_run = 0, query = 0;
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-run"))        want_run = 1;
        else if (!strcmp(argv[i], "-query")) query = 1;
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: myos-open [-run] [-query] <path>\n");
        return 2;
    }

    const char *path = argv[i];

    /* 相対パスのまま渡されると、起動されたアプリ側の cwd 次第で
     * 迷子になる。ここで絶対パスに直しておく。 */
    char abs[PATH_MAX];
    if (path[0] != '/' && realpath(path, abs)) path = abs;

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "myos-open: %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (query) { printf("directory -> myos-files\n"); return 0; }
        run2("myos-files", path);
    }

    FileType types[FT_MAX];
    int n = ft_load(types, FT_MAX);
    const FileType *t = ft_find(types, n, path);

    if (t) {
        const char *cmd = (want_run && t->run[0]) ? t->run : t->open;
        char store[FT_CMD + PATH_MAX + 64];
        char *av[FT_ARGV];
        int na = ft_argv(cmd, path, av, FT_ARGV, store, sizeof(store));
        if (na > 0) {
            if (query) {
                printf("%s -> %s\n", t->desc, cmd);
                return 0;
            }
            run_argv(av);
        }
        fprintf(stderr, "myos-open: bad command line: %s\n", cmd);
        return 2;
    }

    if (st.st_mode & S_IXUSR) {
        if (query) { printf("executable -> run directly\n"); return 0; }
        char *av[2];
        av[0] = (char *)path;
        av[1] = NULL;
        run_argv(av);
    }

    if (query) { printf("unknown -> firefox-esr\n"); return 0; }
    run2("firefox-esr", path);
    return 127;
}

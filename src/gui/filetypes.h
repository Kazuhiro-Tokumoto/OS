/* ==========================================================================
 * filetypes.h  -  ファイルの関連付け (Win98 の「ファイルの種類」相当)
 *
 * どの拡張子をどのアプリで開くかを、コードに埋め込まずに設定ファイルへ出す。
 *
 *   /etc/myos/filetypes.conf   システム既定
 *   ~/.myos/filetypes.conf     ユーザーの上書き (こちらが優先)
 *
 * 1 行 1 種類。
 *
 *   拡張子(カンマ区切り)|説明|アイコン|開く|実行(任意)
 *   py|Python Script|app|myos-notepad %1|myos-term python3 %1
 *
 * %1 がファイルのパスに置き換わる。シェルは通さないので、
 * 空白や引用符の入ったファイル名でも壊れない。
 *
 * 「開く」がダブルクリックの動作、「実行」は右クリックに出る 2 つ目の動詞。
 * Windows でいうと open 動詞と、その他の動詞にあたる。
 * ========================================================================== */
#ifndef FILETYPES_H
#define FILETYPES_H

#include "myosconf.h"
#include <strings.h>

#define FT_MAX    64
#define FT_EXTS  192
#define FT_DESC   64
#define FT_ICON   16
#define FT_CMD   256
#define FT_ARGV   32

typedef struct {
    char exts[FT_EXTS];   /* "html,htm" — 小文字で持つ */
    char desc[FT_DESC];
    char icon[FT_ICON];
    char open[FT_CMD];
    char run[FT_CMD];     /* 空なら 2 つ目の動詞は無し */
} FileType;

/* --- 読み込み ------------------------------------------------------------ */

static inline int ft_load_file(const char *path, FileType *t, int n, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return n;

    char line[1024];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *s = myos_trim(line);
        if (!*s || *s == '#') continue;

        char *fld[5] = { NULL, NULL, NULL, NULL, NULL };
        int nf = 0;
        fld[nf++] = s;
        for (char *p = s; *p && nf < 5; p++)
            if (*p == '|') { *p = 0; fld[nf++] = p + 1; }
        if (nf < 4) continue;

        FileType *e = &t[n];
        memset(e, 0, sizeof(*e));
        snprintf(e->exts, sizeof(e->exts), "%s", myos_trim(fld[0]));
        snprintf(e->desc, sizeof(e->desc), "%s", myos_trim(fld[1]));
        snprintf(e->icon, sizeof(e->icon), "%s", myos_trim(fld[2]));
        snprintf(e->open, sizeof(e->open), "%s", myos_trim(fld[3]));
        if (nf >= 5) snprintf(e->run, sizeof(e->run), "%s", myos_trim(fld[4]));
        for (char *p = e->exts; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (!e->exts[0] || !e->open[0]) continue;
        n++;
    }
    fclose(f);
    return n;
}

/* ユーザーの分を先に読む。探すときは先頭から見るので、
 * 同じ拡張子があればユーザーの指定が勝つ。 */
static inline int ft_load(FileType *t, int max)
{
    char user[512];
    snprintf(user, sizeof(user), "%s/.myos/filetypes.conf", myos_home());

    int n = 0;
    n = ft_load_file(user, t, n, max);
    n = ft_load_file(MYOS_ETC "/filetypes.conf", t, n, max);
    return n;
}

/* --- 検索 ---------------------------------------------------------------- */

/* "foo.tar.gz" -> "gz"。拡張子が無ければ NULL。 */
static inline const char *ft_ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return NULL;
    return dot + 1;
}

static inline int ft_ext_match(const char *list, const char *ext)
{
    size_t le = strlen(ext);
    for (const char *p = list; *p; ) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == le && !strncasecmp(p, ext, le)) return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

static inline const FileType *ft_find(const FileType *t, int n, const char *name)
{
    const char *ext = ft_ext_of(name);
    if (!ext) return NULL;
    for (int i = 0; i < n; i++)
        if (ft_ext_match(t[i].exts, ext)) return &t[i];
    return NULL;
}

/* --- コマンド文字列を argv に割る ---------------------------------------- */
/*
 * シェルを通さないので、ここで自分で割る。
 *   ' ' で区切る / '...' でくくった中は 1 つの語 / %1 はパスに置き換える
 * 置き換え後の文字列は store に詰め、argv はそこを指す。
 * 戻り値は語数。失敗したら 0。
 */
static inline int ft_argv(const char *cmd, const char *path,
                          char **argv, int maxargv,
                          char *store, size_t storen)
{
    size_t used = 0;
    int n = 0;
    const char *p = cmd;

    while (*p && n < maxargv - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char *out = store + used;
        size_t start = used;
        int quote = 0;

        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            if (*p == '\'') { quote = !quote; p++; continue; }
            if (p[0] == '%' && p[1] == '1') {
                size_t l = strlen(path);
                if (used + l + 1 >= storen) return 0;
                memcpy(store + used, path, l);
                used += l;
                p += 2;
                continue;
            }
            if (used + 2 >= storen) return 0;
            store[used++] = *p++;
        }
        if (used + 1 >= storen) return 0;
        store[used++] = 0;
        if (used > start) argv[n++] = out;
    }
    argv[n] = NULL;
    return n;
}

#endif /* FILETYPES_H */

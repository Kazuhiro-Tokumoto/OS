/* ==========================================================================
 * myos_install_text.c  -  インストーラの前半 (Windows 98 の青いテキスト画面)
 *
 * 実物の Windows 98 のセットアップは 2 段構えだった。
 *   前半: 青いテキストモード。ScanDisk が走り、FDISK と FORMAT をやる
 *   後半: GUI のウィザード。ファイルをコピーする
 * ここは前半にあたる。X は使わず、Linux のコンソールに直接描く。
 *
 * やること:
 *   1. ようこそ (ENTER で続行 / F3 で終了 … これも 98 と同じ)
 *   2. ディスクを探して一覧にする
 *   3. インストール先と入れ方 (丸ごと消す / 空き領域に入れる) を選ぶ
 *   4. 最終確認 (消える内容をはっきり出す)
 *   5. パーティションを切って ext4 で初期化する
 *
 * 決めた内容は /tmp/myos-install.conf に書く。後半の GUI がそれを読む。
 *
 * 描画は ANSI エスケープだけで済ませている。ncurses を持ち込むと
 * インストーラのためにライブラリが増えるので、それは避けた。
 *
 * ビルド:
 *   gcc -O2 -o myos-install-text myos_install_text.c
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_DISKS 16
#define CONF_PATH "/tmp/myos-install.conf"

/* Win98 のセットアップ画面の色。青地に白、見出しは黒地に水色。 */
#define C_RESET   "\033[0m"
#define C_SCREEN  "\033[44;37m"     /* 青地 / 白 */
#define C_BAR     "\033[47;34m"     /* 白地 / 青 … 上下のバー */
#define C_HI      "\033[44;97m"     /* 青地 / 明るい白 */
#define C_SEL     "\033[47;30m"     /* 白地 / 黒 … 選択中の行 */
#define C_WARN    "\033[44;93m"     /* 青地 / 黄 */

/* 画面の桁数と行数。決め打ちにしていたので、1024x768 のフレーム
 * バッファ (8x16 の字で 128x48) では左上の 80x25 しか塗られず、
 * 残りが黒いまま残っていた。起動時に実物を測る。 */
static int COLS = 80;
static int ROWS = 25;

static void probe_screen(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= 40 && ws.ws_row >= 10) {
        COLS = ws.ws_col;
        ROWS = ws.ws_row;
    }
    /* 上限。極端に広い画面で端から端まで塗ると、書き換えのたびに
     * 目に見えて遅くなる。 */
    if (COLS > 240) COLS = 240;
    if (ROWS > 80)  ROWS = 80;
}

typedef struct {
    char name[32];       /* sda */
    char model[64];
    long long bytes;
    int  removable;
    long long free_bytes; /* 末尾の空き (概算) */
    int  has_parts;
} Disk;

static Disk disks[MAX_DISKS];
static int  n_disks = 0;

/* --- 端末 ---------------------------------------------------------------- */
static struct termios saved_tio;

static void raw_mode(void)
{
    struct termios t;
    tcgetattr(0, &saved_tio);
    t = saved_tio;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}

static void restore_mode(void) { tcsetattr(0, TCSANOW, &saved_tio); }

static void cls(void)
{
    printf("\033[2J\033[H%s", C_SCREEN);
    /* 画面全体を青で塗る。端末によっては \033[2J で背景色が乗らない。 */
    for (int y = 0; y < ROWS; y++) {
        printf("\033[%d;1H", y + 1);
        for (int x = 0; x < COLS; x++) putchar(' ');
    }
    fflush(stdout);
}

static void at(int row, int col) { printf("\033[%d;%dH", row, col); }

static void say(int row, int col, const char *s) { at(row, col); fputs(s, stdout); }

/* 上下のバー。98 のセットアップと同じ位置。 */
static void frame(const char *title, const char *hint)
{
    printf("%s", C_BAR);
    at(1, 1);
    for (int x = 0; x < COLS; x++) putchar(' ');
    at(1, 3);
    fputs(title, stdout);

    at(ROWS, 1);
    for (int x = 0; x < COLS; x++) putchar(' ');
    at(ROWS, 3);
    fputs(hint, stdout);
    printf("%s", C_SCREEN);
}

/* 押されたキーを 1 つ返す。矢印などは 1000 番台に丸める。
 *
 * 入力が閉じたら (EOF) その場で終わる。ここで EOF を返し続けると
 * 呼び出し側の for(;;) が回りっぱなしになり、CPU を食って止まらない。
 * 実機のコンソールでは起きないが、パイプ越しに動かすと必ず踏む。 */
static int getkey(void)
{
    int c = getchar();
    if (c == EOF) {
        restore_mode();
        printf("\033[0m\n");
        exit(1);
    }
    if (c != 0x1B) return c;
    /* 矢印などのエスケープ列 */
    int a = getchar();
    if (a != '[') return 0x1B;
    int b = getchar();
    switch (b) {
    case 'A': return 1000;   /* 上 */
    case 'B': return 1001;   /* 下 */
    default:
        /* F3 は \033[13~ など端末で違う。数字が続く形は読み捨てる。 */
        while (b >= '0' && b <= '9') b = getchar();
        return 1002;
    }
}

/* --- ディスクを探す ------------------------------------------------------ */
static long long read_ll(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long long v = -1;
    if (fscanf(f, "%lld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void read_str(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = 0;
    }
    fclose(f);
}

/* 起動に使った媒体のディスク名。USB から起動したときに要る。
 *
 * CD (sr*) は下の一覧から外しているが、USB メモリは sd* なので普通に
 * インストール先の候補として並んでしまう。しかも USB から起動すると
 * その USB が sda になることが多く、既定の選択がそれになる。選んだら
 * 動いている最中に自分自身を消すことになる。 */
static char boot_disk[32] = "";
static int  boot_disk_hidden = 0;   /* 一覧から外したかどうか */

static void find_boot_disk(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;

    char dev[128], mp[128];
    while (fscanf(f, "%127s %127s %*[^\n]", dev, mp) == 2) {
        if (strcmp(mp, "/myos-medium")) continue;

        const char *b = strrchr(dev, '/');
        b = b ? b + 1 : dev;

        /* パーティション (sdb1) なら親のディスク (sdb) まで遡る。
         * /sys/class/block/sdb1 の実体は
         *   ../../devices/.../block/sdb/sdb1
         * なので、1 つ上の名前が親になる。nvme0n1p1 でも同じ形。 */
        char link[256], real[512];
        snprintf(link, sizeof(link), "/sys/class/block/%s", b);
        ssize_t n = readlink(link, real, sizeof(real) - 1);
        if (n > 0) {
            real[n] = 0;
            char *last = strrchr(real, '/');
            if (last) {
                *last = 0;
                char *par = strrchr(real, '/');
                par = par ? par + 1 : real;
                if (strcmp(par, "block"))       /* 親が block なら本体そのもの */
                    b = par;
            }
        }
        snprintf(boot_disk, sizeof(boot_disk), "%s", b);
        break;
    }
    fclose(f);
}

/* /sys/block を見て、実体のあるディスクだけ拾う。
 * loop / ram / sr は除く (sr は読み取り専用の光学ドライブ)。 */
static void scan_disks(void)
{
    find_boot_disk();

    DIR *d = opendir("/sys/block");
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) && n_disks < MAX_DISKS) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        if (!strncmp(n, "loop", 4) || !strncmp(n, "ram", 3) ||
            !strncmp(n, "sr", 2)   || !strncmp(n, "dm-", 3) ||
            !strncmp(n, "md", 2)   || !strncmp(n, "zram", 4)) continue;

        /* 起動した媒体そのものは出さない。USB から起動したときに
         * 自分を消させないため。 */
        if (boot_disk[0] && !strcmp(n, boot_disk)) { boot_disk_hidden = 1; continue; }

        char p[256];
        snprintf(p, sizeof(p), "/sys/block/%s/size", n);
        long long sectors = read_ll(p);
        if (sectors <= 0) continue;

        Disk *dk = &disks[n_disks];
        memset(dk, 0, sizeof(*dk));
        snprintf(dk->name, sizeof(dk->name), "%s", n);
        dk->bytes = sectors * 512LL;

        snprintf(p, sizeof(p), "/sys/block/%s/removable", n);
        dk->removable = (read_ll(p) == 1);

        snprintf(p, sizeof(p), "/sys/block/%s/device/model", n);
        read_str(p, dk->model, sizeof(dk->model));
        if (!dk->model[0]) snprintf(dk->model, sizeof(dk->model), "Disk");

        /* パーティションが既にあるか。%s%d と %sp%d の両方を見る
         * (nvme0n1p1 のような名前があるため)。 */
        for (int i = 1; i <= 8; i++) {
            snprintf(p, sizeof(p), "/sys/block/%s/%s%d", n, n, i);
            if (access(p, F_OK) == 0) { dk->has_parts = 1; break; }
            snprintf(p, sizeof(p), "/sys/block/%s/%sp%d", n, n, i);
            if (access(p, F_OK) == 0) { dk->has_parts = 1; break; }
        }
        n_disks++;
    }
    closedir(d);
}

static void human(long long b, char *out, size_t n)
{
    if (b >= (1LL << 30)) snprintf(out, n, "%.1f GB", (double)b / (1LL << 30));
    else                  snprintf(out, n, "%.0f MB", (double)b / (1LL << 20));
}

/* --- 画面 ---------------------------------------------------------------- */
static void page_welcome(void)
{
    cls();
    frame(" myOS Setup ", " ENTER=Continue   F3=Exit ");

    say(4,  6, "Welcome to Setup.");
    say(6,  6, "This program prepares myOS to run on your computer.");

    say(9,  6, "  * To set up myOS now, press ENTER.");
    say(11, 6, "  * To quit Setup and shut down, press F3.");

    say(15, 6, "Setup will ask you which disk to use before changing");
    say(16, 6, "anything. Nothing is written until you confirm.");

    /* 「使った時点で同意したものとする」と決めた以上、その文面は
     * どこかで実際に見えていないと成り立たない。文書の中だけに
     * 書いてあっても、読んでいない人に同意させたことにはならない。
     * 最初の画面に出す。 */
    say(19, 6, "myOS is provided as is. The author takes no responsibility");
    say(20, 6, "for anything that happens. By using it you accept that.");
    say(21, 6, "myOS itself is MIT licensed. See LICENSE in the source tree.");
    fflush(stdout);
}

static int page_select_disk(void)
{
    int sel = 0;
    for (;;) {
        cls();
        frame(" myOS Setup - Select Disk ",
              " UP/DOWN=Move   ENTER=Select   F3=Exit ");

        say(3, 6, "Setup found the following disks on your computer.");
        say(4, 6, "Select the disk where you want to install myOS.");

        for (int i = 0; i < n_disks; i++) {
            char size[32], line[128];
            human(disks[i].bytes, size, sizeof(size));
            snprintf(line, sizeof(line), " %-8s %-24.24s %10s  %-12s ",
                     disks[i].name, disks[i].model, size,
                     disks[i].has_parts ? "has data" : "empty");
            at(7 + i, 8);
            fputs(i == sel ? C_SEL : C_SCREEN, stdout);
            fputs(line, stdout);
            fputs(C_SCREEN, stdout);
        }

        if (n_disks == 0) {
            fputs(C_WARN, stdout);
            say(8, 8, "No disks were found. Setup cannot continue.");
            /* 起動した媒体を外したせいで空になった場合は、そう言う。
             * 黙って「見つかりません」だけだと、ディスクを認識できて
             * いないのだと思われる。 */
            if (boot_disk_hidden)
                say(10, 8, "The drive you started Setup from is not offered "
                           "as a target.");
            fputs(C_SCREEN, stdout);
        }
        fflush(stdout);

        int k = getkey();
        if (k == 1000 && sel > 0) sel--;
        else if (k == 1001 && sel < n_disks - 1) sel++;
        else if ((k == '\r' || k == '\n') && n_disks > 0) return sel;
        else if (k == 1002 || k == 'q') return -1;
    }
}

/* 入れ方。丸ごと消すか、空き領域に入れるか。 */
static int page_select_mode(const Disk *d)
{
    int sel = 0;
    for (;;) {
        char size[32];
        human(d->bytes, size, sizeof(size));

        cls();
        frame(" myOS Setup - Installation Type ",
              " UP/DOWN=Move   ENTER=Select   ESC=Back ");

        char head[128];
        snprintf(head, sizeof(head), "Disk %s  (%s, %s)",
                 d->name, d->model, size);
        say(3, 6, head);
        say(5, 6, "How should Setup use this disk?");

        const char *opt[2] = {
            " Erase the entire disk and install myOS         ",
            " Install myOS in the unused space               ",
        };
        const char *desc[2][2] = {
            { "Everything on this disk will be removed.",
              "This is the simplest and most reliable choice." },
            { "Existing partitions are kept.",
              "Requires unpartitioned free space on the disk." },
        };

        for (int i = 0; i < 2; i++) {
            at(8 + i * 5, 8);
            fputs(i == sel ? C_SEL : C_SCREEN, stdout);
            fputs(opt[i], stdout);
            fputs(C_SCREEN, stdout);
            say(9 + i * 5, 10, desc[i][0]);
            say(10 + i * 5, 10, desc[i][1]);
        }
        fflush(stdout);

        int k = getkey();
        if (k == 1000 && sel > 0) sel--;
        else if (k == 1001 && sel < 1) sel++;
        else if (k == '\r' || k == '\n') return sel;
        else if (k == 0x1B) return -1;
    }
}

/* 最後の確認。消えるものをはっきり出す。 */
static int page_confirm(const Disk *d, int whole)
{
    cls();
    frame(" myOS Setup - Confirm ", " F8=Continue   ESC=Back ");

    char size[32], line[128];
    human(d->bytes, size, sizeof(size));

    say(4, 6, "Setup is about to write to your disk.");

    snprintf(line, sizeof(line), "Disk        : %s  (%s)", d->name, d->model);
    say(7, 6, line);
    snprintf(line, sizeof(line), "Capacity    : %s", size);
    say(8, 6, line);
    snprintf(line, sizeof(line), "Method      : %s",
             whole ? "Erase the entire disk" : "Use the unused space");
    say(9, 6, line);
    say(10, 6, "File system : ext4");

    if (whole) {
        fputs(C_WARN, stdout);
        say(13, 6, "WARNING");
        say(14, 6, "Everything on this disk will be permanently erased,");
        say(15, 6, "including any other operating system and all files.");
        fputs(C_SCREEN, stdout);
    } else {
        say(13, 6, "Existing partitions will be kept. Setup will only use");
        say(14, 6, "the unpartitioned space at the end of the disk.");
    }

    fputs(C_HI, stdout);
    say(18, 6, "Press F8 to continue, or ESC to go back.");
    fputs(C_SCREEN, stdout);
    fflush(stdout);

    for (;;) {
        int k = getkey();
        /* F8 は端末によって列が違う。数字付きのエスケープ列は
         * getkey が 1002 に丸めるので、それを F8 とみなす。
         * 分かりやすいように 'y' も受ける。 */
        if (k == 1002 || k == 'y' || k == 'Y') return 1;
        if (k == 0x1B || k == 'n' || k == 'N') return 0;
    }
}

/* 既にあるパーティションの数。空き領域に足すとき、
 * 新しい番号が何番から始まるかを知るために要る。 */
static int count_parts_before(const Disk *d)
{
    int n = 0;
    for (int i = 1; i <= 16; i++) {
        char p[256];
        snprintf(p, sizeof(p), "/sys/block/%s/%s%d", d->name, d->name, i);
        if (access(p, F_OK) == 0) { n++; continue; }
        snprintf(p, sizeof(p), "/sys/block/%s/%sp%d", d->name, d->name, i);
        if (access(p, F_OK) == 0) n++;
    }
    return n;
}

/* --- 実際の書き込み ------------------------------------------------------ */
static int run(char *const argv[])
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* 失敗したら何が失敗したかを出して止まる。
 * 黙って前の画面に戻ると「なぜか中止された」ようにしか見えず、
 * 直しようが無い。 */
static void fail_page(const char *what, int rc)
{
    cls();
    frame(" myOS Setup - Error ", " Press any key ");
    say(6, 6, "Setup could not prepare the disk.");
    char line[128];
    snprintf(line, sizeof(line), "The command '%s' failed (exit %d).",
             what, rc);
    say(8, 6, line);
    say(11, 6, "This usually means the tool is missing from the");
    say(12, 6, "installation medium, or the disk is in use.");
    fflush(stdout);
    getkey();
}

/* パーティションを切って ext4 で初期化する。
 * ここが 98 でいう FDISK と FORMAT にあたる。 */
/* スワップをどれだけ取るか (MB)。0 なら作らない。
 *
 * 入れる相手は Core 2 Duo 世代 (2006-) の実機を想定していて、
 * メモリが 1GB や 2GB の機械が普通にある。そこで Firefox を開くと
 * スワップが無い場合は OOM killer が走って、何の前触れもなく
 * アプリが消える。「重い」ではなく「壊れた」に見えるので用意する。
 *
 * 目安はメモリと同じだけ。休止状態は作らないので倍は要らない。
 * 4GB を超えても使い切ることはまず無いので頭を打つ。
 * ルートに 2.5GB 残せないなら作らない (入らないほうが困る)。 */
/* ルートを 2 本にできるか、できるならいくつずつ取るか。
 *
 * 0 を返したら「無理なので今までどおり 1 本」。
 *
 * 2 本にする理由は更新のため。使っていないほうへ新しい中身を展開して、
 * ペイロードテーブルの root= を書き換えるだけで入れ替わる。壊れていたら
 * 起動中に R を押せば 1 つ前のテーブルで立つ。カーネルの A/B と
 * まったく同じ仕掛けが、そのままルートにも効く。
 *
 * 小さいディスクでは諦める。古い機械を切り捨てたくないので、
 * 「A/B が取れないなら入れられない」にはしない。
 */
static long root_ab_mb(long long avail_bytes, long swap)
{
    long long disk_mb = avail_bytes / (1024 * 1024);
    /* 起動領域 256MB とスワップを引いた残りを、root x2 と home で分ける */
    long long rest = disk_mb - 256 - swap;

    /* home に最低これだけは残す。1GB では「入れた直後から置き場が無い」
     * のと変わらないので、実用になる線を引く。 */
    const long long HOME_MIN = 4096;
    /* ルート 1 本の既定と下限。myOS 本体が 2.5GB あり、apt で入れる
     * ものを足すと 4GB では窮屈。8GB あれば当面困らない。
     *
     * 下限を 6GB と高めに置いているのは、**縮めてまで A/B にする値打ちが
     * 無い**から。16GB のディスクで 4.7GB ずつに割るより、13GB の 1 本の
     * ほうが日々使いやすい。更新の便利さと、そもそも使えることを
     * 秤にかけて、後者を採る。 */
    const long long WANT = 8192;
    const long long MIN  = 6144;

    long long each = WANT;
    if (rest - each * 2 < HOME_MIN)
        each = (rest - HOME_MIN) / 2;   /* home を守って縮める */
    if (each < MIN) return 0;           /* 縮めても足りない。1 本でいく */
    if (each > WANT) each = WANT;
    return (long)each;
}

static long swap_mb(long long avail_bytes)
{
    long ram_mb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char k[64];
        long long v;
        while (fscanf(f, "%63s %lld %*[^\n]", k, &v) == 2)
            if (!strcmp(k, "MemTotal:")) { ram_mb = (long)(v / 1024); break; }
        fclose(f);
    }
    long long disk_mb = avail_bytes / (1024 * 1024);

    long mb = ram_mb > 0 ? ram_mb : 1024;
    if (mb > 4096) mb = 4096;           /* これ以上あっても使い切らない */

    /* ディスクの 15% まで。小さいディスクでメモリが多い機械
     * (古いノートに 4GB 載せたような構成) で、スワップが
     * ディスクの半分を食うのを防ぐ。 */
    long long cap = disk_mb * 15 / 100;
    if (mb > cap) mb = (long)cap;

    /* ルートに 3GB は残す。myOS 自体が 2.5GB あるので、
     * ここを削ると入れた直後から空きが無い。 */
    long long room_mb = disk_mb - 256 - 3072;
    if (room_mb < mb) mb = (long)room_mb;

    if (mb < 512) return 0;             /* 中途半端に取るくらいなら作らない */
    return mb;
}

/* ルート B と home。A/B が取れなかったときは空のまま。
 * 引数で持ち回すと do_partition の顔が長くなりすぎるのでここに置く。 */
static char rootb[64];
static char home[64];

static int do_partition(const Disk *d, int whole, char *rootdev, size_t rn,
                        char *swapdev, size_t sn, int *first_part)
{
    char dev[64];
    snprintf(dev, sizeof(dev), "/dev/%s", d->name);

    cls();
    frame(" myOS Setup - Preparing Disk ", " Please wait ");
    say(6, 6, "Setup is preparing the disk. This takes a moment.");
    say(8, 6, "  Creating partitions ...");
    fflush(stdout);

    /* どちらの入れ方でも同じ形にする。
     *   1 つ目 … カーネルを生で置く場所 (256MB)。ファイルシステムは作らない
     *   2 つ目 … ルート (ext4)
     *
     * ブートローダーは ext4 を読めないので、カーネルは生のセクタに置く。
     * 場所をパーティションとして宣言しておけば、他のものに使われない。 */
    /* どちらの入れ方でも同じ形にする。
     *   1 つ目 … カーネルを生で置く場所 (256MB)
     *   2 つ目 … スワップ (取れるときだけ)
     *   3 つ目 … ルート (ext4、残り全部)
     *
     * スワップをルートより前に置くのは、ルートを「残り全部」で
     * 済ませたいから。後ろに置くと、ルートの大きさを自分で計算する
     * ことになる。MBR の基本区画は 4 つまでなので、空き領域へ入れる
     * ときは既存が 2 つまで、という制限も付く。そこは諦めて、
     * スワップだけ落として続ける。 */
    long sw = swap_mb(whole ? d->bytes : d->free_bytes);

    /* ルートを 2 本取れるか。取れたら更新でまるごと入れ替えられる。
     * 「ディスク全体を使う」ときだけ。空き領域へ足す入れ方では、
     * 既存の区画がいくつあるか分からず、基本区画 4 つの枠に収まる
     * 保証が無い。そこまで面倒を見ると失敗の仕方が増える。 */
    long ab = whole ? root_ab_mb(d->bytes, sw) : 0;

    char cmd[256];
    FILE *fp;
    int rc;

    for (;;) {
        if (whole) {
            snprintf(cmd, sizeof(cmd), "sfdisk %s >/dev/null 2>&1", dev);
            fp = popen(cmd, "w");
            if (!fp) return 0;
            fprintf(fp, "label: dos\n");
            /* 256MB。カーネル 20MB のほかに、GPU と無線のファームウェアを
             * 詰めた initramfs (90MB 前後) が入る。128MB でも今は収まるが
             * 余裕が 16MB しか無く、ファームが増えるたびに危うくなる。 */
            fprintf(fp, ",256M,83,*\n");
            if (sw) fprintf(fp, ",%ldM,82\n", sw);
            if (ab) {
                /* ルート A、そのあと拡張区画にルート B と home。
                 *
                 * MBR の基本区画は 4 つまで。起動領域・スワップ・
                 * ルート A で 3 つ使うので、残りは 1 つしかない。
                 * そこを拡張区画にして、中にルート B と home を置く。
                 *
                 * カーネルは区画を見ずに生 LBA で読むので、
                 * ブートローダーには何の影響も無い。Linux は論理区画から
                 * 普通に起動できる。 */
                fprintf(fp, ",%ldM,83\n", ab);   /* root A */
                fprintf(fp, ",,5\n");            /* 拡張区画 (残り全部) */
                fprintf(fp, ",%ldM,83\n", ab);   /* root B (論理) */
                fprintf(fp, ",,83\n");           /* home  (論理、残り) */
            } else {
                fprintf(fp, ",,83\n");
            }
            rc = pclose(fp);
            *first_part = 1;
        } else {
            snprintf(cmd, sizeof(cmd), "sfdisk --append %s >/dev/null 2>&1",
                     dev);
            fp = popen(cmd, "w");
            if (!fp) return 0;
            fprintf(fp, ",256M,83\n");
            if (sw) fprintf(fp, ",%ldM,82\n", sw);
            fprintf(fp, ",,83\n");
            rc = pclose(fp);
            *first_part = count_parts_before(d) + 1;
        }
        if (rc == 0) break;
        /* 諦める順。まず A/B、次にスワップ。
         * どちらも「無いと入らない」ものではないので、
         * 入れられることを優先する。 */
        if (ab) { ab = 0; continue; }
        if (sw) { sw = 0; continue; }
        fail_page(whole ? "sfdisk" : "sfdisk --append", rc);
        return 0;
    }

    /* パーティション名。nvme は p が入る。 */
    const char *sep = (strstr(d->name, "nvme") || strstr(d->name, "mmcblk"))
                      ? "p" : "";
    swapdev[0] = 0;
    if (sw)
        snprintf(swapdev, sn, "/dev/%s%s%d", d->name, sep, *first_part + 1);
    snprintf(rootdev, rn, "/dev/%s%s%d", d->name, sep,
             *first_part + (sw ? 2 : 1));

    /* ルート B と home。論理区画は基本区画がいくつあっても **必ず 5 から**
     * 数える。スワップの有無で番号がずれない (実際に切って確かめた)。 */
    rootb[0] = home[0] = 0;
    if (ab) {
        snprintf(rootb, sizeof(rootb), "/dev/%s%s5", d->name, sep);
        snprintf(home,  sizeof(home),  "/dev/%s%s6", d->name, sep);
    }

    /* カーネルにテーブルを読み直させる */
    char *pr[] = { "partprobe", dev, NULL };
    run(pr);
    sleep(2);

    if (swapdev[0]) {
        say(9, 6, "  Preparing swap ...");
        fflush(stdout);
        char *ms[] = { "mkswap", "-L", "myos-swap", swapdev, NULL };
        /* ここで失敗しても入れるのはやめない。
         * スワップが無くても動くし、無いことは fstab に書かなければ
         * そのまま伝わる。 */
        if (run(ms) != 0) swapdev[0] = 0;
        say(10, 6, "  Formatting as ext4 ...");
    } else {
        say(9, 6, "  Formatting as ext4 ...");
    }
    fflush(stdout);

    char *mk[] = { "mkfs.ext4", "-F", "-q", "-m", "0", "-L", "myos",
                   rootdev, NULL };
    rc = run(mk);
    if (rc != 0) { fail_page("mkfs.ext4", rc); return 0; }

    if (rootb[0]) {
        /* ルート B。いまは空のまま置く。更新のときに、ここへ新しい
         * 中身を展開して切り替える。空でも困らないのは、切り替えるのは
         * 展開し終えてからだから。 */
        say(11, 6, "  Preparing the spare system area ...");
        fflush(stdout);
        char *mb[] = { "mkfs.ext4", "-F", "-q", "-m", "0", "-L", "myos-b",
                       rootb, NULL };
        /* ここで転んでも入れるのはやめない。A/B が使えなくなるだけで、
         * 入れた myOS は普通に動く。使えないことは boot.conf に
         * 書かなければそのまま伝わる。 */
        if (run(mb) != 0) rootb[0] = 0;
    }

    if (home[0]) {
        /* home は共有。ルートを入れ替えても、ここは触らない。
         * これが無いと A/B にする意味が半分無くなる (自分のファイルが
         * 古いほうに置き去りになる)。 */
        say(11, 6, "  Formatting the home area ...");
        fflush(stdout);
        char *mh[] = { "mkfs.ext4", "-F", "-q", "-m", "0", "-L", "myos-home",
                       home, NULL };
        if (run(mh) != 0) { home[0] = 0; rootb[0] = 0; }
    }

    say(12, 6, "Done.");
    fflush(stdout);
    return 1;
}

/* 生領域 (1 つ目のパーティション) が何セクタ目から始まるか。
 * sfdisk が決めるので、切ったあとに sysfs から読むのが確実。 */
static long long part_start(const Disk *d, int idx)
{
    char p[256];
    snprintf(p, sizeof(p), "/sys/block/%s/%s%d/start", d->name, d->name, idx);
    long long v = read_ll(p);
    if (v > 0) return v;
    snprintf(p, sizeof(p), "/sys/block/%s/%sp%d/start", d->name, d->name, idx);
    v = read_ll(p);
    return v > 0 ? v : 2048;
}

/* --- 本体 ---------------------------------------------------------------- */
int main(void)
{
    probe_screen();
    raw_mode();
    atexit(restore_mode);

    page_welcome();
    for (;;) {
        int k = getkey();
        if (k == '\r' || k == '\n') break;
        /* 2 は「使う人がやめた」。1 は「途中で失敗した」。
         * myos-install-init がこれを見て出口を変える。
         * やめたときは電源を切り、失敗したときは shell を出す。 */
        if (k == 1002 || k == 'q') { restore_mode(); cls(); printf(C_RESET "\n"); return 2; }
    }

    scan_disks();

    int di, mode;
    for (;;) {
        di = page_select_disk();
        if (di < 0) { restore_mode(); cls(); printf(C_RESET "\n"); return 2; }

        mode = page_select_mode(&disks[di]);
        if (mode < 0) continue;

        if (page_confirm(&disks[di], mode == 0)) break;
    }

    char rootdev[64], swapdev[64] = "";
    int first_part = 1;
    if (!do_partition(&disks[di], mode == 0, rootdev, sizeof(rootdev),
                      swapdev, sizeof(swapdev), &first_part)) {
        restore_mode();
        cls();
        printf(C_RESET "\n");
        return 1;
    }

    long long boot_lba = part_start(&disks[di], first_part);

    /* 決めた内容を後半へ渡す */
    FILE *f = fopen(CONF_PATH, "w");
    if (f) {
        fprintf(f, "disk    = /dev/%s\n", disks[di].name);
        fprintf(f, "root    = %s\n", rootdev);
        fprintf(f, "whole   = %d\n", mode == 0 ? 1 : 0);
        fprintf(f, "bootlba = %lld\n", boot_lba);
        if (swapdev[0]) fprintf(f, "swap    = %s\n", swapdev);
        /* A/B が取れたときだけ書く。書かなければ、後半も更新の仕掛けも
         * 「A/B は無い」として動く。無い機械に嘘を伝えないほうが安全。 */
        if (rootb[0]) fprintf(f, "rootb   = %s\n", rootb);
        if (home[0])  fprintf(f, "home    = %s\n", home);
        fclose(f);
    }

    restore_mode();
    cls();
    printf(C_RESET "\n");
    return 0;
}

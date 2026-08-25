/* ==========================================================================
 * myos_devmgr.c  -  デバイスマネージャー (Windows 98 のあれ)
 *
 * 何が刺さっていて、そのうちどれにドライバが当たっているかを見る。
 *
 * 作った理由は見た目ではなく切り分けのため。
 * 「音が出ない」「無線が繋がらない」と言われたとき、原因は大きく 3 つある。
 *   1. そもそも機器が見えていない (刺さっていない / BIOS で無効)
 *   2. 見えているがドライバが当たっていない (カーネルに入っていない)
 *   3. ドライバは当たっているがファームウェアが無い
 * この 3 つは対処がまるで違うのに、症状は同じ「動かない」になる。
 * ここで区別できるようにしておく。
 *
 * 情報の出どころは sysfs と dmesg だけ。lspci も pci.ids も入れていない
 * (pci.ids だけで 1MB を超える。CD に収める余裕がない)。
 * なので機種名は出せない。代わりに
 *   ベンダー名 (よく見るものだけ内蔵の表から) + PCI のクラス名
 * で「Intel PCI Multimedia Audio Device」のように出す。
 * Windows 9x が知らない機器に付けていた名前と同じ形で、
 * 切り分けにはこれで足りる。
 *
 * ビルド:
 *   gcc -O2 -o myos-devmgr myos_devmgr.c -lX11 -lXft
 * ========================================================================== */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "x98.h"

#define WIN_W   560
#define WIN_H   478
#define PAD     12
#define LIST_X  PAD
#define LIST_Y  46
#define LIST_W  (WIN_W - PAD * 2)
#define LIST_H  344
#define ROW_H   18
#define BTN_W   92
#define BTN_H   24

#define MAX_DEV 256
#define MAX_ROW (MAX_DEV + 16)

/* --- カテゴリ ------------------------------------------------------------
 * 並び順がそのまま画面の並び順になる。Windows 98 の並びに寄せてある。 */
enum {
    CAT_DISK, CAT_DISPLAY, CAT_SOUND, CAT_NET, CAT_USB,
    CAT_PORT, CAT_HID, CAT_SYSTEM, CAT_OTHER, CAT_N
};

static const char *cat_name[CAT_N] = {
    "Disk drives and controllers",
    "Display adapters",
    "Sound, video and game controllers",
    "Network adapters",
    "Universal Serial Bus controllers",
    "Ports",
    "Human interface devices",
    "System devices",
    "Other devices",
};

typedef struct {
    char name[128];         /* 画面に出す名前 */
    char id[24];            /* 8086:1234 */
    char bus[8];            /* "PCI" / "USB" */
    char addr[40];          /* 0000:00:1b.0 / 1-2 */
    char driver[40];        /* 束ねられているドライバ ("" = 無し) */
    char node[40];          /* eth0 / card0 など、使う側から見える名前 */
    char extra[96];         /* 追加の一言 (ALSA のカード名など) */
    int  irq;               /* -1 = 不明 */
    int  cat;
    int  fw_missing;        /* ファームウェアが読めていない */
} Dev;

static Dev  devs[MAX_DEV];
static int  n_devs;

/* 画面の行。カテゴリの見出しか、機器のどちらか。 */
typedef struct { int cat; int dev; } Row;   /* dev < 0 なら見出し */
static Row  rows[MAX_ROW];
static int  n_rows;

static int  cat_open[CAT_N];
static int  sel = 0, top = 0;
static int  showing_props = 0;

static Display *dpy;
static int      screen;
static Window   win;
static X98      x98;
static XIC      ic;

/* dmesg から拾った「ファームウェアが読めなかった」ドライバ名の並び */
static char fw_bad[32][40];
static int  n_fw_bad;

/* --- 小道具 -------------------------------------------------------------- */

/* sysfs の 1 行ものを読む。末尾の改行は落とす。無ければ 0 を返す。 */
static int rd1(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) { buf[0] = 0; return 0; }
    buf[r] = 0;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    return buf[0] != 0;
}

static unsigned long rdhex(const char *path)
{
    char b[64];
    if (!rd1(path, b, sizeof(b))) return 0;
    return strtoul(b, NULL, 16);
}

static int rdint(const char *path)
{
    char b[64];
    if (!rd1(path, b, sizeof(b))) return -1;
    return (int)strtol(b, NULL, 10);
}

/* シンボリックリンクの最後の要素だけ取る (driver -> ../../../bus/pci/drivers/e1000e) */
static int link_base(const char *path, char *buf, size_t n)
{
    char t[512];
    ssize_t r = readlink(path, t, sizeof(t) - 1);
    if (r <= 0) { buf[0] = 0; return 0; }
    t[r] = 0;
    char *s = strrchr(t, '/');
    snprintf(buf, n, "%s", s ? s + 1 : t);
    return 1;
}

/* --- ベンダー名 -----------------------------------------------------------
 * pci.ids は入れていないので、よく見るものだけ内蔵で持つ。
 * 載っていなければ 16 進のまま出す。嘘の名前を出すよりましなので、
 * 迷ったら足さない。 */
typedef struct { unsigned short id; const char *name; } Vendor;
static const Vendor vendors[] = {
    { 0x1002, "ATI/AMD" },      { 0x1013, "Cirrus Logic" },
    { 0x1022, "AMD" },          { 0x1033, "NEC" },
    { 0x1039, "SiS" },          { 0x104c, "Texas Instruments" },
    { 0x1077, "QLogic" },       { 0x1095, "Silicon Image" },
    { 0x10de, "NVIDIA" },       { 0x10ec, "Realtek" },
    { 0x1102, "Creative" },     { 0x1106, "VIA" },
    { 0x1180, "Ricoh" },        { 0x11ab, "Marvell" },
    { 0x1217, "O2 Micro" },     { 0x125d, "ESS" },
    { 0x126f, "Silicon Motion" },{ 0x1274, "Ensoniq" },
    { 0x1283, "ITE" },          { 0x12d8, "Pericom" },
    { 0x1344, "Micron" },       { 0x144d, "Samsung" },
    { 0x14c3, "MediaTek" },     { 0x14e4, "Broadcom" },
    { 0x15ad, "VMware" },       { 0x15b7, "SanDisk" },
    { 0x1631, "Packard Bell" }, { 0x1668, "Actiontec" },
    { 0x168c, "Qualcomm Atheros" },
    { 0x1912, "Renesas" },      { 0x1969, "Attansic" },
    { 0x1987, "Phison" },       { 0x197b, "JMicron" },
    { 0x1af4, "Red Hat (virtio)" },
    { 0x1b21, "ASMedia" },      { 0x1b36, "Red Hat (QEMU)" },
    { 0x1b73, "Fresco Logic" }, { 0x1c5c, "SK hynix" },
    { 0x1cc1, "ADATA" },        { 0x1d6a, "Aquantia" },
    { 0x2646, "Kingston" },     { 0x3388, "Hint" },
    { 0x8086, "Intel" },        { 0x80ee, "VirtualBox" },
    { 0x9710, "MosChip" },      { 0xffff, NULL },
};

static const char *vendor_name(unsigned int v)
{
    for (int i = 0; vendors[i].name; i++)
        if (vendors[i].id == v) return vendors[i].name;
    return NULL;
}

/* --- PCI のクラス --------------------------------------------------------
 * class は 0xCCSSPP。CC = クラス、SS = サブクラス、PP = インターフェース。
 * ここも「よく出るものだけ」。分からなければクラスだけ言う。 */
static void pci_class_name(unsigned long cls, char *out, size_t n, int *cat)
{
    unsigned cc = (cls >> 16) & 0xFF, ss = (cls >> 8) & 0xFF;
    const char *s = NULL;
    int c = CAT_SYSTEM;

    switch (cc) {
    case 0x01:
        c = CAT_DISK;
        switch (ss) {
        case 0x01: s = "IDE Controller";          break;
        case 0x06: s = "SATA Controller (AHCI)";  break;
        case 0x07: s = "SAS Controller";          break;
        case 0x08: s = "NVMe Controller";         break;
        default:   s = "Mass Storage Controller"; break;
        }
        break;
    case 0x02:
        c = CAT_NET;
        s = (ss == 0x80) ? "Network Controller" : "Ethernet Controller";
        break;
    case 0x03:
        c = CAT_DISPLAY;
        s = (ss == 0x00) ? "VGA Display Controller" : "Display Controller";
        break;
    case 0x04:
        c = CAT_SOUND;
        switch (ss) {
        case 0x00: s = "Multimedia Video Device";     break;
        case 0x01: s = "Multimedia Audio Device";     break;
        case 0x03: s = "Audio Device (HD Audio)";     break;
        default:   s = "Multimedia Device";           break;
        }
        break;
    case 0x05: s = "Memory Controller";  c = CAT_SYSTEM; break;
    case 0x06:
        c = CAT_SYSTEM;
        switch (ss) {
        case 0x00: s = "Host Bridge";     break;
        case 0x01: s = "ISA Bridge";      break;
        case 0x04: s = "PCI-to-PCI Bridge"; break;
        default:   s = "Bridge";          break;
        }
        break;
    case 0x07: s = "Communication Device"; c = CAT_PORT;   break;
    case 0x08: s = "System Peripheral";    c = CAT_SYSTEM; break;
    case 0x09: s = "Input Device";         c = CAT_HID;    break;
    case 0x0b: s = "Processor";            c = CAT_SYSTEM; break;
    case 0x0c:
        switch (ss) {
        case 0x03: s = "USB Controller";   c = CAT_USB;    break;
        case 0x05: s = "SMBus Controller"; c = CAT_SYSTEM; break;
        default:   s = "Serial Bus Controller"; c = CAT_SYSTEM; break;
        }
        break;
    case 0x0d: s = "Wireless Controller";  c = CAT_NET;    break;
    case 0x10: s = "Encryption Device";    c = CAT_SYSTEM; break;
    case 0x11: s = "Signal Processing Device"; c = CAT_SYSTEM; break;
    default:   s = "PCI Device";           c = CAT_SYSTEM; break;
    }
    snprintf(out, n, "%s", s);
    *cat = c;
}

/* USB は bInterfaceClass で仕分ける。 */
static int usb_cat(unsigned cls)
{
    switch (cls) {
    case 0x01: return CAT_SOUND;
    case 0x02: case 0xe0: return CAT_NET;
    case 0x03: return CAT_HID;
    case 0x07: return CAT_PORT;
    case 0x08: return CAT_DISK;
    case 0x09: return CAT_USB;
    default:   return CAT_USB;
    }
}

/* --- dmesg からファームウェアの失敗を拾う ---------------------------------
 * カーネルが出すのは
 *   Direct firmware load for iwlwifi-xxx.ucode failed with error -2
 *   iwlwifi 0000:03:00.0: firmware: failed to load ...
 * のどちらか。前後の行にドライバ名が出ているので、
 * 「行の中に出てくる語」を全部控えておいて、あとで機器のドライバ名と
 * 突き合わせる。多めに拾って困らない情報なので、緩めに見る。 */
static void note_fw_word(const char *w)
{
    if (!*w || n_fw_bad >= 32) return;
    for (int i = 0; i < n_fw_bad; i++)
        if (!strcmp(fw_bad[i], w)) return;
    snprintf(fw_bad[n_fw_bad++], sizeof(fw_bad[0]), "%s", w);
}

static void scan_dmesg(void)
{
    int fd[2];
    if (pipe(fd) < 0) return;
    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return; }
    if (p == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, 2); close(null); }
        close(fd[1]);
        char *av[] = { "dmesg", NULL };
        execvp(av[0], av);
        _exit(127);
    }
    close(fd[1]);

    FILE *f = fdopen(fd[0], "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (!strstr(line, "irmware")) continue;
            if (!strstr(line, "failed") && !strstr(line, "Failed") &&
                !strstr(line, "missing")) continue;
            /* 行頭のドライバ名 ("iwlwifi 0000:03:00.0: ...") と、
             * ファイル名の頭 ("iwlwifi-cc-a0-72.ucode") の両方を拾う */
            char w[40];
            int k = 0;
            for (const char *s = line; *s && k < (int)sizeof(w) - 1; s++) {
                if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
                    *s == '_') {
                    w[k++] = *s;
                } else {
                    w[k] = 0;
                    if (k >= 4) note_fw_word(w);
                    k = 0;
                }
            }
        }
        fclose(f);
    }
    int st;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) { }
}

static int fw_looks_missing(const char *driver)
{
    if (!driver[0]) return 0;
    for (int i = 0; i < n_fw_bad; i++)
        if (!strcmp(fw_bad[i], driver)) return 1;
    return 0;
}

/* --- 使う側から見える名前を貼る ------------------------------------------
 * /sys/class/net/eth0/device が機器を指しているので、そこから逆に
 * 「この PCI 機器は eth0 だ」と分かる。
 *
 * ただし device の指す先が PCI 機器そのものとは限らない。
 * virtio なら .../0000:00:04.0/virtio1 のように 1 段下、
 * USB の LAN アダプタなら .../1-2/1-2:1.0 のようにもっと下を指す。
 * なので realpath まで開いてから、経路の中に自分の場所が出てくる機器を探す。
 * 経路の深いところで当たったものを採る (親のブリッジに取られないように)。 */

/* path の中に「/name」という区切りのそろった部分があるか。
 * 見つかった位置を返す (無ければ -1)。"1-2" が "1-2:1.0" に
 * 引っかからないよう、後ろは / か : か終端に限る。 */
static int path_has(const char *path, const char *name)
{
    size_t ln = strlen(name);
    int best = -1;
    for (const char *s = path; (s = strstr(s, name)); s += 1) {
        if (s == path || s[-1] != '/') continue;
        char c = s[ln];
        if (c && c != '/' && c != ':') continue;
        best = (int)(s - path);
    }
    return best;
}

/* realpath の指す機器に一番近い Dev を返す。無ければ -1。 */
/* realpath の受け皿は PATH_MAX 無いと駄目。
 * 短い配列を渡すと glibc の _FORTIFY_SOURCE が
 * "buffer overflow detected" で落とす (実際に踏んだ)。 */
static int dev_for_path(const char *real)
{
    int best = -1, best_at = -1;
    for (int i = 0; i < n_devs; i++) {
        int at = path_has(real, devs[i].addr);
        if (at > best_at) { best_at = at; best = i; }
    }
    return best_at < 0 ? -1 : best;
}

/* 名前が「cardN」ちょうどか。
 * /sys/class/drm には card0 の他に、つなぎ口ごとの card0-Unknown-1 や
 * card0-HDMI-A-1、それに renderD128 も並んでいる。どれも device が
 * 同じ機器を指すので、除けないと画面に card0-Unknown-1 と出てしまう。
 * (実際に VirtualBox でそう出た) */
static int is_drm_card(const char *name)
{
    if (strncmp(name, "card", 4)) return 0;
    for (const char *s = name + 4; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return name[4] != 0;
}

static void tag_class(const char *dir, int only_drm_card)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (only_drm_card && !is_drm_card(e->d_name)) continue;
        char p[512], real[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s/device", dir, e->d_name);
        if (!realpath(p, real)) continue;
        int i = dev_for_path(real);
        if (i >= 0 && !devs[i].node[0])
            snprintf(devs[i].node, sizeof(devs[i].node), "%s", e->d_name);
    }
    closedir(d);
}

/* 起動時の画面を持っているもの (simpledrm など)。
 *
 * myOS は VirtualBox で画面が真っ黒になるのを避けるため vmwgfx を
 * 止めてある。すると VGA の機器にはドライバが当たらず、代わりに
 * simpledrm がブートローダーの用意した画面をそのまま使い続ける。
 * これを知らないと「画面は映っているのにドライバが無い」と出て、
 * 壊れているように見える。Properties でそう言えるように控えておく。 */
static char boot_fb[48];

static void find_boot_fb(void)
{
    boot_fb[0] = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_drm_card(e->d_name)) continue;
        char p[512], drv[48], real[PATH_MAX];
        snprintf(p, sizeof(p), "/sys/class/drm/%s/device/driver", e->d_name);
        if (!link_base(p, drv, sizeof(drv))) continue;
        /* PCI の機器に当たっているものは本物のドライバ。ここでは要らない */
        snprintf(p, sizeof(p), "/sys/class/drm/%s/device", e->d_name);
        if (realpath(p, real) && dev_for_path(real) >= 0) continue;
        snprintf(boot_fb, sizeof(boot_fb), "%s", drv);
        break;
    }
    closedir(d);
}

static void tag_sound_names(void)
{
    /* /proc/asound/cards:
     *  0 [PCH  ]: HDA-Intel - HDA Intel PCH
     *                         HDA Intel PCH at 0xf7f10000 irq 33      */
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int idx = -1;
        if (sscanf(line, " %d [", &idx) != 1 || idx < 0) continue;
        const char *dash = strstr(line, "]: ");
        if (!dash) continue;
        char nice[96];
        snprintf(nice, sizeof(nice), "%s", dash + 3);
        char *nl = strchr(nice, '\n');
        if (nl) *nl = 0;
        char p[128], real[PATH_MAX];
        snprintf(p, sizeof(p), "/sys/class/sound/card%d/device", idx);
        if (!realpath(p, real)) continue;
        int i = dev_for_path(real);
        if (i >= 0) snprintf(devs[i].extra, sizeof(devs[i].extra), "%s", nice);
    }
    fclose(f);
}

/* --- PCI を数える --------------------------------------------------------- */
static void scan_pci(void)
{
    const char *root = "/sys/bus/pci/devices";
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && n_devs < MAX_DEV) {
        if (e->d_name[0] == '.') continue;
        Dev *v = &devs[n_devs];
        memset(v, 0, sizeof(*v));
        snprintf(v->bus, sizeof(v->bus), "PCI");
        snprintf(v->addr, sizeof(v->addr), "%s", e->d_name);

        char p[512];
        snprintf(p, sizeof(p), "%s/%s/class", root, e->d_name);
        unsigned long cls = rdhex(p);
        snprintf(p, sizeof(p), "%s/%s/vendor", root, e->d_name);
        unsigned long ven = rdhex(p);
        snprintf(p, sizeof(p), "%s/%s/device", root, e->d_name);
        unsigned long dev = rdhex(p);
        snprintf(p, sizeof(p), "%s/%s/irq", root, e->d_name);
        v->irq = rdint(p);
        snprintf(p, sizeof(p), "%s/%s/driver", root, e->d_name);
        link_base(p, v->driver, sizeof(v->driver));

        snprintf(v->id, sizeof(v->id), "%04lx:%04lx", ven, dev);

        char cname[64];
        pci_class_name(cls, cname, sizeof(cname), &v->cat);
        const char *vn = vendor_name((unsigned)ven);
        if (vn) snprintf(v->name, sizeof(v->name), "%s %s", vn, cname);
        else    snprintf(v->name, sizeof(v->name), "%s (%04lx)", cname, ven);

        n_devs++;
    }
    closedir(d);
}

/* --- USB を数える ---------------------------------------------------------
 * /sys/bus/usb/devices には機器 ("1-2") とインターフェース ("1-2:1.0") が
 * 混ざって並んでいる。名前に ':' があるほうがインターフェース。
 * ドライバはインターフェース側に当たるので、機器の下を覗きに行く。 */
static void scan_usb(void)
{
    const char *root = "/sys/bus/usb/devices";
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && n_devs < MAX_DEV) {
        if (e->d_name[0] == '.') continue;
        if (strchr(e->d_name, ':')) continue;      /* インターフェース */

        char p[512], b[128];
        snprintf(p, sizeof(p), "%s/%s/idVendor", root, e->d_name);
        if (!rd1(p, b, sizeof(b))) continue;       /* 機器ではない */
        unsigned long ven = strtoul(b, NULL, 16);
        snprintf(p, sizeof(p), "%s/%s/idProduct", root, e->d_name);
        rd1(p, b, sizeof(b));
        unsigned long pid = strtoul(b, NULL, 16);

        Dev *v = &devs[n_devs];
        memset(v, 0, sizeof(*v));
        snprintf(v->bus, sizeof(v->bus), "USB");
        snprintf(v->addr, sizeof(v->addr), "%s", e->d_name);
        snprintf(v->id, sizeof(v->id), "%04lx:%04lx", ven, pid);
        v->irq = -1;

        /* USB の機器は自分で名前を名乗る。あればそれを使う。 */
        char prod[96] = "", manu[64] = "";
        snprintf(p, sizeof(p), "%s/%s/product", root, e->d_name);
        rd1(p, prod, sizeof(prod));
        snprintf(p, sizeof(p), "%s/%s/manufacturer", root, e->d_name);
        rd1(p, manu, sizeof(manu));

        /* インターフェースを見て、種類とドライバを決める */
        unsigned icls = 0;
        char idir[512];
        snprintf(idir, sizeof(idir), "%s/%s", root, e->d_name);
        DIR *sd = opendir(idir);
        if (sd) {
            struct dirent *se;
            while ((se = readdir(sd))) {
                /* 機器のディレクトリの下にあって ':' を含むものが
                 * インターフェース。名前で前方一致は取らない。
                 * 根元のハブ (usb1) のインターフェースは "1-0:1.0" で、
                 * 機器名で始まらないため、前方一致で見ていたときは
                 * ドライバが見つからず「!」が付いていた。 */
                if (!strchr(se->d_name, ':')) continue;
                snprintf(p, sizeof(p), "%s/%s/bInterfaceClass",
                         idir, se->d_name);
                if (rd1(p, b, sizeof(b)) && !icls)
                    icls = (unsigned)strtoul(b, NULL, 16);
                if (!v->driver[0]) {
                    snprintf(p, sizeof(p), "%s/%s/driver", idir, se->d_name);
                    link_base(p, v->driver, sizeof(v->driver));
                }
            }
            closedir(sd);
        }
        v->cat = usb_cat(icls);

        /* 根元のハブ (usb1, usb2 ...) は自分の名前として
         * "Linux 6.12.9 ehci_hcd EHCI Host Controller" のような
         * 長いものを名乗る。Windows と同じ呼び方に直す。 */
        if (!strncmp(e->d_name, "usb", 3))
            snprintf(v->name, sizeof(v->name), "USB Root Hub");
        else if (prod[0] && manu[0])
            snprintf(v->name, sizeof(v->name), "%s %s", manu, prod);
        else if (prod[0])
            snprintf(v->name, sizeof(v->name), "%s", prod);
        else {
            const char *vn = vendor_name((unsigned)ven);
            snprintf(v->name, sizeof(v->name), "%s USB Device",
                     vn ? vn : "Unknown");
        }
        n_devs++;
    }
    closedir(d);
}

/* --- 行を組み立てる ------------------------------------------------------- */
static void build_rows(void)
{
    n_rows = 0;
    for (int c = 0; c < CAT_N; c++) {
        int any = 0;
        for (int i = 0; i < n_devs; i++) if (devs[i].cat == c) { any = 1; break; }
        if (!any) continue;
        if (n_rows >= MAX_ROW) break;
        rows[n_rows].cat = c; rows[n_rows].dev = -1; n_rows++;
        if (!cat_open[c]) continue;
        for (int i = 0; i < n_devs && n_rows < MAX_ROW; i++)
            if (devs[i].cat == c) {
                rows[n_rows].cat = c; rows[n_rows].dev = i; n_rows++;
            }
    }
    if (sel >= n_rows) sel = n_rows ? n_rows - 1 : 0;
}

static void rescan(void)
{
    n_devs = 0; n_fw_bad = 0;
    scan_dmesg();
    scan_pci();
    scan_usb();
    tag_class("/sys/class/net", 0);
    tag_class("/sys/class/drm", 1);
    find_boot_fb();
    tag_sound_names();
    for (int i = 0; i < n_devs; i++)
        devs[i].fw_missing = fw_looks_missing(devs[i].driver);
    build_rows();
}

/* --- 描画 ----------------------------------------------------------------- */


/* --- 縦スクロールバー (Windows 98 のあれ) -------------------------------
 * 一覧に入り切らないぶんがあることを見せるために要る。
 * 選択を動かせば勝手にずれるが、それだと「まだ下にある」ことが分からない。
 * つまみを引っ張るのは作らない。矢印と、つまみの上下を突いての 1 画面送り、
 * それとホイールがあれば足りる。 */
#define SB_W 16

static void sb_arrow(int px, int py, int down)
{
    for (int i = 0; i < 4; i++) {
        int w = 1 + i * 2;
        int y = down ? py + 5 - i : py + i;
        x98_hline(&x98, win, px + 8 - w / 2 - 1, y, w, x98.text);
    }
}

static void draw_scrollbar(int n, int vis, int topv)
{
    if (n <= vis) return;
    int x = LIST_X + LIST_W - 2 - SB_W;
    int y = LIST_Y + 2;
    int h = LIST_H - 4;

    x98_fill(&x98, win, x, y, SB_W, h, x98_rgb24(&x98, 0xC0C0C0));
    x98_button(&x98, win, x, y, SB_W, SB_W, "", 0);
    sb_arrow(x + 4, y + 6, 0);
    x98_button(&x98, win, x, y + h - SB_W, SB_W, SB_W, "", 0);
    sb_arrow(x + 4, y + h - SB_W + 6, 1);

    int track = h - SB_W * 2;
    int th = track * vis / n;
    if (th < 12) th = 12;
    if (th > track) th = track;
    int maxtop = n - vis;
    int ty = y + SB_W + (maxtop > 0 ? (track - th) * topv / maxtop : 0);
    x98_button(&x98, win, x, ty, SB_W, th, "", 0);
}

/* スクロールバーが突かれたか。突かれていれば top を動かして 1 を返す。 */
static int sb_click(int mx, int my, int n, int vis, int *topv)
{
    if (n <= vis) return 0;
    int x = LIST_X + LIST_W - 2 - SB_W;
    if (mx < x || mx >= x + SB_W) return 0;
    int y = LIST_Y + 2, h = LIST_H - 4;
    if (my < y || my >= y + h) return 0;

    int track = h - SB_W * 2;
    int th = track * vis / n;
    if (th < 12) th = 12;
    if (th > track) th = track;
    int maxtop = n - vis;
    int ty = y + SB_W + (maxtop > 0 ? (track - th) * (*topv) / maxtop : 0);

    if (my < y + SB_W)            (*topv)--;
    else if (my >= y + h - SB_W)  (*topv)++;
    else if (my < ty)             (*topv) -= vis;
    else if (my >= ty + th)       (*topv) += vis;

    if (*topv > maxtop) *topv = maxtop;
    if (*topv < 0) *topv = 0;
    return 1;
}

/* ツリーの ± 箱。Windows 98 のあの小さいやつ。 */
static void draw_pm(int px, int py, int open)
{
    x98_fill(&x98, win, px, py, 9, 9, x98.white);
    x98_hline(&x98, win, px, py, 9, x98.shadow);
    x98_hline(&x98, win, px, py + 8, 9, x98.shadow);
    x98_vline(&x98, win, px, py, 9, x98.shadow);
    x98_vline(&x98, win, px + 8, py, 9, x98.shadow);
    x98_hline(&x98, win, px + 2, py + 4, 5, x98.text);
    if (!open) x98_vline(&x98, win, px + 4, py + 2, 5, x98.text);
}

/* ドライバが無い機器に付ける黄色い「!」。 */
static void draw_bang(int px, int py)
{
    x98_fill(&x98, win, px, py, 11, 11, x98_rgb24(&x98, 0xFFD000));
    x98_hline(&x98, win, px, py, 11, x98.text);
    x98_hline(&x98, win, px, py + 10, 11, x98.text);
    x98_vline(&x98, win, px, py, 11, x98.text);
    x98_vline(&x98, win, px + 10, py, 11, x98.text);
    x98_vline(&x98, win, px + 5, py + 2, 5, x98.text);
    x98_fill(&x98, win, px + 5, py + 8, 1, 1, x98.text);
}

static void dev_line(const Dev *v, char *out, size_t n)
{
    if (v->extra[0] && v->node[0])
        snprintf(out, n, "%s  (%s)", v->extra, v->node);
    else if (v->extra[0])
        snprintf(out, n, "%s", v->extra);
    else if (v->node[0])
        snprintf(out, n, "%s  (%s)", v->name, v->node);
    else
        snprintf(out, n, "%s", v->name);
}

static void draw_list(void)
{
    x98_bevel(&x98, win, LIST_X, LIST_Y, LIST_W, LIST_H, 0);
    x98_fill(&x98, win, LIST_X + 2, LIST_Y + 2, LIST_W - 4, LIST_H - 4,
             x98.white);

    int vis = (LIST_H - 4) / ROW_H;
    draw_scrollbar(n_rows, vis, top);
    for (int r = 0; r < vis && top + r < n_rows; r++) {
        Row *w = &rows[top + r];
        int y = LIST_Y + 2 + r * ROW_H;
        int on = (top + r == sel);
        unsigned long fg = x98.text;
        if (on) {
            x98_fill(&x98, win, LIST_X + 2, y,
                     LIST_W - 4 - (n_rows > vis ? SB_W : 0), ROW_H,
                     x98.select_bg);
            fg = x98.white;
        }
        int ty = y + (ROW_H - x98_text_h(&x98)) / 2;
        if (w->dev < 0) {
            draw_pm(LIST_X + 8, y + (ROW_H - 9) / 2, cat_open[w->cat]);
            x98_text(&x98, win, LIST_X + 24, ty, cat_name[w->cat], fg);
        } else {
            const Dev *v = &devs[w->dev];
            char line[192];
            dev_line(v, line, sizeof(line));
            int tx = LIST_X + 40;
            if (!v->driver[0] || v->fw_missing) {
                draw_bang(LIST_X + 26, y + (ROW_H - 11) / 2);
                tx = LIST_X + 42;
            }
            x98_text(&x98, win, tx, ty, line, fg);
        }
    }
}

static void props_lines(const Dev *v, char out[][96], int *n)
{
    int k = 0;
    snprintf(out[k++], 96, "%s", v->name);
    if (v->extra[0]) snprintf(out[k++], 96, "%s", v->extra);
    snprintf(out[k++], 96, "Bus          %s   %s", v->bus, v->addr);
    snprintf(out[k++], 96, "Hardware ID  %s", v->id);
    snprintf(out[k++], 96, "Driver       %s",
             v->driver[0] ? v->driver : "(none)");
    if (v->node[0]) snprintf(out[k++], 96, "System name  %s", v->node);
    if (v->irq >= 0) snprintf(out[k++], 96, "Interrupt    %d", v->irq);
    *n = k;
}

static const char *dev_status(const Dev *v)
{
    if (!v->driver[0]) {
        if (v->cat == CAT_DISPLAY && boot_fb[0])
            return "No driver is loaded, but the screen is being drawn "
                   "by the boot framebuffer.";
        return "No driver is loaded for this device.";
    }
    if (v->fw_missing)
        return "The driver is loaded but its firmware is missing.";
    return "This device is working properly.";
}

/* 画面の板で、しかも NVIDIA か。
 *
 * ここだけ「ドライバの更新」を出す。Intel の i915 と AMD の amdgpu は
 * メーカー自身の公式実装なので、入れ替える相手が無い。
 * 出しても押せる先が無いボタンを並べるほうが不親切。 */
static int is_nvidia_gpu(const Dev *v)
{
    return v->cat == CAT_DISPLAY &&
           strncmp(v->id, "10de:", 5) == 0;
}

static void draw_props(void)
{
    const Dev *v = &devs[rows[sel].dev];
    int w = 400, h = 240;
    int px = (WIN_W - w) / 2, py = (WIN_H - h) / 2;

    x98_fill(&x98, win, px, py, w, h, x98.face);
    x98_bevel(&x98, win, px, py, w, h, 1);
    x98_titlebar(&x98, win, px + 3, py + 3, w - 6, 18, "Properties");

    char lines[8][96];
    int n = 0;
    props_lines(v, lines, &n);
    for (int i = 0; i < n; i++)
        x98_text(&x98, win, px + 14, py + 32 + i * 18, lines[i],
                 i == 0 ? x98.text : x98.shadow);

    /* 状態は枠で囲って目立たせる。ここが見たくて開くので。 */
    int sy = py + 32 + n * 18 + 8;
    x98_bevel(&x98, win, px + 12, sy, w - 24, 44, 0);
    x98_fill(&x98, win, px + 14, sy + 2, w - 28, 40, x98.white);
    x98_text(&x98, win, px + 20, sy + 8, "Device status:", x98.text);
    x98_text(&x98, win, px + 20, sy + 24, dev_status(v), x98.text);

    x98_button(&x98, win, px + w - BTN_W - 14, py + h - BTN_H - 12,
               BTN_W, BTN_H, "OK", 0);
    if (is_nvidia_gpu(v))
        x98_button(&x98, win, px + 14, py + h - BTN_H - 12,
                   BTN_W + 48, BTN_H, "Update Driver...", 0);
}

static void redraw(void)
{
    x98_fill(&x98, win, 0, 0, WIN_W, WIN_H, x98.face);
    x98_text(&x98, win, PAD, 12, "Devices on this computer", x98.text);

    char sum[128];
    int bad = 0;
    for (int i = 0; i < n_devs; i++)
        if (!devs[i].driver[0] || devs[i].fw_missing) bad++;
    snprintf(sum, sizeof(sum),
             "%d device%s found, %d without a driver",
             n_devs, n_devs == 1 ? "" : "s", bad);
    x98_text(&x98, win, PAD, 28, sum, x98.shadow);

    draw_list();

    x98_text(&x98, win, PAD, LIST_Y + LIST_H + 8,
             "Up/Down = move   Enter = open/close or properties   R = refresh",
             x98.shadow);

    int by = WIN_H - BTN_H - PAD;
    x98_button(&x98, win, WIN_W - 3 * BTN_W - 26, by, BTN_W, BTN_H,
               "Properties", 0);
    x98_button(&x98, win, WIN_W - 2 * BTN_W - 19, by, BTN_W, BTN_H,
               "Refresh", 0);
    x98_button(&x98, win, WIN_W - BTN_W - 12, by, BTN_W, BTN_H, "Close", 0);

    if (showing_props) draw_props();
}

static void scroll_into_view(void)
{
    int vis = (LIST_H - 4) / ROW_H;
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;
}

static void activate(void)
{
    if (sel < 0 || sel >= n_rows) return;
    if (rows[sel].dev < 0) {
        int c = rows[sel].cat;
        cat_open[c] = !cat_open[c];
        build_rows();
        /* 開いた見出しがそのまま選ばれたままになるように、行を探し直す */
        for (int i = 0; i < n_rows; i++)
            if (rows[i].cat == c && rows[i].dev < 0) { sel = i; break; }
        scroll_into_view();
    } else {
        showing_props = 1;
    }
}

int main(void)
{
    x98_im_setup_locale();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "myos-devmgr: no display\n"); return 1; }
    screen = DefaultScreen(dpy);
    x98_init(&x98, dpy, screen);
    x98_im_open(&x98);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WIN_W, WIN_H, 0, 0, x98.face);
    XStoreName(dpy, win, "Device Manager");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &del, 1);
    XMapWindow(dpy, win);

    ic = x98_ic_new(&x98, win);
    x98_ic_focus(ic);

    /* 見たいのはたいてい下の 4 つなので、最初から開けておく。
     * ブリッジだらけの System devices は畳んだまま。 */
    cat_open[CAT_DISPLAY] = cat_open[CAT_SOUND] = 1;
    cat_open[CAT_NET] = cat_open[CAT_DISK] = 1;
    rescan();

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;

        switch (ev.type) {
        case Expose:
            redraw();
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == del) goto done;
            break;

        case KeyPress: {
            char buf[32];
            KeySym ks;
            int n = x98_lookup(ic, &ev.xkey, buf, sizeof(buf), &ks);

            if (showing_props) {
                if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter)
                    showing_props = 0;
                redraw();
                break;
            }

            if (ks == XK_Escape) goto done;
            else if (ks == XK_Up   && sel > 0) sel--;
            else if (ks == XK_Down && sel < n_rows - 1) sel++;
            else if (ks == XK_Prior) sel -= (LIST_H - 4) / ROW_H;
            else if (ks == XK_Next)  sel += (LIST_H - 4) / ROW_H;
            else if (ks == XK_Home) sel = 0;
            else if (ks == XK_End)  sel = n_rows - 1;
            else if (ks == XK_Left) {
                if (rows[sel].dev < 0) cat_open[rows[sel].cat] = 0;
                build_rows();
            } else if (ks == XK_Right) {
                if (rows[sel].dev < 0) cat_open[rows[sel].cat] = 1;
                build_rows();
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                activate();
            } else if (ks == XK_F5 ||
                       (n > 0 && (buf[0] == 'r' || buf[0] == 'R'))) {
                rescan();
            }
            if (sel < 0) sel = 0;
            if (sel >= n_rows) sel = n_rows ? n_rows - 1 : 0;
            scroll_into_view();
            redraw();
            break;
        }

        case ButtonPress: {
            int mx = ev.xbutton.x, my = ev.xbutton.y;

            if (showing_props) {
                /* 「ドライバの更新」だけは、閉じる前に拾う。
                 * 座標は draw_props と同じ積み方をすること。 */
                const Dev *v = &devs[rows[sel].dev];
                int w = 400, h = 240;
                int px = (WIN_W - w) / 2, py = (WIN_H - h) / 2;
                int uy = py + h - BTN_H - 12;
                if (is_nvidia_gpu(v) &&
                    my >= uy && my < uy + BTN_H &&
                    mx >= px + 14 && mx < px + 14 + BTN_W + 48) {
                    /* 端末の中で聞きながら進める。ドライバ本体は
                     * 使う人が用意するので、窓を作るより文字のほうが
                     * 案内しやすい (95 の「ディスク使用...」と同じ)。 */
                    pid_t p = fork();
                    if (p == 0) {
                        execlp("myos-term", "myos-term",
                               "/usr/local/bin/myos-nvidia", "wizard",
                               (char *)NULL);
                        _exit(127);
                    }
                }
                /* OK 以外を押しても閉じてよい。困る操作が無いので。 */
                showing_props = 0;
                redraw();
                break;
            }

            int by = WIN_H - BTN_H - PAD;
            if (my >= by && my < by + BTN_H) {
                if (mx >= WIN_W - 3 * BTN_W - 26 &&
                    mx < WIN_W - 2 * BTN_W - 26) {
                    if (sel < n_rows && rows[sel].dev >= 0) showing_props = 1;
                } else if (mx >= WIN_W - 2 * BTN_W - 19 &&
                           mx < WIN_W - BTN_W - 19) {
                    rescan();
                } else if (mx >= WIN_W - BTN_W - 12) {
                    goto done;
                }
            } else if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                /* ホイール。3 行ずつ。 */
                int vis = (LIST_H - 4) / ROW_H;
                top += (ev.xbutton.button == 4) ? -3 : 3;
                if (top > n_rows - vis) top = n_rows - vis;
                if (top < 0) top = 0;
            } else if (sb_click(mx, my, n_rows, (LIST_H - 4) / ROW_H, &top)) {
                /* スクロールバー */
            } else if (mx >= LIST_X && mx < LIST_X + LIST_W &&
                       my >= LIST_Y + 2 && my < LIST_Y + LIST_H - 2) {
                int i = top + (my - LIST_Y - 2) / ROW_H;
                if (i >= 0 && i < n_rows) {
                    sel = i;
                    /* ± 箱を突いたときだけ開閉。名前の上は選ぶだけ。
                     * 二度押しで詳細、というのは Windows 98 と同じ。 */
                    if (rows[i].dev < 0 && mx < LIST_X + 22) activate();
                    else if (ev.xbutton.button == 1 &&
                             rows[i].dev < 0 && mx < LIST_X + 24) activate();
                }
            }
            redraw();
            break;
        }
        }
    }

done:
    XCloseDisplay(dpy);
    return 0;
}

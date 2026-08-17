/* ==========================================================================
 * init.c  -  myOS initramfs 用の最小 init (PID 1)
 *
 * 「自作ブートローダーから Linux カーネルが起動し、ユーザーランドまで
 *  到達した」ことを確認するためだけの、最小限のプログラム。
 *
 * libc を一切使わず、syscall 命令を直接叩いている。
 *   - 動的リンカーも共有ライブラリも使わない (この時点ではまだ何も無い)
 *   - _start がエントリポイント。return してはいけない (PID 1 が死ぬと panic)
 *
 * ビルド:
 *   gcc -static -nostdlib -nostartfiles -ffreestanding -Os -o init init.c
 *
 * 将来的な方針 (引き継ぎ資料 7章):
 *   実行ファイル形式は標準 ELF、動的リンカーと共有ライブラリは既存の
 *   glibc / musl を流用する。なのでこの init も「とりあえず動かすための
 *   足場」であって、本番では busybox などをそのまま置く想定。
 * ========================================================================== */

/* --- x86-64 の syscall 呼び出し規約 --------------------------------------
 * 番号は RAX、引数は RDI, RSI, RDX, R10, R8, R9。
 * syscall 命令は RCX と R11 を壊すので clobber に入れる。
 */
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_nanosleep 35

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static unsigned long slen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static void put(int fd, const char *s)
{
    sys3(SYS_write, fd, (long)s, (long)slen(s));
}

void _start(void)
{
    static const char banner[] =
        "\n"
        "================================================================\n"
        "  myOS: userland reached.\n"
        "\n"
        "  Booted by a hand-written bootloader:\n"
        "    stage1 (MBR, 512 bytes)\n"
        "      -> stage2_linux (Linux 32-bit boot protocol)\n"
        "        -> Linux kernel\n"
        "          -> this /init (PID 1, no libc)\n"
        "\n"
        "  Phase 2-B complete.\n"
        "================================================================\n"
        "\n";

    /* カーネルは /dev/console を fd 0/1/2 として開いてくれるが、
     * 開けなかった場合に備えて自分でも開きにいく。 */
    put(1, banner);
    put(2, banner);

    int fd = (int)sys3(SYS_open, (long)"/dev/console", 1 /* O_WRONLY */, 0);
    if (fd >= 0) {
        put(fd, banner);
        sys3(SYS_close, fd, 0, 0);
    }

    /* PID 1 は絶対に終了してはいけない (終了するとカーネルパニックになる) */
    for (;;) {
        struct timespec ts = { 3600, 0 };
        sys3(SYS_nanosleep, (long)&ts, 0, 0);
    }
}

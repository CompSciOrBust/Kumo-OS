/*
 * fatimg: work on a FAT or exFAT volume inside a disk image file.
 *
 * It links the same vendored FatFs as the FS system module, so a volume
 * formatted or filled here is laid out exactly as the module would do it.
 * scripts/hzdisk.py drives it and supplies the partition's offset and size.
 *
 *   fatimg <image> <offset> <size> <sector-size> format [fat32]
 *   fatimg <image> <offset> <size> <sector-size> ls [<path>]
 *   fatimg <image> <offset> <size> <sector-size> mkdir <path>
 *   fatimg <image> <offset> <size> <sector-size> put <host-path> <path>
 *   fatimg <image> <offset> <size> <sector-size> get <path> <host-file>
 *
 * Offsets and sizes are in bytes. Paths inside the volume start with '/'.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ff.h"
#include "diskio.h"
#include "hostdir.h"

static int       g_fd = -1;
static long long g_offset;
static long long g_size;
static unsigned  g_sector_size;

/* ===== FatFs disk interface: drive 0 is the byte range of the image ===== */

DSTATUS disk_initialize(BYTE pdrv) { return pdrv == 0 ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE pdrv)     { return pdrv == 0 ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    const long long pos = (long long)sector * g_sector_size;
    const long long len = (long long)count * g_sector_size;
    if (pdrv != 0 || pos + len > g_size) return RES_PARERR;
    return pread(g_fd, buff, (size_t)len, g_offset + pos) == len ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    const long long pos = (long long)sector * g_sector_size;
    const long long len = (long long)count * g_sector_size;
    if (pdrv != 0 || pos + len > g_size) return RES_PARERR;
    return pwrite(g_fd, buff, (size_t)len, g_offset + pos) == len ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:        return fsync(g_fd) == 0 ? RES_OK : RES_ERROR;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)(g_size / g_sector_size); return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff  = (WORD)g_sector_size;            return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1;                              return RES_OK;
        default:               return RES_PARERR;
    }
}

/* ===== Helpers ===== */

static void die(const char *what, FRESULT fr) {
    fprintf(stderr, "fatimg: %s failed (FatFs error %d)\n", what, (int)fr);
    exit(1);
}

static void check(const char *what, FRESULT fr) {
    if (fr != FR_OK) die(what, fr);
}

/* "0:" + path, in a fresh buffer. */
static char *volume_path(const char *path) {
    if (path[0] != '/') {
        fprintf(stderr, "fatimg: path '%s' must start with '/'\n", path);
        exit(1);
    }
    char *out = malloc(strlen(path) + 3);
    sprintf(out, "0:%s", path);
    return out;
}

static char *join(const char *dir, const char *name) {
    const size_t n = strlen(dir);
    char *out = malloc(n + strlen(name) + 2);
    sprintf(out, "%s%s%s", dir, (n > 0 && dir[n - 1] == '/') ? "" : "/", name);
    return out;
}

static void list_tree(const char *vpath) {
    DIR dir;
    check("opendir", f_opendir(&dir, vpath));
    for (;;) {
        FILINFO info;
        check("readdir", f_readdir(&dir, &info));
        if (info.fname[0] == '\0') break;

        char *child = join(vpath, info.fname);
        if (info.fattrib & AM_DIR) {
            printf("D %12s %s\n", "", child + 2);
            list_tree(child);
        } else {
            printf("F %12llu %s\n", (unsigned long long)info.fsize, child + 2);
        }
        free(child);
    }
    f_closedir(&dir);
}

static void put_file(const char *host, const char *vpath) {
    FILE *in = fopen(host, "rb");
    if (!in) { fprintf(stderr, "fatimg: cannot open '%s': %s\n", host, strerror(errno)); exit(1); }

    FIL out;
    check("create file", f_open(&out, vpath, FA_CREATE_ALWAYS | FA_WRITE));

    static char buffer[1 << 20];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        UINT written = 0;
        check("write", f_write(&out, buffer, (UINT)n, &written));
        if (written != n) { fprintf(stderr, "fatimg: volume is full\n"); exit(1); }
    }
    check("close", f_close(&out));
    fclose(in);
}

static void put_tree(const char *host, const char *vpath);

struct put_context {
    const char *host;
    const char *vpath;
};

static void put_child(const char *name, void *context) {
    const struct put_context *ctx = context;

    /* Finder's metadata files are never wanted on the volume. */
    if (strcmp(name, ".DS_Store") == 0) return;

    char *host_child = join(ctx->host, name);
    char *vol_child  = join(ctx->vpath, name);
    put_tree(host_child, vol_child);
    free(host_child);
    free(vol_child);
}

static void put_tree(const char *host, const char *vpath) {
    struct stat st;
    if (stat(host, &st) != 0) { fprintf(stderr, "fatimg: cannot stat '%s': %s\n", host, strerror(errno)); exit(1); }

    if (!S_ISDIR(st.st_mode)) {
        put_file(host, vpath);
        return;
    }

    /* The volume's root always exists; any other directory is created if missing. */
    if (strcmp(vpath, "0:/") != 0) {
        const FRESULT fr = f_mkdir(vpath);
        if (fr != FR_OK && fr != FR_EXIST) die("mkdir", fr);
    }

    struct put_context ctx = { host, vpath };
    if (host_list_dir(host, put_child, &ctx) != 0) {
        fprintf(stderr, "fatimg: cannot read '%s': %s\n", host, strerror(errno));
        exit(1);
    }
}

static void get_file(const char *vpath, const char *host) {
    FIL in;
    check("open file", f_open(&in, vpath, FA_READ));

    FILE *out = fopen(host, "wb");
    if (!out) { fprintf(stderr, "fatimg: cannot create '%s': %s\n", host, strerror(errno)); exit(1); }

    static char buffer[1 << 20];
    for (;;) {
        UINT n = 0;
        check("read", f_read(&in, buffer, sizeof(buffer), &n));
        if (n == 0) break;
        fwrite(buffer, 1, n, out);
    }
    fclose(out);
    f_close(&in);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: fatimg <image> <offset> <size> <sector-size> format|ls|mkdir|put|get [args]\n");
        return 2;
    }

    g_offset      = strtoll(argv[2], NULL, 0);
    g_size        = strtoll(argv[3], NULL, 0);
    g_sector_size = (unsigned)strtoul(argv[4], NULL, 0);
    const char *command = argv[5];

    g_fd = open(argv[1], O_RDWR);
    if (g_fd < 0) { fprintf(stderr, "fatimg: cannot open '%s': %s\n", argv[1], strerror(errno)); return 1; }

    static FATFS fatfs;

    if (strcmp(command, "format") == 0) {
        /* The byte range is the whole FatFs drive, so no partition table goes inside it (FM_SFD). */
        static BYTE work[1 << 16];
        /* The type is chosen from the size unless FAT32 is asked for (an EFI system partition should be FAT32). */
        const int fat32 = argc > 6 && strcmp(argv[6], "fat32") == 0;
        const MKFS_PARM parameters = { .fmt = (BYTE)((fat32 ? FM_FAT32 : FM_ANY) | FM_SFD) };
        check("format", f_mkfs("0:", &parameters, work, sizeof(work)));
        return 0;
    }

    check("mount (is the partition formatted?)", f_mount(&fatfs, "0:", 1));

    if (strcmp(command, "ls") == 0) {
        char *vpath = volume_path(argc > 6 ? argv[6] : "/");
        list_tree(vpath);
    } else if (strcmp(command, "mkdir") == 0 && argc == 7) {
        check("mkdir", f_mkdir(volume_path(argv[6])));
    } else if (strcmp(command, "put") == 0 && argc == 8) {
        put_tree(argv[6], volume_path(argv[7]));
    } else if (strcmp(command, "get") == 0 && argc == 8) {
        get_file(volume_path(argv[6]), argv[7]);
    } else {
        fprintf(stderr, "fatimg: bad command or arguments\n");
        return 2;
    }

    f_unmount("0:");
    close(g_fd);
    return 0;
}

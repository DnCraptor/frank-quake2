/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * newlib file syscalls on FatFs, read and write: the engine keeps its stdio
 * (fopen of the paks, config.cfg, save games). Paths are FatFs paths
 * ("/quake2/baseq2/pak0.pak").
 *
 * Read-only files get a FatFs fast-seek cluster map: the paks are read at
 * random offsets, and without one each seek back walks the FAT chain.
 * Reads that land outside SRAM go through an SRAM staging buffer: the SD SPI
 * path never writes PSRAM directly (same rule as frank-castle/frank-fodder).
 * FIL objects (and their sector buffers) stay in SRAM for the same reason.
 *
 * Core 0 only: FatFs is built without locks.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ff.h" /* its directory type is FFDIR: no clash with <dirent.h> */
#include <dirent.h>
#include "pico/stdio.h"
#include "pico/time.h"

#define FIRST_FD   3
#define MAX_FILES  12 /* the 3 paks + their pool of 8 handles + one */
#define CLMT_WORDS 32 /* a contiguous file needs 4; fragmented ones get a heap map */
#define STAGING    4096

typedef struct {
    FIL fil;
    DWORD clmt[CLMT_WORDS];
    DWORD *big_clmt;
    bool used;
} qfile_t;

static qfile_t files[MAX_FILES];
static uint8_t staging[STAGING] __attribute__((aligned(4)));

static inline bool in_sram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x20000000u && a < 0x20082000u;
}

static qfile_t *file_of(int fd) {
    int i = fd - FIRST_FD;
    return (i >= 0 && i < MAX_FILES && files[i].used) ? &files[i] : NULL;
}

static int fr_errno(FRESULT fr) {
    switch (fr) {
    case FR_OK: return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
    case FR_INVALID_NAME: return ENOENT;
    case FR_DENIED:
    case FR_WRITE_PROTECTED: return EACCES;
    case FR_EXIST: return EEXIST;
    case FR_TOO_MANY_OPEN_FILES: return EMFILE;
    case FR_NOT_ENOUGH_CORE: return ENOMEM;
    default: return EIO;
    }
}

/* Fast seek for a file opened read-only; harmless if it fails. */
static void map_clusters(qfile_t *f) {
    f->clmt[0] = CLMT_WORDS;
    f->fil.cltbl = f->clmt;
    FRESULT fr = f_lseek(&f->fil, CREATE_LINKMAP);
    if (fr == FR_NOT_ENOUGH_CORE) {
        DWORD need = f->clmt[0];
        f->big_clmt = malloc(need * sizeof(DWORD));
        if (f->big_clmt) {
            f->big_clmt[0] = need;
            f->fil.cltbl = f->big_clmt;
            fr = f_lseek(&f->fil, CREATE_LINKMAP);
        }
    }
    if (fr != FR_OK) {
        f->fil.cltbl = NULL;
        free(f->big_clmt);
        f->big_clmt = NULL;
    }
}

int _open(const char *fn, int oflag, ...) {
    int i;
    for (i = 0; i < MAX_FILES && files[i].used; i++)
        ;
    if (i == MAX_FILES) {
        errno = EMFILE;
        return -1;
    }
    qfile_t *f = &files[i];
    BYTE mode;
    switch (oflag & O_ACCMODE) {
    case O_RDONLY: mode = FA_READ; break;
    case O_WRONLY: mode = FA_WRITE; break;
    default: mode = FA_READ | FA_WRITE; break;
    }
    if (oflag & O_CREAT) {
        if (oflag & O_EXCL) mode |= FA_CREATE_NEW;
        else if (oflag & O_TRUNC) mode |= FA_CREATE_ALWAYS;
        else mode |= FA_OPEN_ALWAYS;
    } else if (oflag & O_TRUNC) {
        mode |= FA_CREATE_ALWAYS;
    }
    if (oflag & O_APPEND) mode |= FA_OPEN_APPEND;

    memset(f, 0, sizeof(*f));
    FRESULT fr = f_open(&f->fil, fn, mode);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    f->used = true;
    if (mode == FA_READ && f_size(&f->fil) > 64 * 1024) map_clusters(f);
    return FIRST_FD + i;
}

int _close(int fd) {
    qfile_t *f = file_of(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    FRESULT fr = f_close(&f->fil);
    free(f->big_clmt);
    f->big_clmt = NULL;
    f->used = false;
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    return 0;
}

int _read(int fd, char *buf, int len) {
    if (fd == 0) return stdio_get_until(buf, len, at_the_end_of_time);
    qfile_t *f = file_of(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    if (in_sram(buf)) {
        UINT got = 0;
        FRESULT fr = f_read(&f->fil, buf, (UINT)len, &got);
        if (fr != FR_OK) {
            errno = fr_errno(fr);
            return -1;
        }
        return (int)got;
    }
    int done = 0;
    while (done < len) {
        UINT want = (UINT)(len - done) > STAGING ? STAGING : (UINT)(len - done), got = 0;
        FRESULT fr = f_read(&f->fil, staging, want, &got);
        if (fr != FR_OK) {
            if (done) break;
            errno = fr_errno(fr);
            return -1;
        }
        memcpy(buf + done, staging, got);
        done += (int)got;
        if (got < want) break;
    }
    return done;
}

int _write(int fd, char *buf, int len) {
    if (fd == 1 || fd == 2) {
        stdio_put_string(buf, len, false, true);
        return len;
    }
    qfile_t *f = file_of(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    UINT put = 0;
    FRESULT fr = f_write(&f->fil, buf, (UINT)len, &put);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    return (int)put;
}

off_t _lseek(int fd, off_t off, int whence) {
    qfile_t *f = file_of(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f_tell(&f->fil) : (int64_t)f_size(&f->fil);
    int64_t p = base + off;
    if (p < 0) {
        errno = EINVAL;
        return -1;
    }
    /* a fast-seek map cannot extend the file: seeking past the end of a
     * read-only file only has to fail the next read, as it does */
    if (f->fil.cltbl && p > (int64_t)f_size(&f->fil)) p = (int64_t)f_size(&f->fil);
    FRESULT fr = f_lseek(&f->fil, (FSIZE_t)p);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    return (off_t)p;
}

int _fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    if (fd < FIRST_FD) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    qfile_t *f = file_of(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFREG;
    st->st_size = (off_t)f_size(&f->fil);
    st->st_blksize = 512;
    return 0;
}

int _isatty(int fd) {
    return fd >= 0 && fd < FIRST_FD;
}

int _stat(const char *path, struct stat *st) {
    FILINFO fi;
    memset(st, 0, sizeof(*st));
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    st->st_mode = (fi.fattrib & AM_DIR) ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)fi.fsize;
    return 0;
}

int stat(const char *path, struct stat *st) {
    return _stat(path, st);
}

int _unlink(const char *path) {
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    return 0;
}

int _rename(const char *from, const char *to) {
    f_unlink(to); /* POSIX rename replaces the target, FatFs does not */
    FRESULT fr = f_rename(from, to);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    return 0;
}

int rename(const char *from, const char *to) {
    return _rename(from, to);
}

int remove(const char *path) {
    return _unlink(path);
}

int unlink(const char *path) {
    return _unlink(path);
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK) {
        errno = fr_errno(fr);
        return -1;
    }
    return 0;
}

/* ---- directories (Sys_FindFirst: save slots, SV_WipeSavegame) ---- */

struct q2_dir {
    FFDIR dir;
    struct dirent ent;
};

DIR *opendir(const char *path) {
    struct q2_dir *d = malloc(sizeof(*d));
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    FRESULT fr = f_opendir(&d->dir, path);
    if (fr != FR_OK) {
        free(d);
        errno = fr_errno(fr);
        return NULL;
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    FILINFO fi;
    if (f_readdir(&d->dir, &fi) != FR_OK || !fi.fname[0]) return NULL;
    d->ent.d_type = (fi.fattrib & AM_DIR) ? DT_DIR : DT_REG;
    strncpy(d->ent.d_name, fi.fname, sizeof(d->ent.d_name) - 1);
    d->ent.d_name[sizeof(d->ent.d_name) - 1] = 0;
    return &d->ent;
}

int closedir(DIR *d) {
    f_closedir(&d->dir);
    free(d);
    return 0;
}

/* ---- "sdtest": raw read throughput of the SD card (FatFs, into SRAM) ---- */

void q2_sd_bench(void) {
    static FIL f;
    static const UINT sizes[] = {512, 4096, STAGING};
    if (f_open(&f, "/quake2/baseq2/pak0.pak", FA_READ) != FR_OK) {
        printf("sdtest: no pak0.pak\n");
        return;
    }
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        f_lseek(&f, (FSIZE_t)k * 8 * 1024 * 1024);
        uint32_t total = 0, t0 = time_us_32();
        while (total < 1024 * 1024) {
            UINT got = 0;
            if (f_read(&f, staging, sizes[k], &got) != FR_OK || !got) break;
            total += got;
        }
        uint32_t us = time_us_32() - t0;
        printf("sdtest: %u-byte reads: %lu KB in %lu ms, %lu KB/s\n", sizes[k], (unsigned long)(total / 1024),
               (unsigned long)(us / 1000), (unsigned long)((uint64_t)total * 1000000 / 1024 / (us + 1)));
    }
    /* random 4 KB reads, as the paks are read */
    uint32_t t0 = time_us_32();
    for (int i = 0; i < 64; i++) {
        UINT got;
        f_lseek(&f, ((FSIZE_t)i * 2654435761u % (FSIZE_t)(f_size(&f) - 8192)) & ~(FSIZE_t)511);
        f_read(&f, staging, 4096, &got);
    }
    printf("sdtest: 64 random 4 KB reads: %lu us each\n", (unsigned long)((time_us_32() - t0) / 64));
    f_close(&f);
}

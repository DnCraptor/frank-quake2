/*
 * frank-quake2 — a minimal <dirent.h> over FatFs (newlib has none on bare
 * metal); implemented in src/rp2350/q2_fs.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef Q2_COMPAT_DIRENT_H
#define Q2_COMPAT_DIRENT_H

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    unsigned char d_type;
    char d_name[256];
};

typedef struct q2_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif

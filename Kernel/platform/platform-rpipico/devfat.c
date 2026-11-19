#include "fatfs/ff.h"

/*
 * ff.h renames f_read/f_write to fatfs_read/fatfs_write so that they do not
 * clash with the kernel's mangled _read/_write syscalls (see mangle.h).
 * Drop those rename macros before kernel.h is included, otherwise the mangle
 * rewrites the kernel prototypes to the FatFs names as well.  The renamed
 * FatFs entry points are called directly below.
 */
#undef f_read
#undef f_write

#include "fatfs/diskio.h"
#include <kernel.h>
#include <kdata.h>
#include <blkdev.h>
#include <string.h>
#include <printf.h>
#include "devfat.h"

extern void fat_diskio_bind(uint16_t dev);

#define FAT_MAX_INOS 64
#define FAT_PATH_MAX 64

static FATFS fat_volume;
static bool  fat_active = false;
static char  fat_paths[FAT_MAX_INOS][FAT_PATH_MAX];
static uint16_t fat_parents[FAT_MAX_INOS];
/* Scratch buffer shared by the read/write/truncate paths.  The kernel is
 * single threaded, so one buffer is enough and saves 1 KB of precious RAM. */
static uint8_t fat_bounce[512];

static void fat_reset_inos(void) {
    memset(fat_paths, 0, sizeof(fat_paths));
    memset(fat_parents, 0, sizeof(fat_parents));
    strcpy(fat_paths[1], "/");
    fat_parents[1] = 1;
}

/* True when some live inode is still bound to path slot 'slot'.  Such a slot
 * must not be recycled, otherwise its open descriptors would silently start
 * addressing a different file. */
static bool fat_slot_live(uint16_t slot) {
    inoptr j;
    for (j = i_tab; j < i_tab + ITABSIZE; j++) {
        /* Use the same "live" predicate as devfat_i_open()'s reuse guard:
         * c_refs>0 AND valid magic.  Otherwise a slot held by a live inode
         * with a stale c_super/mount bit is seen as free and rebound to a
         * different file, aliasing the still-open descriptor's inode. */
        if (j->c_refs > 0 && j->c_magic == CMAGIC && j->c_num == slot &&
            j->c_super < NMOUNTS &&
            (fs_tab[j->c_super].m_flags & 0x80))
            return true;
    }
    return false;
}

/* Drop the path slots bound to 'path' and detach any inode that referenced
 * them.  Without the inode detach a live descriptor could recreate the
 * deleted name through FA_OPEN_ALWAYS on its next write, or alias a future
 * file that happens to reuse the freed slot. */
static void fat_invalidate_path(const char *path) {
    uint16_t i;
    inoptr j;
    for (i = 1; i < FAT_MAX_INOS; i++) {
        if (!fat_paths[i][0] || strcmp(fat_paths[i], path) != 0)
            continue;
        for (j = i_tab; j < i_tab + ITABSIZE; j++) {
            if (j->c_num == i && j->c_super < NMOUNTS &&
                (fs_tab[j->c_super].m_flags & 0x80))
                j->c_num = 0;
        }
        fat_paths[i][0] = '\0';
        fat_parents[i] = 0;
    }
}

/* Intern a FAT path into a slot.  Returns 0 on failure: a slot referenced by
 * a live inode is never stolen. */
static uint16_t fat_intern_path(const char *path, uint16_t parent_ino) {
    static uint16_t next_evict = 2;
    uint16_t slot;
    uint16_t n;
    bool claimed = false;

    for (slot = 1; slot < FAT_MAX_INOS; slot++) {
        if (fat_paths[slot][0] && strcmp(fat_paths[slot], path) == 0)
            return slot;
    }
    for (slot = 2; slot < FAT_MAX_INOS; slot++) {
        if (fat_paths[slot][0] == '\0') {
            claimed = true;
            break;
        }
    }
    if (!claimed) {
        /* All slots in use: find a non-root slot no live inode references. */
        for (n = 0; n < FAT_MAX_INOS - 2; n++) {
            slot = 2 + (uint16_t)(((uint32_t)next_evict - 2 + n) %
                                  (FAT_MAX_INOS - 2));
            if (!fat_slot_live(slot)) {
                next_evict = (uint16_t)(slot + 1);
                if (next_evict >= FAT_MAX_INOS)
                    next_evict = 2;
                claimed = true;
                break;
            }
        }
    }
    if (!claimed)
        return 0;

    strncpy(fat_paths[slot], path, FAT_PATH_MAX - 1);
    fat_paths[slot][FAT_PATH_MAX - 1] = '\0';
    fat_parents[slot] = parent_ino ? parent_ino : 1;
    return slot;
}

static void fat_join_path(const char *dir, const char *name, char *out) {
    if (strcmp(dir, "/") == 0) {
        out[0] = '/';
        strncpy(out + 1, name, FAT_PATH_MAX - 2);
        out[FAT_PATH_MAX - 1] = '\0';
    } else {
        size_t dlen = strlen(dir);
        if (dlen >= FAT_PATH_MAX - 2)
            dlen = FAT_PATH_MAX - 2;
        memcpy(out, dir, dlen);
        out[dlen] = '/';
        strncpy(out + dlen + 1, name, FAT_PATH_MAX - dlen - 2);
        out[FAT_PATH_MAX - 1] = '\0';
    }
}

int devfat_mount_and_provision(uint16_t dev) {
    fat_diskio_bind(dev);

    if (f_mount(&fat_volume, "", 1) != FR_OK) {
        fat_diskio_bind(0);
        fat_active = false;
        return -1;
    }
    fat_active = true;
    fat_reset_inos();

    static const char *offload_dirs[] = {
        "/fuzix",
        "/fuzix/bin",
        "/fuzix/games",
        "/fuzix/include",
        "/fuzix/lib",
        "/fuzix/share",
        "/fuzix/share/man",
        "/fuzix/share/man/man1",
        "/fuzix/src",
        NULL
    };

    static FILINFO fno;
    for (int i = 0; offload_dirs[i] != NULL; i++) {
        if (f_stat(offload_dirs[i], &fno) != FR_OK) {
            f_mkdir(offload_dirs[i]);
        }
    }
    return 0;
}

void devfat_unmount(void) {
    if (fat_active) {
        f_mount(NULL, "", 0);
        fat_diskio_bind(0);
        fat_active = false;
        memset(fat_paths, 0, sizeof(fat_paths));
    }
}

int devfat_stat_path(const char *path, uint32_t *size_out, uint8_t *is_dir_out) {
    if (!fat_active)
        return -1;
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        if (size_out) *size_out = 0;
        if (is_dir_out) *is_dir_out = 1;
        return 0;
    }
    static FILINFO fno;
    if (f_stat(path, &fno) != FR_OK)
        return -1;
    if (size_out) *size_out = (uint32_t)fno.fsize;
    if (is_dir_out) *is_dir_out = (fno.fattrib & AM_DIR) ? 1 : 0;
    return 0;
}

int devfat_read_file(const char *path, uint32_t offset, void *ubuf, uint16_t len) {
    if (!fat_active)
        return -1;
    static FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
        return -1;
    if (f_lseek(&fp, offset) != FR_OK) {
        f_close(&fp);
        return -1;
    }

    uint16_t total = 0;
    while (total < len) {
        uint16_t chunk = len - total;
        if (chunk > sizeof(fat_bounce))
            chunk = sizeof(fat_bounce);
        UINT br = 0;
        if (fatfs_read(&fp, fat_bounce, chunk, &br) != FR_OK) {
            f_close(&fp);
            return -1;
        }
        if (br == 0)
            break;
        if (udata.u_sysio) {
            memcpy((uint8_t *)ubuf + total, fat_bounce, br);
        } else if (uput(fat_bounce, (uint8_t *)ubuf + total, br) < 0) {
            f_close(&fp);
            return -1;
        }
        total += br;
        if (br < chunk)
            break;
    }
    f_close(&fp);
    return total;
}

int devfat_write_file(const char *path, uint32_t offset, const void *ubuf, uint16_t len, int create) {
    if (!fat_active)
        return -1;
    static FIL fp;
    FRESULT fr;
    FRESULT cr;
    BYTE mode = FA_WRITE | (create ? FA_OPEN_ALWAYS : FA_OPEN_EXISTING);
    if (f_open(&fp, path, mode) != FR_OK)
        return -1;
    if (f_lseek(&fp, offset) != FR_OK) {
        f_close(&fp);
        return -1;
    }

    uint16_t total = 0;
    while (total < len) {
        uint16_t chunk = len - total;
        if (chunk > sizeof(fat_bounce))
            chunk = sizeof(fat_bounce);
        if (udata.u_sysio) {
            memcpy(fat_bounce, (const uint8_t *)ubuf + total, chunk);
        } else if (uget((const uint8_t *)ubuf + total, fat_bounce, chunk) < 0) {
            f_close(&fp);
            return -1;
        }
        UINT bw = 0;
        if (fatfs_write(&fp, fat_bounce, chunk, &bw) != FR_OK) {
            f_close(&fp);
            return -1;
        }
        total += bw;
        if (bw < chunk)
            break;
    }
    /* f_close() flushes through f_sync().  Check both results, but call
     * them separately (no || short-circuit) so the FatFs open-object lock
     * is released even when the flush fails. */
    fr = f_sync(&fp);
    cr = f_close(&fp);
    if (fr != FR_OK || cr != FR_OK) {
        udata.u_error = EIO;
        return -1;
    }
    return total;
}

/*
 * Remove a file from a FAT directory.  unlink() must not use the generic
 * ch_link() path here: that would write a Fuzix directory record through
 * writei() and corrupt the FAT directory.
 */
int devfat_unlink(inoptr ino, inoptr pino, const char *name) {
    char path[FAT_PATH_MAX];

    (void)ino;
    if (!fat_active || pino->c_num < 1 || pino->c_num >= FAT_MAX_INOS)
        goto bad;
    fat_join_path(fat_paths[pino->c_num], name, path);
    if (f_unlink(path) != FR_OK)
        goto bad;
    /* Detach the path and every inode bound to it.  Otherwise a live
     * descriptor could recreate the deleted file through FA_OPEN_ALWAYS
     * on its next write, or alias a different file reusing this slot. */
    fat_invalidate_path(path);
    return 0;
bad:
    udata.u_error = EIO;
    return -1;
}

/* Remove an empty directory.  FatFs rejects non-empty directories. */
int devfat_rmdir(inoptr ino) {
    char path[FAT_PATH_MAX];

    if (!fat_active || ino->c_num < 1 || ino->c_num >= FAT_MAX_INOS)
        goto bad;
    /* Copy before invalidating: the helper clears this very slot. */
    strncpy(path, fat_paths[ino->c_num], FAT_PATH_MAX - 1);
    path[FAT_PATH_MAX - 1] = '\0';
    if (f_unlink(path) != FR_OK)
        goto bad;
    fat_invalidate_path(path);
    return 0;
bad:
    udata.u_error = EIO;
    return -1;
}

/* Create a directory directly through FatFs and return its inode. */
inoptr devfat_mkdir(inoptr pino, const char *name) {
    char path[FAT_PATH_MAX];
    uint16_t child;

    if (!fat_active || pino->c_num < 1 || pino->c_num >= FAT_MAX_INOS)
        goto bad;
    fat_join_path(fat_paths[pino->c_num], name, path);
    if (f_mkdir(path) != FR_OK)
        goto bad;
    child = fat_intern_path(path, pino->c_num);
    if (!child) {
        f_unlink(path);	/* roll back the directory we just created */
        udata.u_error = ENFILE;
        return NULLINODE;
    }
    return devfat_i_open(pino->c_dev, child);
bad:
    udata.u_error = EIO;
    return NULLINODE;
}

/* Rename a FAT file.  FatFs does not overwrite an existing destination (it
 * reports FR_EXIST), so remove a plain-file destination first to get POSIX
 * rename semantics.  Afterwards rebind path slots that named the source so
 * descriptors already open on it keep referring to the renamed file. */
int devfat_rename(inoptr srcp, const char *sname, inoptr dstp, const char *dname) {
    char sp[FAT_PATH_MAX];
    char dp[FAT_PATH_MAX];
    static FILINFO fno;
    FRESULT fr;
    uint16_t i;

    if (!fat_active ||
        srcp->c_num < 1 || srcp->c_num >= FAT_MAX_INOS ||
        dstp->c_num < 1 || dstp->c_num >= FAT_MAX_INOS) {
        udata.u_error = EIO;
        return -1;
    }
    fat_join_path(fat_paths[srcp->c_num], sname, sp);
    fat_join_path(fat_paths[dstp->c_num], dname, dp);

    /* Overwrite an existing plain-file destination.  Directories are left
     * for f_rename() to reject (FR_EXIST -> EEXIST). */
    if (f_stat(dp, &fno) == FR_OK && !(fno.fattrib & AM_DIR)) {
        fr = f_unlink(dp);
        if (fr != FR_OK) {
            udata.u_error = (fr == FR_EXIST) ? EEXIST : EIO;
            return -1;
        }
        /* Detach inodes bound to the old destination before its slot can
         * be taken over by the renamed source. */
        fat_invalidate_path(dp);
    }

    fr = f_rename(sp, dp);
    if (fr != FR_OK) {
        udata.u_error = (fr == FR_EXIST) ? EEXIST : EIO;
        return -1;
    }

    for (i = 1; i < FAT_MAX_INOS; i++) {
        if (fat_paths[i][0] && strcmp(fat_paths[i], sp) == 0) {
            strncpy(fat_paths[i], dp, FAT_PATH_MAX - 1);
            fat_paths[i][FAT_PATH_MAX - 1] = '\0';
            fat_parents[i] = dstp->c_num;
        }
    }
    return 0;
}

/*
 * Set the size of a FAT file.  The generic kernel truncation path only
 * knows how to walk its own block map, so FAT files must be shortened
 * through FatFs.  Without this, an O_TRUNC open (or creat()) leaves the
 * old tail of a longer file in place, silently corrupting any freshly
 * written file that happens to be shorter than the previous contents.
 */
int devfat_truncate(inoptr ino, uint32_t size) {
    if (!fat_active || ino->c_num < 1 || ino->c_num >= FAT_MAX_INOS)
        goto bad;
    if (getmode(ino) == MODE_R(F_DIR)) {
        udata.u_error = EISDIR;
        return -1;
    }

    static FIL fp;
    FRESULT fr;
    FRESULT cr;
    if (f_open(&fp, fat_paths[ino->c_num], FA_WRITE | FA_OPEN_ALWAYS) != FR_OK)
        goto bad;

    FSIZE_t cur = f_size(&fp);
    if (size < cur) {
        if (f_lseek(&fp, size) != FR_OK || f_truncate(&fp) != FR_OK)
            goto bad_close;
    } else if (size > cur) {
        /* FatFs is built without FF_USE_EXPAND, so extend the file by
         * writing zero bytes rather than seeking past EOF. */
        uint32_t left = (uint32_t)(size - cur);
        UINT bw;
        memset(fat_bounce, 0, sizeof(fat_bounce));
        if (f_lseek(&fp, cur) != FR_OK)
            goto bad_close;
        while (left) {
            UINT chunk = (left > sizeof(fat_bounce)) ? sizeof(fat_bounce) : (UINT)left;
            if (fatfs_write(&fp, fat_bounce, chunk, &bw) != FR_OK || bw != chunk)
                goto bad_close;
            left -= bw;
        }
    }
    /* Flush the directory/size update and close; check both results but
     * call them separately so the lock is freed even if the flush fails. */
    fr = f_sync(&fp);
    cr = f_close(&fp);
    if (fr != FR_OK || cr != FR_OK) {
        udata.u_error = EIO;
        return -1;
    }
    return 0;

bad_close:
    f_close(&fp);
bad:
    udata.u_error = EIO;
    return -1;
}

int devfat_readdir_entry(const char *dir_path, uint16_t index, char *name_out, uint8_t *is_dir_out) {
    if (!fat_active)
        return -1;
    static DIR dir;
    static FILINFO fno;
    if (f_opendir(&dir, dir_path) != FR_OK)
        return -1;

    uint16_t valid = 0;
    while (1) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
            f_closedir(&dir);
            return -1;
        }
        if (fno.fattrib & 0x0E)
            continue;
        if (fno.fname[0] == '.' && (fno.fname[1] == '\0' || (fno.fname[1] == '.' && fno.fname[2] == '\0')))
            continue;
        bool bad_name = false;
        for (int k = 0; fno.fname[k] != '\0'; k++) {
            uint8_t c = (uint8_t)fno.fname[k];
            if (c < 0x20 || c > 0x7E || c == '/') {
                bad_name = true;
                break;
            }
        }
        uint8_t c0 = (uint8_t)fno.fname[0];
        if (bad_name || !((c0 >= '0' && c0 <= '9') || (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_' || c0 == '-' || c0 == '.'))
            continue;
        if (valid == index)
            break;
        valid++;
    }
    strncpy(name_out, fno.fname, 29);
    name_out[29] = '\0';
    if (is_dir_out)
        *is_dir_out = (fno.fattrib & AM_DIR) ? 1 : 0;
    f_closedir(&dir);
    return 0;
}

inoptr devfat_i_open(uint16_t dev, uint16_t ino) {
    struct mount *m = fs_tab_get(dev);
    if (!m || !fat_active || ino < 1 || ino >= FAT_MAX_INOS || fat_paths[ino][0] == '\0') {
        udata.u_error = ENOENT;
        return NULLINODE;
    }

    uint32_t sz = 0;
    uint8_t is_dir = 0;
    if (devfat_stat_path(fat_paths[ino], &sz, &is_dir) != 0) {
        udata.u_error = ENOENT;
        return NULLINODE;
    }

    inoptr nindex = NULLINODE;
    for (inoptr j = i_tab; j < i_tab + ITABSIZE; j++) {
        if (!j->c_refs)
            nindex = j;
        /* Harden slot reuse against a stale/corrupted inode table (C7).
         * Only a slot that is genuinely live (c_refs > 0) and carries a
         * valid magic may match an already-open file.  If memory corruption
         * left c_refs == 0 on a slot whose c_dev/c_num still match, taking
         * the "reuse" branch would resurrect it (0 -> 1) while a stale OFT
         * still references it; the later close then derefs a zero-ref
         * inode and panics with "inode freed".  Such a slot is instead
         * treated as free and fully re-initialised below. */
        if (j->c_refs && j->c_magic == CMAGIC &&
            j->c_dev == dev && j->c_num == ino) {
            j->c_node.i_size = is_dir ? 0x7FFF : sz;
            /* FAT records no Unix ownership.  Attribute the inode to the
             * user that opened it so that the kernel's permission checks
             * (notably the write test after O_CREAT) work for non-root
             * users instead of seeing a root-owned 0644 file. */
            j->c_node.i_uid = udata.u_euid;
            j->c_node.i_gid = udata.u_egid;
            j->c_refs++;
            return j;
        }
    }

    if (!nindex) {
        udata.u_error = ENFILE;
        return NULLINODE;
    }

    memset(&nindex->c_node, 0, sizeof(nindex->c_node));
    nindex->c_node.i_mode = is_dir ? (F_DIR | 0777) : (F_REG | 0777);
    nindex->c_node.i_uid = udata.u_euid;
    nindex->c_node.i_gid = udata.u_egid;
    nindex->c_node.i_nlink = is_dir ? 2 : 1;
    nindex->c_node.i_size = is_dir ? 0x7FFF : sz;
    nindex->c_dev = dev;
    nindex->c_num = ino;
    nindex->c_super = m - fs_tab;
    nindex->c_magic = CMAGIC;
    nindex->c_flags = (m->m_flags & MS_RDONLY) ? CRDONLY : 0;
    nindex->c_refs = 1;
    return nindex;
}

inoptr devfat_srch_dir(inoptr wd, const char *compname) {
    if (!fat_active || wd->c_num < 1 || wd->c_num >= FAT_MAX_INOS)
        return NULLINODE;

    if (compname[0] == '.' && compname[1] == '\0')
        return devfat_i_open(wd->c_dev, wd->c_num);

    if (compname[0] == '.' && compname[1] == '.' && compname[2] == '\0')
        return devfat_i_open(wd->c_dev, fat_parents[wd->c_num]);

    static char child_path[FAT_PATH_MAX];
    fat_join_path(fat_paths[wd->c_num], compname, child_path);

    uint32_t sz = 0;
    uint8_t is_dir = 0;
    if (devfat_stat_path(child_path, &sz, &is_dir) != 0)
        return NULLINODE;

    uint16_t child_ino = fat_intern_path(child_path, wd->c_num);
    if (!child_ino) {
        udata.u_error = ENFILE;
        return NULLINODE;
    }
    return devfat_i_open(wd->c_dev, child_ino);
}

inoptr devfat_newfile(inoptr pino, const char *name) {
    if (!fat_active || pino->c_num < 1 || pino->c_num >= FAT_MAX_INOS)
        return NULLINODE;

    static char child_path[FAT_PATH_MAX];
    fat_join_path(fat_paths[pino->c_num], name, child_path);

    static FIL fp;
    FRESULT fr;
    FRESULT cr;
    if (f_open(&fp, child_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        udata.u_error = EIO;
        return NULLINODE;
    }
    /* Check the flush/close: a failed commit must not be reported as a
     * successful create.  Call both unconditionally to free the FatFs lock. */
    fr = f_sync(&fp);
    cr = f_close(&fp);
    if (fr != FR_OK || cr != FR_OK) {
        udata.u_error = EIO;
        return NULLINODE;
    }

    uint16_t child_ino = fat_intern_path(child_path, pino->c_num);
    if (!child_ino) {
        f_unlink(child_path);	/* roll back the file we just created */
        udata.u_error = ENFILE;
        return NULLINODE;
    }
    return devfat_i_open(pino->c_dev, child_ino);
}

void devfat_readi(inoptr ino) {
    udata.u_done = 0;
    if (!fat_active || ino->c_num < 1 || ino->c_num >= FAT_MAX_INOS) {
        udata.u_error = EIO;
        return;
    }

    const char *path = fat_paths[ino->c_num];
    if (getmode(ino) == MODE_R(F_DIR)) {
        static struct direct ent;
        while (udata.u_count >= DIR_LEN) {
            uint16_t idx = udata.u_offset / DIR_LEN;
            memset(&ent, 0, sizeof(ent));
            if (idx == 0) {
                ent.d_ino = ino->c_num;
                ent.d_name[0] = '.';
            } else if (idx == 1) {
                ent.d_ino = fat_parents[ino->c_num];
                ent.d_name[0] = '.';
                ent.d_name[1] = '.';
            } else {
                static char name[30];
                uint8_t is_dir = 0;
                if (devfat_readdir_entry(path, idx - 2, name, &is_dir) != 0)
                    break;
                static char child_path[FAT_PATH_MAX];
                fat_join_path(path, name, child_path);
                ent.d_ino = fat_intern_path(child_path, ino->c_num);
                strncpy(ent.d_name, name, 30);
            }
            if (udata.u_sysio) {
                memcpy(udata.u_base, &ent, DIR_LEN);
            } else if (uput(&ent, udata.u_base, DIR_LEN) < 0) {
                return;
            }
            udata.u_base += DIR_LEN;
            udata.u_offset += DIR_LEN;
            udata.u_count -= DIR_LEN;
            udata.u_done += DIR_LEN;
        }
        return;
    }

    if (ino->c_node.i_size <= udata.u_offset)
        return;
    uint32_t avail = ino->c_node.i_size - udata.u_offset;
    uint16_t to_read = (udata.u_count < avail) ? udata.u_count : (uint16_t)avail;
    int r = devfat_read_file(path, udata.u_offset, udata.u_base, to_read);
    if (r < 0) {
        udata.u_error = EIO;
        return;
    }
    udata.u_base += r;
    udata.u_offset += r;
    udata.u_count -= r;
    udata.u_done += r;
}

void devfat_writei(inoptr ino) {
    udata.u_done = 0;
    if (!fat_active || ino->c_num < 1 || ino->c_num >= FAT_MAX_INOS) {
        udata.u_error = EIO;
        return;
    }
    if (getmode(ino) == MODE_R(F_DIR)) {
        udata.u_error = EISDIR;
        return;
    }
    const char *path = fat_paths[ino->c_num];
    int w = devfat_write_file(path, udata.u_offset, udata.u_base, udata.u_count, 1);
    if (w < 0) {
        udata.u_error = EIO;
        return;
    }
    udata.u_base += w;
    udata.u_offset += w;
    udata.u_count -= w;
    udata.u_done += w;
    if (udata.u_offset > ino->c_node.i_size)
        ino->c_node.i_size = udata.u_offset;
}

int devfat_mkfs(uint16_t dev) {
    if (fat_active) {
        udata.u_error = EBUSY;
        return -1;
    }
    /* f_mkfs writes an MBR at LBA 0; it must be given the whole disk
     * (partition 0), never a partition device (/dev/hdb1) or it writes a
     * nested MBR inside the partition. */
    if ((dev & 0x0F) != 0) {
        udata.u_error = EINVAL;
        return -1;
    }

    /* A 512-byte work buffer makes FatFs write the FAT table one sector at
     * a time (~60k SD writes on a 116 GB card = minutes of unresponsive
     * console).  16 KB cuts that ~32x. */
    static uint8_t mkfs_work[4 * 1024];
    MKFS_PARM opt = { FM_ANY, 2, 0, 0, 0 };
    fat_diskio_bind(dev);
    FRESULT res = f_mkfs("", &opt, mkfs_work, sizeof(mkfs_work));
    fat_diskio_bind(0);

    if (res != FR_OK) {
        udata.u_error = EIO;
        return -1;
    }

    /* Pick up the partition table we just wrote. */
    blkdev_rescan(dev);
    return 0;
}

#ifndef DEVFAT_H
#define DEVFAT_H

#include <stdint.h>
#include <kernel.h>

int  devfat_mount_and_provision(uint16_t dev);
void devfat_unmount(void);
int  devfat_read_file(const char *path, uint32_t offset, void *ubuf, uint16_t len);
int  devfat_write_file(const char *path, uint32_t offset, const void *ubuf, uint16_t len, int create);
int  devfat_stat_path(const char *path, uint32_t *size_out, uint8_t *is_dir_out);
int  devfat_readdir_entry(const char *dir_path, uint16_t index, char *name_out, uint8_t *is_dir_out);

inoptr devfat_i_open(uint16_t dev, uint16_t ino);
inoptr devfat_srch_dir(inoptr wd, const char *compname);
inoptr devfat_newfile(inoptr pino, const char *name);
void   devfat_readi(inoptr ino);
void   devfat_writei(inoptr ino);
int    devfat_truncate(inoptr ino, uint32_t size);
int    devfat_unlink(inoptr ino, inoptr pino, const char *name);
int    devfat_rmdir(inoptr ino);
inoptr devfat_mkdir(inoptr pino, const char *name);
int    devfat_rename(inoptr srcp, const char *sname, inoptr dstp, const char *dname);
int    devfat_mkfs(uint16_t dev);

#endif

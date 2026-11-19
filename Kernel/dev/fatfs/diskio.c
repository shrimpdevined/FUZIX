#include <kernel.h>
#include <kdata.h>
#include <blkdev.h>
#include "ff.h"
#include "diskio.h"

static uint16_t bound_sd_dev = 0;

void fat_diskio_bind(uint16_t dev) {
    bound_sd_dev = dev;
}

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return (bound_sd_dev != 0) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    return (bound_sd_dev != 0) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    while (count) {
        UINT n = (count > 0xFFFF) ? 0xFFFF : count;
        if (blkdev_rw(bound_sd_dev, (uint32_t)sector, buff, (uint16_t)n, true) != 0)
            return RES_ERROR;
        sector += n;
        buff += (uint32_t)n * 512;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    while (count) {
        UINT n = (count > 0xFFFF) ? 0xFFFF : count;
        if (blkdev_rw(bound_sd_dev, (uint32_t)sector, (uint8_t *)buff, (uint16_t)n, false) != 0)
            return RES_ERROR;
        sector += n;
        buff += (uint32_t)n * 512;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        /* disk_write() is synchronous through blkdev_rw(); FatFs does not
         * use the Fuzix buffer cache.  Calling sync() here recursed from
         * deep inside f_unlink() into the SD driver, overflowing the small
         * kernel stack into udata (corrupting process fd tables), and wrote
         * a bogus Fuzix superblock over sector 1 of the FAT volume. */
        return RES_OK;
    case GET_SECTOR_COUNT: {
        uint32_t sectors = blkdev_size(bound_sd_dev);
        if (!buff || sectors == 0)
            return RES_ERROR;
        *(LBA_t *)buff = sectors;
        return RES_OK;
    }
    case GET_SECTOR_SIZE:
        if (!buff)
            return RES_PARERR;
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buff)
            return RES_PARERR;
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <timer.h>
#include <stdbool.h>
#include <stdlib.h>
#include <blkdev.h>
#include "dev/devsd.h"
#include "picosdk.h"
#include "globals.h"
#include "config.h"
#include <hardware/spi.h>

/* Shared SPI0 Bus: pins come from config.h (single source of truth). */
#define Pico_SD_SCK PIN_SPI0_SCK
#define Pico_SD_TX  PIN_SPI0_MOSI
#define Pico_SD_RX  PIN_SPI0_MISO
#define Pico_SD_CS  PIN_SD_CS
#define Pico_LCD_CS PIN_LCD_CS     /* must stay HIGH during SD transfers */

#define Pico_SD_SPI_MOD spi0

#define SLOW_SPEED 250000
/* SD clock after init.  The card is switched to high-speed mode with CMD6
 * before this is applied (see devsd_discard.c), which permits up to 50 MHz.
 * Set FCC/RELIABILITY: 12 MHz is conservative and works without CMD6 on all
 * cards; 25/50 MHz require CMD6 and good signal integrity. */
#ifndef SD_FAST_HZ
/* CMD6 high-speed mode (0x80FFFFF1) is now applied correctly, so 50 MHz is
 * in-spec for SDHC/SDXC cards.  The LCD shares this bus at 62 MHz, so the
 * LCD/SD mutual exclusion in devlcd.c keeps transactions from colliding. */
#define SD_FAST_HZ 50000000
#endif
#define FAST_SPEED SD_FAST_HZ

void sd_rawinit(void)
{
    //ensure display CS stays deselected on shared SPI0
    gpio_init(Pico_LCD_CS);
    gpio_set_function(Pico_LCD_CS, GPIO_FUNC_SIO);
    gpio_set_dir(Pico_LCD_CS, true);
    gpio_put(Pico_LCD_CS, true);

    //initilase GPIO ports
    gpio_init(Pico_SD_SCK);
    gpio_init(Pico_SD_TX);
    gpio_init(Pico_SD_RX);
    gpio_init(Pico_SD_CS);

    //set GPIO post function
    gpio_set_function(Pico_SD_SCK, GPIO_FUNC_SPI); // SCK
    gpio_set_function(Pico_SD_TX, GPIO_FUNC_SPI);  // TX
    gpio_set_function(Pico_SD_RX, GPIO_FUNC_SPI);  // RX
    gpio_pull_up(Pico_SD_RX);
    gpio_set_function(Pico_SD_CS, GPIO_FUNC_SIO);  // CS
    gpio_set_dir(Pico_SD_CS, true);
    gpio_put(Pico_SD_CS, true);

    //initalise SPI module
    spi_init(Pico_SD_SPI_MOD, SLOW_SPEED);
    spi_set_format(Pico_SD_SPI_MOD, 8, 0, 0, SPI_MSB_FIRST);
}

/* LCD and SD share spi0.  Every LCD write leaves the bus clock at 62 MHz;
 * remember which speed the SD card needs so each SD transaction can restore
 * it before clocking any data. */
static bool sd_fast_clock = false;

void sd_spi_clock(bool go_fast)
{
    sd_fast_clock = go_fast;
    spi_set_baudrate(Pico_SD_SPI_MOD,
        go_fast ? FAST_SPEED : SLOW_SPEED);
}

void sd_spi_raise_cs(void)
{
    gpio_put(Pico_SD_CS, true);
}

void sd_spi_lower_cs(void)
{
    gpio_put(Pico_LCD_CS, true);
    gpio_put(Pico_SD_CS, false);
    /* The shared bus clock may have been changed to the LCD's fast pixel
     * clock since our last transfer.  Restore the SD clock before we begin. */
    spi_set_baudrate(Pico_SD_SPI_MOD,
        sd_fast_clock ? FAST_SPEED : SLOW_SPEED);
}


void sd_spi_transmit_byte(uint_fast8_t b)
{
    spi_write_blocking(Pico_SD_SPI_MOD, (uint8_t*) &b, 1);
}

uint_fast8_t sd_spi_receive_byte(void)
{
    uint8_t b;
    spi_read_blocking(Pico_SD_SPI_MOD, 0xff, (uint8_t*) &b, 1);
    return b;
}

bool sd_spi_receive_sector(void)
{
    if (!blk_op.addr)
        return false;
    spi_read_blocking(Pico_SD_SPI_MOD, 0xff, (uint8_t*) blk_op.addr, 512);
    /* The blocking Pico SPI API has no per-byte error-token feedback; the
     * data-start token is validated by the caller (devsd.c). A completed
     * transfer therefore counts as success. */
    return true;
}

bool sd_spi_transmit_sector(void)
{
    if (!blk_op.addr)
        return false;
    spi_write_blocking(Pico_SD_SPI_MOD,  (uint8_t*) blk_op.addr, 512);
    return true;
}

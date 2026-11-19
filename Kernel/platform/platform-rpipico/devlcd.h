#ifndef DEVLCD_H
#define DEVLCD_H

#include <stdint.h>
#include <stdbool.h>

#define LCD_WIDTH           480
#define LCD_HEIGHT          320

/* /dev/fb0 ioctl commands */
#define FB_SET_MODE         0x4601
#define FB_SET_WINDOW       0x4602
#define FB_SET_BACKLIGHT    0x4603
#define FB_SET_COLORS       0x4604

#define FB_MODE_TEXT        0
#define FB_MODE_GFX         1

struct fb_rect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
};

struct fb_colors {
    uint16_t fg_rgb565;
    uint16_t bg_rgb565;
};

void devlcd_init(void);
void lcd_task(void);   /* repaint rows deferred while the SD card owned spi0 */
int  devfb_open(uint_fast8_t minor, uint16_t flag);
int  devfb_close(uint_fast8_t minor);
int  devfb_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);
int  devfb_ioctl(uint_fast8_t minor, uarg_t request, char *data);

#endif

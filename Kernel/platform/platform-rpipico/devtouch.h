#ifndef DEVTOUCH_H
#define DEVTOUCH_H

#include <stdint.h>

struct touch_event {
    uint16_t x;
    uint16_t y;
    uint8_t  pressed;
    uint8_t  points;
};

void devtouch_init(void);
int  devtouch_open(uint_fast8_t minor, uint16_t flag);
int  devtouch_close(uint_fast8_t minor);
int  devtouch_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);

#endif

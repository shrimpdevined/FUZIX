#include <kernel.h>
#include <kdata.h>
#include "picosdk.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "config.h"
#include "devtouch.h"

void devtouch_init(void) {
    gpio_init(PIN_CTP_RST);
    gpio_set_dir(PIN_CTP_RST, GPIO_OUT);
    gpio_put(PIN_CTP_RST, 0);
    busy_wait_ms(10);
    gpio_put(PIN_CTP_RST, 1);
    busy_wait_ms(50);

    gpio_init(PIN_CTP_INT);
    gpio_set_dir(PIN_CTP_INT, GPIO_IN);
    gpio_pull_up(PIN_CTP_INT);

    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(PIN_CTP_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_CTP_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_CTP_SDA);
    gpio_pull_up(PIN_CTP_SCL);

    /* Safe timed-out config writes (won't hang if touch controller absent) */
    uint8_t thresh[2] = { 0x80, 18 };
    uint8_t period[2] = { 0x88, 12 };
    i2c_write_timeout_us(i2c1, I2C_CTP_ADDR, thresh, 2, false, 5000);
    i2c_write_timeout_us(i2c1, I2C_CTP_ADDR, period, 2, false, 5000);
}

int devtouch_open(uint_fast8_t minor, uint16_t flag) {
    (void)minor; (void)flag;
    return 0;
}

int devtouch_close(uint_fast8_t minor) {
    (void)minor;
    return 0;
}

int devtouch_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag) {
    (void)minor; (void)rawflag; (void)flag;
    if (udata.u_count < sizeof(struct touch_event)) {
        udata.u_error = EINVAL;
        return -1;
    }

    uint8_t reg = 0x02;
    uint8_t buf[5] = { 0 };
    struct touch_event ev = { 0, 0, 0, 0 };

    if (i2c_write_timeout_us(i2c1, I2C_CTP_ADDR, &reg, 1, true, 5000) == 1) {
        if (i2c_read_timeout_us(i2c1, I2C_CTP_ADDR, buf, 5, false, 5000) == 5) {
            uint8_t pts = buf[0] & 0x0F;
            if (pts > 0 && pts <= 2) {
                uint16_t raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
                uint16_t raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
                ev.x = (raw_y < 480) ? (479 - raw_y) : 0;
                ev.y = (raw_x < 320) ? raw_x : 319;
                ev.pressed = 1;
                ev.points  = pts;
            }
        }
    }

    if (uput(&ev, udata.u_base, sizeof(ev)) < 0)
        return -1;
    return sizeof(ev);
}

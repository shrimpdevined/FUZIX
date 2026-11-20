#include <kernel.h>
#include <kdata.h>
#include <vt.h>
#include <printf.h>
#include "picosdk.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "config.h"
#include "devlcd.h"
#include "font6x8.h"
#include <devsd.h>
#ifndef SD_DRIVE_NONE
#define SD_DRIVE_NONE 0xFF   /* devsd.h keeps this private to the SD driver */
#endif

static uint16_t term_fg   = 0x07E0; /* Terminal green (RGB565) */
static uint16_t term_bg   = 0x0000; /* Black background */
static bool     gfx_mode  = false;
static bool     lcd_ready = false;
static uint8_t  cur_attr  = 0;
static struct fb_rect active_win = { 0, 0, LCD_WIDTH, LCD_HEIGHT };

/* Rows whose pixels may not match vt_shadow because the shared SPI bus was
 * owned by the SD card when the VT tried to draw them.  They are repainted
 * later from thread/idle context; text is never simply thrown away. */
static uint8_t row_dirty[VT_HEIGHT];

static uint8_t  vt_shadow[VT_HEIGHT][VT_WIDTH];

static inline void lcd_cs_assert(void) {
    gpio_put(PIN_SD_CS, 1);  /* Deselect SD card */
    gpio_put(PIN_LCD_CS, 0);
}

static inline void lcd_cs_release(void) {
    gpio_put(PIN_LCD_CS, 1);
}

/* LCD and SD share spi0.  If the SD driver owns the bus (sd_drive != NONE),
 * an LCD draw would deselect the SD card mid-transaction and switch the clock
 * to 62 MHz, corrupting it ("sd: timeout" spiral).  Skip LCD output while SD
 * is busy — the VT shadow buffer retains the state and redraws later. */
static inline int lcd_bus_busy(void) {
    return sd_drive != SD_DRIVE_NONE;
}

/* Common preamble for bulk pixel streams: D/C high, LCD CS asserted (SD
 * deselected on the shared bus) and the fast pixel clock selected. */
static void lcd_bulk_start(void) {
    gpio_put(PIN_LCD_DC, 1);
    lcd_cs_assert();
    spi_set_baudrate(spi0, 62 * 1000 * 1000);
}

static void lcd_cmd(uint8_t cmd) {
    gpio_put(PIN_LCD_DC, 0);
    lcd_cs_assert();
    spi_write_blocking(spi0, &cmd, 1);
    lcd_cs_release();
}

static void lcd_data(const uint8_t *buf, size_t len) {
    gpio_put(PIN_LCD_DC, 1);
    lcd_cs_assert();
    spi_write_blocking(spi0, buf, len);
    lcd_cs_release();
}

static void lcd_data_byte(uint8_t b) {
    lcd_data(&b, 1);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t caset[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t raset[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_cmd(0x2A);
    lcd_data(caset, 4);
    lcd_cmd(0x2B);
    lcd_data(raset, 4);
    lcd_cmd(0x2C);
}

void devlcd_init(void) {
    /* Initialize control pins and deselect both SPI0 slaves */
    gpio_init(PIN_LCD_CS);  gpio_set_dir(PIN_LCD_CS, GPIO_OUT);  gpio_put(PIN_LCD_CS, 1);
    gpio_init(PIN_SD_CS);   gpio_set_dir(PIN_SD_CS, GPIO_OUT);   gpio_put(PIN_SD_CS, 1);
    gpio_init(PIN_LCD_DC);  gpio_set_dir(PIN_LCD_DC, GPIO_OUT);
    gpio_init(PIN_LCD_RST); gpio_set_dir(PIN_LCD_RST, GPIO_OUT);
    gpio_init(PIN_LCD_BL);  gpio_set_dir(PIN_LCD_BL, GPIO_OUT);  gpio_put(PIN_LCD_BL, 0);

    /* Configure SPI0 Mode 0 at 10 MHz */
    spi_init(spi0, 10 * 1000 * 1000);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SPI0_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI0_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI0_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_SPI0_MISO);

    /* Hardware reset ST7796S (trimmed: onlySleep Out below needs a long wait) */
    gpio_put(PIN_LCD_RST, 1); busy_wait_ms(10);
    gpio_put(PIN_LCD_RST, 0); busy_wait_ms(20);
    gpio_put(PIN_LCD_RST, 1); busy_wait_ms(50);

    /* Bodmer ST7796 Init Sequence */
    lcd_cmd(0x01); busy_wait_ms(20);
    lcd_cmd(0x11); busy_wait_ms(120);   /* datasheet-required Sleep Out delay */

    lcd_cmd(0xF0); lcd_data_byte(0xC3);
    lcd_cmd(0xF0); lcd_data_byte(0x96);

    lcd_cmd(0x36); lcd_data_byte(0xE8); /* Landscape right-side up, BGR */
    lcd_cmd(0x3A); lcd_data_byte(0x55); /* RGB565 */

    lcd_cmd(0xB4); lcd_data_byte(0x01);
    { uint8_t d[] = { 0x80, 0x02, 0x3B }; lcd_cmd(0xB6); lcd_data(d, 3); }
    { uint8_t d[] = { 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 }; lcd_cmd(0xE8); lcd_data(d, 8); }

    lcd_cmd(0xC1); lcd_data_byte(0x06);
    lcd_cmd(0xC2); lcd_data_byte(0xA7);
    lcd_cmd(0xC5); lcd_data_byte(0x18);
    /* Held-breath time after VCOM/gamma/unlock is not required by the
     * ST7796S datasheet — removed 3x120ms of boot delay. */

    { uint8_t d[] = { 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B }; lcd_cmd(0xE0); lcd_data(d, 14); }
    { uint8_t d[] = { 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B }; lcd_cmd(0xE1); lcd_data(d, 14); }

    lcd_cmd(0xF0); lcd_data_byte(0x3C);
    lcd_cmd(0xF0); lcd_data_byte(0x69);

    lcd_cmd(0x21); /* Inversion ON */
    lcd_cmd(0x29); /* Display ON */
    busy_wait_ms(20);

    gpio_put(PIN_LCD_BL, 1);
    lcd_ready = true;
    vtinit();
}

void plot_char(int8_t y, int8_t x, uint16_t c) {
    if (!lcd_ready || x < 0 || x >= VT_WIDTH || y < 0 || y >= VT_HEIGHT)
        return;
    /* The shadow buffer is authoritative: update it even when the display
     * cannot be touched right now.  Otherwise text printed while the SD
     * card owns spi0 vanishes from the LCD permanently. */
    vt_shadow[y][x] = (uint8_t)((c & 0x7F) | (((c >> 8) & 0x02) ? 0x80 : 0));

    if (gfx_mode || lcd_bus_busy()) {
        row_dirty[y] = 1;
        return;
    }

    uint8_t ch = (uint8_t)(c & 0x7F);
    if (ch < 0x20 || ch > 0x7E) ch = ' ';
    bool inv = ((c >> 8) & 0x02) || (cur_attr & 0x02);
    uint16_t fg = __builtin_bswap16(inv ? term_bg : term_fg);
    uint16_t bg = __builtin_bswap16(inv ? term_fg : term_bg);
    uint16_t tile[6 * 8];
    const uint8_t *glyph = &font6x8[(ch - 0x20) * 6];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 6; col++) {
            tile[row * 6 + col] = (glyph[col] & (1 << row)) ? fg : bg;
        }
    }
    lcd_set_window(x * 6, y * 8, x * 6 + 5, y * 8 + 7);
    spi_set_baudrate(spi0, 62 * 1000 * 1000);
    lcd_data((uint8_t *)tile, sizeof(tile));
}

static uint16_t lcd_row_buf[LCD_WIDTH];

/* Returns true only if the row was actually redrawn (MASTER_PLAN D3): the
 * caller must keep row_dirty set on a skip so the repaint is not lost. */
static bool lcd_redraw_row(int8_t y) {
    if (!lcd_ready || gfx_mode || lcd_bus_busy() || y < 0 || y >= VT_HEIGHT) return false;
    uint16_t fg_norm = __builtin_bswap16(term_fg);
    uint16_t bg_norm = __builtin_bswap16(term_bg);
    lcd_set_window(0, y * 8, LCD_WIDTH - 1, y * 8 + 7);
    lcd_bulk_start();
    for (int row = 0; row < 8; row++) {
        int px = 0;
        for (int col = 0; col < VT_WIDTH; col++) {
            uint8_t sv = vt_shadow[y][col];
            uint8_t ch = sv & 0x7F;
            if (ch < 0x20 || ch > 0x7E) ch = ' ';
            bool inv = (sv & 0x80) != 0;
            uint16_t fg = inv ? bg_norm : fg_norm;
            uint16_t bg = inv ? fg_norm : bg_norm;
            const uint8_t *glyph = &font6x8[(ch - 0x20) * 6];
            for (int c = 0; c < 6; c++) {
                lcd_row_buf[px++] = (glyph[c] & (1 << row)) ? fg : bg;
            }
        }
        while (px < LCD_WIDTH) lcd_row_buf[px++] = bg_norm;
        spi_write_blocking(spi0, (uint8_t *)lcd_row_buf, sizeof(lcd_row_buf));
    }
    lcd_cs_release();
    return true;
}

void clear_lines(int8_t y, int8_t ct) {
    if (!lcd_ready || ct <= 0) return;
    if (y < 0) y = 0;
    if (y + ct > VT_HEIGHT) ct = VT_HEIGHT - y;
    for (int r = y; r < y + ct; r++) {
        for (int col = 0; col < VT_WIDTH; col++)
            vt_shadow[r][col] = ' ';
        row_dirty[r] = 1;
    }

    if (gfx_mode || lcd_bus_busy())
        return;

    uint16_t bg = __builtin_bswap16(term_bg);
    for (int i = 0; i < LCD_WIDTH; i++) lcd_row_buf[i] = bg;
    lcd_set_window(0, y * 8, LCD_WIDTH - 1, (y + ct) * 8 - 1);
    lcd_bulk_start();
    for (int r = 0; r < ct * 8; r++) {
        spi_write_blocking(spi0, (uint8_t *)lcd_row_buf, sizeof(lcd_row_buf));
    }
    lcd_cs_release();
    /* A full hardware clear supersedes any earlier partial redraw. */
    for (int r = y; r < y + ct; r++)
        row_dirty[r] = 0;
}

void clear_across(int8_t y, int8_t x, int16_t l) {
    for (int16_t i = 0; i < l; i++) plot_char(y, x + i, ' ');
}

/* The traditional VT cursor is drawn by repainting the old cell and then
 * painting an underscore at the new one for every single output byte.  On
 * a 480x320 SPI display that tripled an already slow path and, because it
 * ran on the serial path, throttled the console itself.  The display
 * follows the text stream exactly; a blinking block is not required. */
void cursor_off(void) { }
void cursor_on(int8_t y, int8_t x) {
    (void)y; (void)x;
}
void cursor_disable(void) { }
void scroll_up(void) {
    if (!lcd_ready) return;
    /* Move every line up one row in the shadow buffer first.  If the SPI
     * bus is unavailable we mark the rows dirty and repaint them later —
     * scrolling must not silently lose screen contents. */
    for (int r = 0; r < VT_HEIGHT - 1; r++) {
        for (int c = 0; c < VT_WIDTH; c++)
            vt_shadow[r][c] = vt_shadow[r + 1][c];
        row_dirty[r] = 1;
    }
    for (int c = 0; c < VT_WIDTH; c++)
        vt_shadow[VT_HEIGHT - 1][c] = ' ';
    row_dirty[VT_HEIGHT - 1] = 1;
    if (gfx_mode || lcd_bus_busy())
        return;
    for (int r = 0; r < VT_HEIGHT - 1; r++) {
        lcd_redraw_row(r);
        row_dirty[r] = 0;
    }
    clear_lines(VT_HEIGHT - 1, 1);
}

void scroll_down(void) {
    if (!lcd_ready) return;
    for (int r = VT_HEIGHT - 1; r > 0; r--) {
        for (int c = 0; c < VT_WIDTH; c++)
            vt_shadow[r][c] = vt_shadow[r - 1][c];
        row_dirty[r] = 1;
    }
    for (int c = 0; c < VT_WIDTH; c++)
        vt_shadow[0][c] = ' ';
    row_dirty[0] = 1;
    if (gfx_mode || lcd_bus_busy())
        return;
    for (int r = VT_HEIGHT - 1; r > 0; r--) {
        lcd_redraw_row(r);
        row_dirty[r] = 0;
    }
    clear_lines(0, 1);
}

/* Repaint rows that could not be drawn when the SD card owned spi0.
 * A small per-call budget keeps the idle hook responsive. */
void lcd_task(void) {
    if (!lcd_ready || gfx_mode || lcd_bus_busy())
        return;
    for (int y = 0, n = 0; y < VT_HEIGHT && n < 4; y++) {
        if (!row_dirty[y])
            continue;
        /* Only clear the dirty flag if the row was really drawn; a skipped
         * row (bus stolen by SD) must be retried later (MASTER_PLAN D3). */
        if (lcd_redraw_row(y)) {
            row_dirty[y] = 0;
            n++;
        }
    }
}
/* Pointer to the LCD shadow so the ioctl can uput() it straight to user
 * space without a 3.2 KB kernel-side staging buffer (RAM budget). */
const void *lcd_shadow_ptr(void)
{
    return vt_shadow;
}

void vtattr_notify(void)  { cur_attr = vtattr; }
void do_beep(void)        { }

int devfb_open(uint_fast8_t minor, uint16_t flag) { (void)minor; (void)flag; return 0; }
int devfb_close(uint_fast8_t minor) {
    (void)minor;
    if (gfx_mode) { gfx_mode = false; clear_lines(0, VT_HEIGHT); }
    return 0;
}
int devfb_ioctl(uint_fast8_t minor, uarg_t request, char *data) {
    (void)minor;
    switch (request) {
    case FB_SET_MODE:
        gfx_mode = ((uintptr_t)data == FB_MODE_GFX);
        if (gfx_mode) {
            /* MASTER_PLAN C5: render everything still queued for the VT
             * before the framebuffer takes over the panel.  Those bytes
             * already went out the serial sink, so dropping them here is
             * what made LCD/serial diverge.  This runs in syscall (thread)
             * context so the SPI render is safe. */
            vt_pump(4096);
            lcd_task();
        } else {
            /* Resync the shadow with the terminal state we return to. */
            clear_lines(0, VT_HEIGHT);
        }
        return 0;
    case FB_SET_WINDOW:
        if (uget(data, &active_win, sizeof(active_win)) < 0) return -1;
        if (active_win.w == 0 || active_win.h == 0 ||
            active_win.x + active_win.w > LCD_WIDTH ||
            active_win.y + active_win.h > LCD_HEIGHT) {
            udata.u_error = EINVAL;
            return -1;
        }
        lcd_set_window(active_win.x, active_win.y,
                       active_win.x + active_win.w - 1,
                       active_win.y + active_win.h - 1);
        return 0;
    case FB_SET_BACKLIGHT:
        gpio_put(PIN_LCD_BL, ((uintptr_t)data != 0) ? 1 : 0);
        return 0;
    case FB_SET_COLORS: {
        struct fb_colors cols;
        if (uget(data, &cols, sizeof(cols)) < 0) return -1;
        term_fg = cols.fg_rgb565;
        term_bg = cols.bg_rgb565;
        if (!gfx_mode) clear_lines(0, VT_HEIGHT);
        return 0;
    }
    default:
        udata.u_error = EINVAL;
        return -1;
    }
}
int devfb_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag) {
    (void)minor; (void)rawflag; (void)flag;
    static uint8_t bounce[512];
    usize_t transferred = 0;
    /* Never steal the shared SPI bus from an in-flight SD transaction.
     * Report the failure honestly: pretending the whole write succeeded
     * silently loses a full frame and makes verification impossible. */
    if (lcd_bus_busy()) {
        udata.u_error = EBUSY;
        return -1;
    }
    lcd_bulk_start();
    while (transferred < udata.u_count) {
        usize_t chunk = udata.u_count - transferred;
        if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
        if (uget(udata.u_base + transferred, bounce, chunk) < 0) {
            lcd_cs_release();
            return -1;
        }
        spi_write_blocking(spi0, bounce, chunk);
        transferred += chunk;
    }
    lcd_cs_release();
    return transferred;
}

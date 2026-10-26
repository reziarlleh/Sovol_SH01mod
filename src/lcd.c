#include "lcd.h"
#include "hal.h"
#include <string.h>
static uint8_t ram[22];
static const uint8_t digits[10] = {0x3Fu, 0x06u, 0x5Bu, 0x4Fu, 0x66u,
                                   0x6Du, 0x7Du, 0x07u, 0x7Fu, 0x6Fu};
static const uint8_t segment_address[7] = {0u, 1u, 2u, 3u, 4u, 5u, 13u};
static void bit(bool value) {
    hal_lcd_data(value);
    hal_delay_us(1u);
    hal_lcd_wr(false);
    hal_delay_us(1u);
    hal_lcd_wr(true);
    hal_delay_us(1u);
}
static void msb(uint16_t value, unsigned count) {
    while (count) {
        --count;
        bit((value & ((uint16_t)1u << count)) != 0u);
    }
}
static void end(void) {
    hal_lcd_cs(true);
    hal_lcd_data(true);
    hal_delay_us(1u);
}
static void command(uint16_t value) {
    hal_lcd_cs(false);
    msb(4u, 3u);
    msb(value, 9u);
    end();
}
static void flush(void) {
    unsigned a, b;
    for (a = 0u; a < sizeof ram; ++a) {
        hal_lcd_cs(false);
        msb(5u, 3u);
        msb((uint16_t)a, 6u);
        for (b = 0u; b < 4u; ++b)
            bit((ram[a] & (1u << b)) != 0u);
        end();
    }
}
static void digit(unsigned position, uint8_t mask) {
    unsigned b;
    for (b = 0u; b < 7u; ++b) {
        ram[segment_address[b]] &= (uint8_t)~(1u << position);
        if (mask & (1u << b))
            ram[segment_address[b]] |= (uint8_t)(1u << position);
    }
}
static void number(unsigned position, unsigned value) {
    digit(position, digits[(value / 10u) % 10u]);
    digit(position + 1u, digits[value % 10u]);
}
static void clear(void) { memset(ram, 0, sizeof ram); }
void lcd_init(void) {
    hal_backlight(false);
    hal_lcd_cs(true);
    hal_lcd_wr(true);
    hal_lcd_data(true);
    command(0x52u);
    command(0x30u);
    command(0u);
    command(0x0Au);
    command(4u);
    command(0u);
    clear();
    flush();
    /* controller_init enables the LCD after reading the saved power state. */
}
void lcd_enable(bool on) {
    if (on) {
        command(2u);
        command(6u);
        hal_backlight(true);
    } else {
        hal_backlight(false);
        command(4u);
        command(0u);
    }
}
void lcd_temperature_humidity(int16_t t10, uint16_t rh10, bool work) {
    unsigned t = (unsigned)(t10 < 0 ? 0 : t10) / 10u, rh = rh10 / 10u;
    if (t > 99u)
        t = 99u;
    if (rh > 99u)
        rh = 99u;
    clear();
    number(0u, t);
    number(2u, rh);
    ram[14] = (uint8_t)(3u | (work ? 8u : 0u));
    flush();
}
void lcd_missing(void) {
    unsigned i;
    clear();
    for (i = 0u; i < 4u; ++i)
        digit(i, 0x40u);
    ram[14] = 3u;
    flush();
}
void lcd_set_temperature(uint8_t t) {
    clear();
    number(0u, t);
    ram[14] = 5u;
    flush();
}
void lcd_time(uint32_t seconds, bool setting, bool work) {
    uint32_t hours = seconds / 3600u;
    unsigned minutes = (unsigned)(seconds / 60u) % 60u;
    if (hours > 99u)
        hours = 99u;
    clear();
    number(0u, (unsigned)hours);
    number(2u, minutes);
    ram[14] = work ? 8u : 0u;
    ram[15] = (uint8_t)(1u | (setting ? 2u : 0u));
    flush();
}
void lcd_error(uint8_t code) {
    clear();
    digit(0u, 0x79u);
    digit(3u, digits[code % 10u]);
    flush();
}
void lcd_fan_percent(uint8_t percent) {
    clear();
    digit(0u, 0x71u); /* F */
    if (percent >= 100u)
        digit(1u, digits[1]);
    number(2u, percent);
    ram[14] = 2u; /* percent only */
    flush();
}

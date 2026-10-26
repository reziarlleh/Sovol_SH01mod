#include "hal.h"
#include "lcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, count, bits, commands[32], command_count;
static bool selected, data, backlight, was_wr = true;
static uint16_t frame;
static uint8_t ram[22];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL lcd:%u %s\n", __LINE__, #x);                                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void hal_delay_us(uint32_t us) { CHECK(us >= 1u); }
void hal_backlight(bool on) { backlight = on; }
void hal_lcd_data(bool high) { data = high; }
void hal_lcd_wr(bool high) {
    if (selected && high && !was_wr) {
        frame = (uint16_t)((frame << 1) | data);
        ++bits;
    }
    was_wr = high;
}
void hal_lcd_cs(bool high) {
    if (high && selected) {
        ++count;
        if (bits == 12u) {
            CHECK(frame >> 9 == 4u && command_count < 32u);
            commands[command_count++] = frame & 511u;
        } else {
            CHECK(bits == 13u && frame >> 10 == 5u);
            unsigned address = (frame >> 4) & 63u;
            CHECK(address < 22u);
            uint8_t value = 0u;
            for (unsigned i = 0u; i < 4u; ++i)
                value |= (uint8_t)(((frame >> (3u - i)) & 1u) << i);
            ram[address] = value;
        }
    }
    selected = !high;
    if (selected) {
        frame = 0u;
        bits = 0u;
    }
}
static uint8_t digit(unsigned pos) {
    static const unsigned address[7] = {0u, 1u, 2u, 3u, 4u, 5u, 13u};
    uint8_t value = 0u;
    for (unsigned i = 0; i < 7u; ++i)
        value |= (uint8_t)(((ram[address[i]] >> pos) & 1u) << i);
    return value;
}
int main(void) {
    unsigned expected[6] = {0x52u, 0x30u, 0u, 0x0Au, 2u, 6u};
    lcd_init();
    CHECK(command_count == 6u && memcmp(expected, commands, sizeof expected) == 0 && backlight &&
          count == 28u);
    lcd_temperature_humidity(270, 450u, true);
    CHECK(digit(0) == 0x5Bu && digit(1) == 7u && digit(2) == 0x66u && digit(3) == 0x6Du);
    CHECK(ram[14] == 11u && ram[15] == 0u);
    lcd_set_temperature(70u);
    CHECK(digit(0) == 7u && digit(1) == 0x3Fu && digit(2) == 0u && digit(3) == 0u && ram[14] == 5u);
    lcd_time(6u * 3600u, true, false);
    CHECK(digit(0) == 0x3Fu && digit(1) == 0x7Du && digit(2) == 0x3Fu && digit(3) == 0x3Fu &&
          ram[15] == 3u);
    lcd_missing();
    for (unsigned i = 0; i < 4u; ++i)
        CHECK(digit(i) == 0x40u);
    lcd_error(4u);
    CHECK(digit(0) == 0x79u && digit(1) == 0u && digit(2) == 0u && digit(3) == 0x66u &&
          ram[14] == 0u);
    lcd_enable(false);
    CHECK(!backlight && commands[command_count - 2u] == 4u && commands[command_count - 1u] == 0u);
    lcd_enable(true);
    CHECK(backlight && commands[command_count - 2u] == 2u && commands[command_count - 1u] == 6u);
    printf("LCD: command bits, RAM, numeric T/RH, 70C, HH:MM, missing, error, OFF: %u "
           "checks passed\n",
           checks);
    return 0;
}

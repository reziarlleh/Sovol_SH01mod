#include "config.h"
#include "flash_hal.h"
#include "storage.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[32768], snapshot[32768];
static unsigned checks, writes, erases, operations, cut_at, cut_mode;
static unsigned page_erases[SH01_JOURNAL_PAGES];
static jmp_buf power_cut;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL storage:%u %s\n", __LINE__, #x);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
uint32_t flash_hal_read(uint32_t address) {
    uint32_t value;
    CHECK(address >= SH01_JOURNAL_BASE && address <= 32764u && address % 4u == 0u);
    memcpy(&value, flash + address, 4u);
    return value;
}
bool flash_hal_write(uint32_t address, uint32_t value) {
    uint32_t old = flash_hal_read(address);
    CHECK((old & value) == value);
    ++writes;
    ++operations;
    if (cut_at == operations && cut_mode == 0u)
        longjmp(power_cut, 1);
    if (cut_at == operations && cut_mode == 1u) {
        uint32_t partial = old & (value | 0xFFFF0000u);
        memcpy(flash + address, &partial, 4u);
        longjmp(power_cut, 1);
    }
    value &= old;
    memcpy(flash + address, &value, 4u);
    if (cut_at == operations && cut_mode == 2u)
        longjmp(power_cut, 1);
    return true;
}
bool flash_hal_erase(uint32_t address) {
    CHECK(address >= SH01_JOURNAL_BASE && address <= 0x7E00u && address % 512u == 0u);
    ++erases;
    ++operations;
    ++page_erases[(address - SH01_JOURNAL_BASE) / 512u];
    if (cut_at == operations && cut_mode == 0u)
        longjmp(power_cut, 1);
    if (cut_at == operations && cut_mode == 1u) {
        memset(flash + address, 255, 256u);
        longjmp(power_cut, 1);
    }
    memset(flash + address, 255, 512u);
    if (cut_at == operations && cut_mode == 2u)
        longjmp(power_cut, 1);
    return true;
}
static void reset(void) {
    memset(flash, 0x5A, sizeof flash);
    memset(flash + SH01_JOURNAL_BASE, 255, 4096u);
    writes = erases = operations = cut_at = cut_mode = 0u;
    memset(page_erases, 0, sizeof page_erases);
    storage_init();
}
static storage_cycle_t cycle(uint32_t remaining) {
    return (storage_cycle_t){remaining, 65u, 96u, true, 100u, false};
}
static void test_roundtrip_and_rotation(void) {
    storage_cycle_t loaded;
    reset();
    CHECK(!storage_load(&loaded));
    for (unsigned i = 0u; i < 650u; ++i) {
        storage_cycle_t c = cycle(345600u - i * 60u);
        CHECK(storage_save(&c));
        storage_init();
        CHECK(storage_load(&loaded) && loaded.running && loaded.remaining_s == c.remaining_s);
        CHECK(loaded.hours == 96u && loaded.target_c == 65u);
    }
    CHECK(erases == 18u && writes == 2600u);
    for (unsigned i = 1u; i < SH01_JOURNAL_PAGES; ++i)
        CHECK(page_erases[i] <= page_erases[0] && page_erases[0] - page_erases[i] <= 1u);
    for (unsigned i = 0u; i < SH01_JOURNAL_BASE; ++i)
        CHECK(flash[i] == 0x5Au);
    CHECK(storage_disarm());
    storage_init();
    CHECK(storage_load(&loaded) && !loaded.running);
}
static void test_torn_append_and_erase(void) {
    storage_cycle_t loaded, c;
    reset();
    c = cycle(345600u);
    CHECK(storage_save(&c));
    memcpy(snapshot, flash, sizeof flash);
    for (unsigned mode = 0u; mode < 3u; ++mode)
        for (unsigned step = 1u; step <= 4u; ++step) {
            memcpy(flash, snapshot, sizeof flash);
            operations = 0u;
            cut_at = step;
            cut_mode = mode;
            storage_init();
            if (setjmp(power_cut) == 0) {
                c = cycle(345540u);
                c.fan_percent = 30u;
                (void)storage_save(&c);
            }
            cut_at = 0u;
            storage_init();
            CHECK(storage_load(&loaded) && loaded.running);
            CHECK(loaded.remaining_s == 345600u || loaded.remaining_s == 345540u);
            CHECK(loaded.fan_percent == (loaded.remaining_s == 345600u ? 100u : 30u));
            c = cycle(345480u);
            CHECK(storage_save(&c));
            storage_init();
            CHECK(storage_load(&loaded) && loaded.remaining_s == 345480u);
        }
    reset();
    for (unsigned i = 0u; i < 200u; ++i) {
        c = cycle(345600u - i * 60u);
        CHECK(storage_save(&c));
    }
    memcpy(snapshot, flash, sizeof flash);
    for (unsigned mode = 0u; mode < 3u; ++mode)
        for (unsigned step = 1u; step <= 5u; ++step) {
            memcpy(flash, snapshot, sizeof flash);
            operations = 0u;
            cut_at = step;
            cut_mode = mode;
            storage_init();
            if (setjmp(power_cut) == 0) {
                c = cycle(333600u);
                (void)storage_save(&c);
            }
            cut_at = 0u;
            storage_init();
            CHECK(storage_load(&loaded) && loaded.running);
            CHECK(loaded.remaining_s == 333660u || loaded.remaining_s == 333600u);
        }
}
static void test_stop_and_corruption(void) {
    storage_cycle_t c = cycle(345600u), loaded;
    reset();
    CHECK(storage_save(&c));
    memcpy(snapshot, flash, sizeof flash);
    for (unsigned mode = 1u; mode <= 2u; ++mode) {
        memcpy(flash, snapshot, sizeof flash);
        storage_init();
        operations = 0u;
        cut_at = 1u;
        cut_mode = mode;
        if (setjmp(power_cut) == 0)
            (void)storage_disarm();
        cut_at = 0u;
        storage_init();
        CHECK(storage_load(&loaded) && !loaded.running);
    }
    reset();
    CHECK(storage_save(&c));
    c.remaining_s -= 60u;
    CHECK(storage_save(&c));
    flash[SH01_JOURNAL_BASE + 20u + 12u] ^= 1u;
    storage_init();
    CHECK(storage_load(&loaded) && !loaded.running);
    c.remaining_s -= 60u;
    CHECK(storage_save(&c));
    storage_init();
    CHECK(storage_load(&loaded) && loaded.running && loaded.remaining_s == 345480u);
    c.hours = 7u;
    CHECK(!storage_save(&c));
    c.hours = 96u;
    c.target_c = 71u;
    CHECK(!storage_save(&c));
    c.target_c = 65u;
    c.remaining_s = 345601u;
    CHECK(!storage_save(&c));
}
static void test_fan_settings_and_legacy(void) {
    storage_cycle_t loaded, c = cycle(345600u);
    reset();
    CHECK(storage_save(&c));
    /* Exactly the old format, not a fixture produced by the new decoder. */
    CHECK(flash_hal_read(SH01_JOURNAL_BASE + 8u) == (345600u | 65u << 19 | 9u << 26 | 1u << 30));
    for (unsigned t = 40u; t <= 70u; t += 5u)
        for (unsigned percent = 10u; percent <= 100u; percent += 10u) {
            c.target_c = (uint8_t)t;
            c.fan_percent = (uint8_t)percent;
            CHECK(storage_save(&c));
            storage_init();
            CHECK(storage_load(&loaded) && loaded.running && loaded.target_c == t &&
                  loaded.fan_percent == percent && loaded.remaining_s == c.remaining_s);
        }
    for (unsigned percent = 0u; percent < 256u; ++percent) {
        c.fan_percent = (uint8_t)percent;
        if (percent < 10u || percent > 100u || percent % 10u)
            CHECK(!storage_save(&c));
    }
}
static void test_powered_off_and_interrupted_save(void) {
    storage_cycle_t c = cycle(12345u), loaded;
    reset();
    CHECK(storage_save(&c));
    CHECK(storage_disarm()); /* This is the first stage of controller OFF. */
    memcpy(snapshot, flash, sizeof flash);
    for (unsigned mode = 0u; mode < 3u; ++mode)
        for (unsigned step = 1u; step <= 4u; ++step) {
            memcpy(flash, snapshot, sizeof flash);
            storage_init();
            operations = 0u;
            cut_at = step;
            cut_mode = mode;
            if (setjmp(power_cut) == 0) {
                c = cycle(12310u);
                c.running = false;
                c.powered_off = true;
                (void)storage_save(&c);
            }
            cut_at = 0u;
            storage_init();
            CHECK(storage_load(&loaded) && !loaded.running);
            CHECK(loaded.remaining_s == (loaded.powered_off ? 12310u : 12345u));
        }
    reset();
    c = cycle(12310u);
    c.running = false;
    c.powered_off = true;
    c.fan_percent = 30u;
    CHECK(storage_save(&c));
    storage_init();
    CHECK(storage_load(&loaded) && loaded.powered_off && !loaded.running &&
          loaded.remaining_s == 12310u && loaded.target_c == 65u && loaded.fan_percent == 30u);
    c.running = true;
    CHECK(!storage_save(&c)); /* OFF can never arm resume. */
    c.running = false;
    c.powered_off = false;
    CHECK(storage_save(&c));
    storage_init();
    CHECK(storage_load(&loaded) && !loaded.running && !loaded.powered_off &&
          loaded.remaining_s == 12310u);
}
int main(void) {
    test_powered_off_and_interrupted_save();
    test_fan_settings_and_legacy();
    test_roundtrip_and_rotation();
    test_torn_append_and_erase();
    test_stop_and_corruption();
    printf("storage: rotation, CRC, interrupted append/erase/stop, bounds: %u checks passed\n",
           checks);
    return 0;
}

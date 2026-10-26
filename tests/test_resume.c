#include "config.h"
#include "controller.h"
#include "flash_hal.h"
#include "hal.h"
#include "lcd.h"
#include "sensor.h"
#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static uint8_t journal[4096];
static bool keys[3], heater, fan, light, healthy = true;
static uint32_t now;
static unsigned lcd_wakes, alarms;
static bool leds[3];
static sensor_sample_t sample = {.temperature_x10 = 250, .humidity_x10 = 450u};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL resume:%u %s\n", __LINE__, #x);                                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
uint32_t flash_hal_read(uint32_t address) {
    uint32_t value;
    CHECK(address >= 0x7000u && address <= 0x7FFCu && address % 4u == 0u);
    memcpy(&value, journal + address - 0x7000u, 4u);
    return value;
}
bool flash_hal_write(uint32_t address, uint32_t value) {
    uint32_t old = flash_hal_read(address);
    CHECK((old & value) == value);
    memcpy(journal + address - 0x7000u, &value, 4u);
    return true;
}
bool flash_hal_erase(uint32_t address) {
    CHECK(address >= 0x7000u && address <= 0x7E00u && address % 512u == 0u);
    memset(journal + address - 0x7000u, 255, 512u);
    return true;
}
void hal_heater(bool on) { heater = on; }
static uint8_t fan_percent;
void hal_fan_percent(uint8_t percent) {
    fan_percent = percent;
    fan = percent != 0u;
}
void hal_click(void) {}
void hal_error_signal(void) { ++alarms; }
void hal_sound_stop(void) {}
void lcd_fan_percent(uint8_t percent) { (void)percent; }
void hal_led(unsigned key, bool on) {
    CHECK(key < 3u);
    leds[key] = on;
}
bool hal_key(unsigned key) {
    CHECK(key < 3u);
    return keys[key];
}
bool hal_adc(uint16_t *out) {
    *out = 20480u;
    return true;
}
bool sensor_ok(uint32_t t) {
    (void)t;
    return healthy;
}
const sensor_sample_t *sensor_get(void) { return &sample; }
bool sensor_overheated(uint32_t t) {
    (void)t;
    return sample.temperature_x10 >= SH01_CHAMBER_FAULT_X10;
}
bool sensor_primary_fault(uint32_t t) {
    (void)t;
    return false;
}
void lcd_enable(bool on) {
    light = on;
    if (on)
        ++lcd_wakes;
}
void lcd_temperature_humidity(int16_t t, uint16_t rh, bool work) {
    (void)t;
    (void)rh;
    (void)work;
}
void lcd_missing(void) {}
void lcd_profile(const char *name) { (void)name; }
void lcd_set_temperature(uint8_t t) { (void)t; }
void lcd_time(uint32_t t, bool setting, bool work) {
    (void)t;
    (void)setting;
    (void)work;
}
void lcd_error(uint8_t code) { (void)code; }
static void tick(uint32_t t) {
    now = t;
    controller_task(t);
}
static void press(unsigned key) {
    uint32_t t = now + 10u;
    keys[key] = true;
    tick(t);
    tick(t + 30u);
    keys[key] = false;
    tick(t + 80u);
    tick(t + 110u);
}
static void reboot(void) {
    memset(keys, 0, sizeof keys);
    now = 0u;
    light = false;
    lcd_wakes = alarms = 0u;
    controller_init(now);
    CHECK(!heater && !fan);
    tick(0u);
}

static void hold(unsigned key) {
    uint32_t t = now + 10u;
    keys[key] = true;
    tick(t);
    tick(t + 30u);
    tick(t + 2030u);
    keys[key] = false;
    tick(t + 2050u);
    tick(t + 2080u);
}
static void enter_custom(void) {
    press(1u);
    CHECK(controller_get()->menu == MENU_PROFILE && controller_get()->profile == 0u);
    hold(1u);
    CHECK(controller_get()->menu == MENU_TEMP);
}
static void test_checkpoint_off_and_fault(void) {
    storage_cycle_t loaded;
    memset(journal, 255, sizeof journal);
    reboot();
    CHECK(controller_get()->mode == MODE_VIEW);
    enter_custom();
    press(2u);
    press(1u);
    for (unsigned i = 0u; i < 9u; ++i)
        press(2u);
    press(1u);
    press(2u);
    press(2u);
    press(2u);
    press(1u);
    CHECK(fan_percent == 30u);
    CHECK(heater && fan && controller_get()->hours == 96u && controller_get()->target_c == 45u);
    tick(now + 60000u);
    CHECK(storage_load(&loaded) && loaded.running);
    uint32_t saved = loaded.remaining_s;
    tick(now + 13000u);
    reboot();
    CHECK(heater && fan && controller_get()->remaining_s == saved);
    CHECK(fan_percent == 30u && loaded.fan_percent == 30u);
    CHECK(controller_get()->target_c == 45u && controller_get()->hours == 96u);
    enter_custom(); /* Even a draft may not alter the confirmed OFF reset. */
    press(2u);
    hold(0u);
    CHECK(!heater && !fan && !light);
    CHECK(storage_load(&loaded) && loaded.powered_off && !loaded.running);
    CHECK(loaded.remaining_s == 21600u && loaded.target_c == 40u && loaded.hours == 6u &&
          loaded.fan_percent == 30u);
    uint32_t stopped = loaded.remaining_s;
    healthy = false;
    reboot();
    tick(6000u);
    CHECK(controller_get()->mode == MODE_OFF && !heater && !fan && !light);
    CHECK(!lcd_wakes && !alarms && !leds[0] && !leds[1] && !leds[2]);
    CHECK(controller_get()->target_c == 40u && controller_get()->remaining_s == stopped);
    healthy = true;
    reboot();
    CHECK(controller_get()->mode == MODE_OFF && !lcd_wakes && !light);
    press(0u);
    CHECK(controller_get()->mode == MODE_VIEW && light && !heater && !fan);
    CHECK(storage_load(&loaded) && !loaded.powered_off && !loaded.running);
    enter_custom();
    press(1u);
    press(1u);
    press(1u);
    CHECK(heater && controller_get()->remaining_s == stopped);
    healthy = false;
    tick(now + 1u);
    CHECK(!heater && controller_get()->mode == MODE_FAULT);
    healthy = true;
    reboot();
    CHECK(controller_get()->mode == MODE_VIEW && !heater);
}
static void test_power_cut_while_browsing_or_editing(void) {
    storage_cycle_t loaded;
    for (unsigned page = 0u; page < 4u; ++page) {
        memset(journal, 255, sizeof journal);
        storage_init();
        storage_cycle_t old = {20000u, 55u, 8u, true, 30u, false};
        CHECK(storage_save(&old));
        reboot();
        CHECK(heater && fan_percent == 30u);
        press(1u);
        if (page) {
            hold(1u);
            for (unsigned i = 1u; i < page; ++i)
                press(1u);
        }
        press(2u);
        tick(now + 1000u);
        CHECK(controller_get()->mode == MODE_DRY && fan_percent == 30u);
        CHECK(controller_get()->target_c == 55u && controller_get()->hours == 8u);
        CHECK(storage_load(&loaded) && loaded.running && loaded.remaining_s == old.remaining_s);
        reboot();
        CHECK(controller_get()->menu == MENU_NONE && controller_get()->mode == MODE_DRY);
        CHECK(controller_get()->target_c == 55u && controller_get()->hours == 8u &&
              controller_get()->remaining_s == 20000u && fan_percent == 30u);
    }
}
static void test_short_cycles_survive_checkpoint_and_reboot(void) {
    storage_cycle_t loaded;
    for (unsigned hours = 1u; hours <= 5u; ++hours) {
        memset(journal, 255, sizeof journal);
        reboot();
        enter_custom();
        press(1u);
        /* 6 -> ... -> 96 -> 1, then choose the required short duration. */
        for (unsigned i = 0u; i < 9u + hours; ++i)
            press(2u);
        press(1u);
        press(1u);
        CHECK(controller_get()->mode == MODE_DRY && controller_get()->hours == hours);
        CHECK(controller_get()->remaining_s == (uint32_t)hours * 3600u);
        tick(now + 60000u);
        CHECK(storage_load(&loaded) && loaded.running && loaded.hours == hours);
        uint32_t saved = loaded.remaining_s;
        reboot();
        CHECK(controller_get()->mode == MODE_DRY && controller_get()->hours == hours &&
              controller_get()->remaining_s == saved && heater && fan_percent == 100u);
    }
    /* HIPS and PA12 must retain their exact 3/4-hour profile durations too. */
    for (unsigned profile = 3u; profile <= 4u; ++profile) {
        memset(journal, 255, sizeof journal);
        reboot();
        press(1u);
        for (unsigned i = 0u; i < profile; ++i)
            press(2u);
        hold(1u);
        CHECK(controller_get()->target_c == 70u && controller_get()->hours == profile);
        reboot();
        CHECK(controller_get()->mode == MODE_DRY && controller_get()->target_c == 70u &&
              controller_get()->hours == profile && fan_percent == 100u);
    }
}
int main(void) {
    test_checkpoint_off_and_fault();
    test_power_cut_while_browsing_or_editing();
    test_short_cycles_survive_checkpoint_and_reboot();
    printf("resume integration: real controller + journal, checkpoint, drafts, short "
           "profiles, cold restart, stop, fault: %u checks passed\n", checks);
    return 0;
}

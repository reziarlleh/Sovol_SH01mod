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
void hal_fan(bool on) { fan = on; }
void hal_beep(bool on) { (void)on; }
void hal_led(unsigned key, bool on) {
    (void)key;
    (void)on;
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
void lcd_enable(bool on) { light = on; }
void lcd_temperature_humidity(int16_t t, uint16_t rh, bool work) {
    (void)t;
    (void)rh;
    (void)work;
}
void lcd_missing(void) {}
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
    light = true;
    controller_init(now);
    CHECK(!heater && !fan);
    tick(0u);
}
int main(void) {
    storage_cycle_t loaded;
    memset(journal, 255, sizeof journal);
    reboot();
    CHECK(controller_get()->mode == MODE_VIEW);
    press(1u);
    press(2u);
    press(1u);
    for (unsigned i = 0u; i < 9u; ++i)
        press(2u);
    press(1u);
    CHECK(heater && fan && controller_get()->hours == 96u && controller_get()->target_c == 45u);
    tick(now + 60000u);
    CHECK(storage_load(&loaded) && loaded.running);
    uint32_t saved = loaded.remaining_s;
    tick(now + 13000u);
    reboot();
    CHECK(heater && fan && controller_get()->remaining_s == saved);
    CHECK(controller_get()->target_c == 45u && controller_get()->hours == 96u);
    uint32_t t = now + 10u;
    keys[0] = true;
    tick(t);
    tick(t + 30u);
    tick(t + 2030u);
    CHECK(!heater && !fan && !light);
    reboot();
    CHECK(controller_get()->mode == MODE_VIEW && !heater && !fan);
    press(1u);
    press(1u);
    press(1u);
    CHECK(heater);
    healthy = false;
    tick(now + 1u);
    CHECK(!heater && controller_get()->mode == MODE_FAULT);
    healthy = true;
    reboot();
    CHECK(controller_get()->mode == MODE_VIEW && !heater);
    printf("resume integration: real controller + journal, checkpoint, cold "
           "restart, stop, fault: "
           "%u checks passed\n",
           checks);
    return 0;
}

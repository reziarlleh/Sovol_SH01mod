#include "config.h"
#include "controller.h"
#include "hal.h"
#include "lcd.h"
#include "sensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, scenarios;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL controller:%u %s\n", __LINE__, #x);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static bool contacts[3], heater, fan, light = true, beeper, healthy = true, adc_good = true;
static uint16_t adc_value = 20480u;
static uint32_t now;
static sensor_sample_t sample;
static unsigned screen, shown;
enum { SCREEN_T_RH, SCREEN_TEMP, SCREEN_TIME, SCREEN_ERROR, SCREEN_MISSING };
void hal_heater(bool on) { heater = on; }
void hal_fan(bool on) { fan = on; }
void hal_beep(bool on) { beeper = on; }
void hal_led(unsigned key, bool on) {
    (void)key;
    (void)on;
}
bool hal_key(unsigned key) {
    CHECK(key < 3u);
    return contacts[key];
}
bool hal_adc(uint16_t *out) {
    *out = adc_value;
    return adc_good;
}
bool sensor_ok(uint32_t t) {
    (void)t;
    return healthy;
}
const sensor_sample_t *sensor_get(void) { return &sample; }
void lcd_enable(bool on) { light = on; }
void lcd_temperature_humidity(int16_t t, uint16_t h, bool work) {
    (void)h;
    (void)work;
    screen = SCREEN_T_RH;
    shown = (unsigned)t;
}
void lcd_missing(void) { screen = SCREEN_MISSING; }
void lcd_set_temperature(uint8_t t) {
    screen = SCREEN_TEMP;
    shown = t;
}
void lcd_time(uint32_t seconds, bool setting, bool work) {
    (void)setting;
    (void)work;
    screen = SCREEN_TIME;
    shown = seconds;
}
void lcd_error(uint8_t e) {
    screen = SCREEN_ERROR;
    shown = e;
}
static const controller_state_t *view(void) { return controller_get(); }
static void task(uint32_t t) {
    now = t;
    controller_task(t);
    CHECK(heater == view()->heater && fan == view()->fan);
    if (view()->mode != MODE_DRY)
        CHECK(!heater && !fan);
    if (heater)
        CHECK(healthy && adc_good && sample.temperature_x10 < 730);
    if (view()->mode == MODE_OFF)
        CHECK(!heater && !fan && !light && !beeper);
}
static void reset_at(uint32_t t) {
    memset(contacts, 0, sizeof contacts);
    heater = fan = beeper = false;
    light = true;
    healthy = adc_good = true;
    adc_value = 20480u;
    sample = (sensor_sample_t){0};
    sample.temperature_x10 = 270;
    sample.humidity_x10 = 450u;
    now = t;
    controller_init(t);
    task(t);
}
static void click(unsigned key) {
    uint32_t start = now + 10u;
    contacts[key] = true;
    task(start);
    task(start + 30u);
    contacts[key] = false;
    task(start + 80u);
    task(start + 110u);
}
static void mode(void) { click(1u); }
static void start(void) {
    mode();
    mode();
    mode();
    CHECK(view()->mode == MODE_DRY);
}
static void test_menu(void) {
    unsigned i;
    ++scenarios;
    reset_at(0u);
    CHECK(view()->mode == MODE_VIEW && view()->target_c == 40u && view()->hours == 6u);
    CHECK(screen == SCREEN_T_RH && !heater);
    mode();
    CHECK(view()->mode == MODE_TEMP && screen == SCREEN_TEMP);
    for (i = 0u; i < 6u; ++i)
        click(2u);
    CHECK(shown == 70u && !heater);
    click(2u);
    CHECK(shown == 40u);
    mode();
    CHECK(view()->mode == MODE_TIME && shown == 21600u);
    for (i = 0u; i < 6u; ++i)
        click(2u);
    CHECK(view()->hours == 12u && shown == 43200u);
    click(2u);
    CHECK(view()->hours == 6u);
    mode();
    CHECK(view()->mode == MODE_DRY && heater && fan);
    click(0u);
    CHECK(screen == SCREEN_TIME);
    click(0u);
    CHECK(screen == SCREEN_T_RH);
}
static void test_temperature_and_cutoff(void) {
    ++scenarios;
    reset_at(0u);
    mode();
    for (unsigned i = 0u; i < 6u; ++i)
        click(2u);
    mode();
    mode();
    CHECK(view()->target_c == 70u && heater);
    sample.temperature_x10 = 700;
    task(now + 250u);
    CHECK(heater);
    sample.temperature_x10 = 710;
    task(now + 250u);
    CHECK(!heater && fan);
    sample.temperature_x10 = 729;
    task(now + 250u);
    CHECK(view()->mode == MODE_DRY);
    sample.temperature_x10 = 730;
    task(now + 250u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 4u && screen == SCREEN_ERROR);
}
static void test_plate_hysteresis_and_faults(void) {
    ++scenarios;
    reset_at(0u);
    start();
    CHECK(heater);
    adc_value = 1418u;
    task(now + 250u);
    CHECK(!heater && fan);
    adc_value = 1859u;
    task(now + 250u);
    CHECK(!heater);
    adc_value = 1860u;
    task(now + 250u);
    CHECK(heater);
    adc_value = 40000u;
    task(now + 250u);
    CHECK(view()->error == 2u && !heater);
    reset_at(0u);
    healthy = false;
    mode();
    mode();
    mode();
    CHECK(view()->error == 1u && !heater);
    reset_at(0u);
    adc_good = false;
    mode();
    mode();
    mode();
    CHECK(view()->error == 2u && !heater);
}
static void test_off_wake_and_boot_hold(void) {
    uint32_t press;
    ++scenarios;
    reset_at(0u);
    start();
    press = now + 10u;
    contacts[0] = true;
    task(press);
    task(press + 30u);
    task(press + 2029u);
    CHECK(view()->mode == MODE_DRY);
    task(press + 2030u);
    CHECK(view()->mode == MODE_OFF);
    contacts[0] = false;
    task(press + 2050u);
    task(press + 2080u);
    CHECK(view()->mode == MODE_OFF);
    click(0u);
    CHECK(view()->mode == MODE_VIEW && !heater && light);
    contacts[0] = true;
    controller_init(now);
    task(now + 3000u);
    CHECK(view()->mode == MODE_VIEW);
    contacts[0] = false;
    task(now + 10u);
    task(now + 30u);
    CHECK(view()->mode == MODE_VIEW);
}
static void test_timer_pause_expiry_and_wrap(void) {
    uint32_t remaining;
    ++scenarios;
    reset_at(0xFFFFF000u);
    start();
    remaining = view()->remaining_s;
    task(now + 3000u);
    CHECK(view()->remaining_s == remaining - 3u);
    mode();
    remaining = view()->remaining_s;
    CHECK(view()->mode == MODE_TEMP && !heater);
    task(now + 3000u);
    CHECK(view()->remaining_s == remaining);
    mode();
    CHECK(shown == remaining);
    mode();
    CHECK(view()->remaining_s == remaining && heater);
    task(now + remaining * 1000u);
    CHECK(view()->mode == MODE_OFF && !heater && !fan);
}
static void test_menu_timeout_and_bounce(void) {
    uint32_t at;
    ++scenarios;
    reset_at(0u);
    mode();
    click(2u);
    task(now + 5000u);
    CHECK(view()->mode == MODE_VIEW && view()->target_c == 40u && !heater);
    mode();
    mode();
    task(now + 5000u);
    CHECK(view()->mode == MODE_VIEW && !heater);
    mode();
    mode();
    click(2u);
    task(now + 5000u);
    CHECK(view()->mode == MODE_DRY && view()->remaining_s == 25200u && heater);
    reset_at(0u);
    at = now;
    for (unsigned i = 0u; i < 20u; ++i) {
        contacts[1] = !contacts[1];
        task(at + i * 3u);
    }
    contacts[1] = false;
    task(at + 100u);
    task(at + 150u);
    CHECK(view()->mode == MODE_VIEW);
}
static void test_held_menu_and_fault_latch(void) {
    uint32_t at;
    ++scenarios;
    reset_at(0u);
    at = now + 10u;
    contacts[1] = true;
    task(at);
    task(at + 30u);
    CHECK(view()->mode == MODE_TEMP);
    task(at + 2500u);
    CHECK(view()->mode == MODE_TEMP);
    contacts[1] = false;
    task(at + 2600u);
    task(at + 2630u);
    CHECK(view()->mode == MODE_TEMP);
    mode();
    mode();
    CHECK(heater);
    healthy = false;
    task(now + 10u);
    CHECK(view()->mode == MODE_FAULT && !heater);
    healthy = true;
    task(now + 1000u);
    CHECK(view()->mode == MODE_FAULT && !heater);
    click(0u);
    CHECK(view()->mode == MODE_VIEW && !heater);
    reset_at(0u);
    sample.temperature_x10 = -100;
    mode();
    mode();
    mode();
    CHECK(view()->error == 3u && !heater);
}
int main(void) {
    test_menu();
    test_temperature_and_cutoff();
    test_plate_hysteresis_and_faults();
    test_off_wake_and_boot_hold();
    test_timer_pause_expiry_and_wrap();
    test_menu_timeout_and_bounce();
    test_held_menu_and_fault_latch();
    printf("controller: %u scenarios, %u checks passed\n", scenarios, checks);
    return 0;
}

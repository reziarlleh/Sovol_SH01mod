#include "config.h"
#include "controller.h"
#include "hal.h"
#include "lcd.h"
#include "sensor.h"
#include "storage.h"
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
static bool primary_fault;
static uint16_t adc_value = 20480u;
static uint32_t now;
static sensor_sample_t sample;
static unsigned screen, shown;
static bool leds[3];
static unsigned led_changes[3];
static storage_cycle_t persisted;
static bool persisted_valid, save_ok = true, disarm_ok = true;
static unsigned saves, error_signals;
static uint8_t fan_percent;

void storage_init(void) {}
bool storage_load(storage_cycle_t *cycle) {
    if (!persisted_valid)
        return false;
    *cycle = persisted;
    return true;
}
bool storage_save(const storage_cycle_t *cycle) {
    ++saves;
    if (!save_ok)
        return false;
    persisted = *cycle;
    persisted_valid = true;
    return true;
}
bool storage_disarm(void) {
    if (!disarm_ok)
        return false;
    persisted.running = false;
    return true;
}
enum { SCREEN_T_RH, SCREEN_TEMP, SCREEN_TIME, SCREEN_ERROR, SCREEN_MISSING, SCREEN_FAN };
void hal_heater(bool on) { heater = on; }
void hal_fan_percent(uint8_t percent) {
    fan_percent = percent;
    fan = percent != 0u;
}
void hal_click(void) { beeper = true; }
void hal_error_signal(void) { ++error_signals; }
void hal_sound_stop(void) { beeper = false; }
void hal_led(unsigned key, bool on) {
    CHECK(key < 3u);
    if (leds[key] != on)
        ++led_changes[key];
    leds[key] = on;
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
bool sensor_overheated(uint32_t t) {
    (void)t;
    return sample.temperature_x10 >= SH01_CHAMBER_FAULT_X10;
}
bool sensor_primary_fault(uint32_t t) {
    (void)t;
    return primary_fault;
}
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
void lcd_fan_percent(uint8_t percent) {
    screen = SCREEN_FAN;
    shown = percent;
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
        CHECK(!heater && !fan && !light && !beeper && !leds[0] && !leds[1] && !leds[2]);
    else if (view()->error == 0u && healthy && adc_good && !primary_fault &&
             sample.temperature_x10 > -100 && sample.temperature_x10 < 730)
        CHECK(leds[0]);
}
static void reset_at(uint32_t t) {
    memset(contacts, 0, sizeof contacts);
    memset(leds, 0, sizeof leds);
    memset(led_changes, 0, sizeof led_changes);
    persisted_valid = false;
    save_ok = disarm_ok = true;
    saves = error_signals = 0u;
    fan_percent = 0u;
    heater = fan = beeper = false;
    light = true;
    healthy = adc_good = true;
    primary_fault = false;
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
/* Older scenarios retain their intent but explicitly confirm the new FAN page.
 * The dedicated fan scenario below exercises individual M presses. */
static void next_setting_or_start(void) {
    click(1u);
    if (view()->mode == MODE_FAN) {
        CHECK(screen == SCREEN_FAN && !heater && !fan);
        click(1u);
    }
}
static void start(void) {
    next_setting_or_start();
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->mode == MODE_DRY);
}
static void test_menu(void) {
    unsigned i;
    ++scenarios;
    reset_at(0u);
    CHECK(view()->mode == MODE_VIEW && view()->target_c == 40u && view()->hours == 6u);
    CHECK(screen == SCREEN_T_RH && !heater);
    next_setting_or_start();
    CHECK(view()->mode == MODE_TEMP && screen == SCREEN_TEMP);
    for (i = 0u; i < 6u; ++i)
        click(2u);
    CHECK(shown == 70u && !heater);
    click(2u);
    CHECK(shown == 40u);
    next_setting_or_start();
    CHECK(view()->mode == MODE_TIME && shown == 21600u);
    for (i = 0u; i < 3u; ++i)
        click(2u);
    CHECK(view()->hours == 12u && shown == 43200u);
    for (i = 0u; i < 7u; ++i)
        click(2u);
    CHECK(view()->hours == 6u);
    next_setting_or_start();
    CHECK(view()->mode == MODE_DRY && heater && fan);
    click(0u);
    CHECK(screen == SCREEN_ERROR && shown == 0u && view()->mode == MODE_DRY && heater);
    task(now + SH01_ERROR_VIEW_MS);
    CHECK(screen == SCREEN_T_RH);
}
static void test_temperature_and_cutoff(void) {
    ++scenarios;
    reset_at(0u);
    next_setting_or_start();
    for (unsigned i = 0u; i < 6u; ++i)
        click(2u);
    next_setting_or_start();
    next_setting_or_start();
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
    CHECK(view()->mode == MODE_FAULT && view()->error == 4u && screen != SCREEN_ERROR);
    click(0u);
    CHECK(screen == SCREEN_ERROR && shown == 4u && view()->mode == MODE_FAULT);
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
    next_setting_or_start();
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->error == 1u && !heater);
    reset_at(0u);
    adc_good = false;
    next_setting_or_start();
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->error == 2u && !heater);
    reset_at(0u);
    start();
    primary_fault = true; /* Reserve warning alone does not stop a valid source. */
    task(now + 10u);
    CHECK(view()->error == 0u && heater && fan && persisted.running);
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
    next_setting_or_start();
    remaining = view()->remaining_s;
    CHECK(view()->mode == MODE_TEMP && !heater);
    task(now + 3000u);
    CHECK(view()->remaining_s == remaining);
    next_setting_or_start();
    CHECK(shown == remaining);
    next_setting_or_start();
    CHECK(view()->remaining_s == remaining && heater);
    task(now + remaining * 1000u);
    CHECK(view()->mode == MODE_OFF && !heater && !fan);
}
static void test_menu_timeout_and_bounce(void) {
    uint32_t at;
    ++scenarios;
    reset_at(0u);
    next_setting_or_start();
    click(2u);
    task(now + 5000u);
    CHECK(view()->mode == MODE_VIEW && view()->target_c == 40u && !heater);
    next_setting_or_start();
    next_setting_or_start();
    task(now + 5000u);
    CHECK(view()->mode == MODE_VIEW && !heater);
    next_setting_or_start();
    next_setting_or_start();
    click(2u);
    task(now + 5000u);
    CHECK(view()->mode == MODE_FAN && !heater);
    task(now + 5000u);
    CHECK(view()->mode == MODE_DRY && view()->remaining_s == 28800u && heater);
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
    next_setting_or_start();
    next_setting_or_start();
    CHECK(heater);
    healthy = false;
    task(now + 10u);
    CHECK(view()->mode == MODE_FAULT && !heater);
    healthy = true;
    task(now + 1000u);
    CHECK(view()->mode == MODE_FAULT && !heater);
    click(0u);
    CHECK(view()->mode == MODE_FAULT && screen == SCREEN_ERROR && shown == 1u && !heater);
    next_setting_or_start();
    CHECK(view()->mode == MODE_TEMP && !heater);
    reset_at(0u);
    sample.temperature_x10 = -100;
    next_setting_or_start();
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->error == 3u && !heater);
}
static void test_leds_do_not_flicker(void) {
    uint32_t epoch;
    unsigned power_changes, mode_changes;
    ++scenarios;
    reset_at(0u);
    CHECK(leds[0] && !leds[1] && !leds[2]);
    power_changes = led_changes[0];
    for (unsigned i = 1u; i <= 1000u; ++i)
        task(i);
    CHECK(led_changes[0] == power_changes && !leds[1] && !leds[2]);
    next_setting_or_start();
    CHECK(leds[1]);
    mode_changes = led_changes[1];
    epoch = now;
    for (unsigned i = 1u; i <= 1000u; ++i)
        task(epoch + i);
    CHECK(led_changes[0] == power_changes && led_changes[1] == mode_changes);
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->mode == MODE_DRY && leds[0] && !leds[1]);
}
static void test_time_presets_and_partial_hour(void) {
    static const uint8_t expected[] = {6u, 8u, 10u, 12u, 18u, 24u, 32u, 48u, 72u, 96u};
    ++scenarios;
    reset_at(0u);
    next_setting_or_start();
    next_setting_or_start();
    for (unsigned i = 0u; i < sizeof expected; ++i) {
        CHECK(view()->hours == expected[i] && shown == (uint32_t)expected[i] * 3600u);
        CHECK(!heater && !fan);
        click(2u);
    }
    CHECK(view()->hours == 6u && shown == 21600u);
    next_setting_or_start();
    task(now + 1000u);
    CHECK(view()->remaining_s == 21599u);
    next_setting_or_start();
    next_setting_or_start();
    click(2u);
    CHECK(view()->hours == 6u && shown == 21600u);
    click(2u);
    CHECK(view()->hours == 8u && shown == 28800u);
    next_setting_or_start();
    task(now + 1000u);
    next_setting_or_start();
    next_setting_or_start();
    CHECK(shown == 28799u);
    click(2u);
    CHECK(view()->hours == 8u && shown == 28800u);
    click(2u);
    CHECK(view()->hours == 10u && shown == 36000u);
}
static void test_96_hour_countdown_and_wrap(void) {
    uint32_t started;
    ++scenarios;
    reset_at(0xFFFFF000u);
    next_setting_or_start();
    next_setting_or_start();
    for (unsigned i = 0u; i < 9u; ++i)
        click(2u);
    CHECK(view()->hours == 96u && shown == 345600u);
    next_setting_or_start();
    CHECK(view()->mode == MODE_DRY && view()->remaining_s == 345600u && heater && fan);
    started = now;
    for (unsigned hour = 1u; hour < 96u; ++hour) {
        task(started + hour * 3600000u);
        CHECK(view()->mode == MODE_DRY && view()->remaining_s == (96u - hour) * 3600u);
    }
    task(started + 345599000u);
    CHECK(view()->mode == MODE_DRY && view()->remaining_s == 1u);
    task(started + 345600000u);
    CHECK(view()->mode == MODE_OFF && !heater && !fan && !light);
}
static void test_power_restore_and_stopped_state(void) {
    uint32_t checkpoint;
    ++scenarios;
    reset_at(0u);
    next_setting_or_start();
    click(2u);
    next_setting_or_start();
    click(2u);
    next_setting_or_start();
    CHECK(view()->target_c == 45u && view()->hours == 8u && persisted.running && saves == 2u);
    task(now + 60000u);
    checkpoint = view()->remaining_s;
    CHECK(persisted.remaining_s == checkpoint && saves == 3u);
    task(now + 30000u);
    memset(contacts, 0, sizeof contacts);
    now = 0u;
    controller_init(now);
    CHECK(view()->mode == MODE_VIEW && !heater && !fan);
    task(0u);
    CHECK(view()->mode == MODE_DRY && heater && fan && view()->remaining_s == checkpoint);
    CHECK(view()->target_c == 45u && view()->hours == 8u && saves == 3u);
    uint32_t press = now + 10u;
    contacts[0] = true;
    task(press);
    task(press + 30u);
    task(press + 2030u);
    CHECK(view()->mode == MODE_OFF && !persisted.running);
    memset(contacts, 0, sizeof contacts);
    now = 0u;
    controller_init(now);
    task(0u);
    CHECK(view()->mode == MODE_OFF && !heater && !fan && !light);
    CHECK(view()->target_c == 40u && view()->hours == 6u && view()->remaining_s == 21600u &&
          persisted.remaining_s == 21600u && persisted.powered_off && !persisted.running);
}
static void test_restore_wait_fault_and_save_failure(void) {
    ++scenarios;
    reset_at(0u);
    start();
    healthy = false;
    now = 0u;
    controller_init(now);
    task(0u);
    CHECK(view()->mode == MODE_VIEW && !heater);
    task(4999u);
    CHECK(!heater);
    healthy = true;
    task(5000u);
    CHECK(view()->mode == MODE_DRY && heater);
    reset_at(0u);
    start();
    healthy = false;
    now = 0u;
    controller_init(now);
    task(5000u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 1u && !persisted.running && !heater);
    reset_at(0u);
    save_ok = false;
    next_setting_or_start();
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->mode == MODE_FAULT && view()->error == 5u && !heater);
    reset_at(0u);
    start();
    save_ok = false;
    task(now + 60000u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 5u && !persisted.running && !heater);
    reset_at(0u);
    start();
    next_setting_or_start();
    CHECK(!persisted.running && !heater);
    now = 0u;
    controller_init(now);
    task(0u);
    CHECK(view()->mode == MODE_VIEW && !heater);
}
static void test_error_led_and_power_view(void) {
    ++scenarios;
    reset_at(0u);
    start();
    primary_fault = true;
    uint32_t at = now + 1u;
    task(at);
    CHECK(leds[0] && heater && fan);
    task(at + 499u);
    CHECK(leds[0]);
    task(at + 500u);
    CHECK(!leds[0] && heater && fan);
    task(at + 1000u);
    CHECK(leds[0]);
    click(0u);
    at = now;
    uint32_t remaining = view()->remaining_s;
    CHECK(screen == SCREEN_ERROR && shown == 6u && view()->mode == MODE_DRY);
    task(at + 4999u);
    CHECK(screen == SCREEN_ERROR && shown == 6u && heater);
    task(at + 5000u);
    CHECK(screen != SCREEN_ERROR && view()->remaining_s == remaining - 5u && heater);
    primary_fault = false;
    task(now + 1u);
    CHECK(leds[0]);
    click(0u);
    CHECK(screen == SCREEN_ERROR && shown == 0u && heater);
    /* A later fatal fault replaces the open E0 view and cannot be dismissed
     * or restarted by the diagnostic POWER press. */
    adc_good = false;
    primary_fault = true;
    task(now + 250u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 2u && !heater && shown == 2u);
    click(0u);
    CHECK(shown == 6u && view()->mode == MODE_FAULT);
    click(0u);
    CHECK(shown == 2u && view()->mode == MODE_FAULT);
    next_setting_or_start();
    CHECK(view()->mode == MODE_FAULT && !heater);
    task(now + 5000u);
    CHECK(screen != SCREEN_ERROR && view()->mode == MODE_FAULT && !heater);
    adc_good = true;
    primary_fault = false;
    task(now + 250u);
    click(0u);
    CHECK(shown == 2u && view()->mode == MODE_FAULT); /* Latched trip remains. */
    next_setting_or_start();
    CHECK(view()->mode == MODE_TEMP && !heater);
    next_setting_or_start();
    next_setting_or_start();
    CHECK(view()->mode == MODE_DRY && heater && view()->error == 0u);
    /* Blink/view deadlines remain valid across the millisecond wrap. */
    reset_at(0xFFFFFF00u);
    primary_fault = true;
    task(now + 1u);
    click(0u);
    CHECK(shown == 6u);
    task(now + 5000u);
    CHECK(screen != SCREEN_ERROR);
}
static void test_fan_menu_and_resume(void) {
    ++scenarios;
    reset_at(0u);
    CHECK(error_signals == 0u); /* No false NTC alarm before the first ADC. */
    click(1u);
    click(1u);
    click(1u);
    CHECK(view()->mode == MODE_FAN && shown == 100u && !heater && !fan);
    for (unsigned percent = 10u; percent <= 100u; percent += 10u) {
        click(2u);
        CHECK(screen == SCREEN_FAN && shown == percent && !heater && !fan);
    }
    click(2u);
    click(2u);
    click(2u);
    CHECK(shown == 30u);
    click(1u);
    CHECK(view()->mode == MODE_DRY && fan_percent == 30u && persisted.fan_percent == 30u);
    task(now + 60000u);
    CHECK(persisted.fan_percent == 30u && persisted.running);
    controller_init(0u);
    task(0u);
    CHECK(view()->mode == MODE_DRY && fan_percent == 30u);
    click(1u);
    click(1u);
    click(1u);
    CHECK(view()->mode == MODE_FAN && shown == 30u && !fan);
    click(2u);
    task(now + 5000u);
    CHECK(view()->mode == MODE_DRY && fan_percent == 40u);
    reset_at(0u);
    click(1u);
    click(1u);
    click(1u);
    task(now + 5000u);
    CHECK(view()->mode == MODE_VIEW && !fan && !heater); /* Abandoned unchanged page. */
}
static void test_error_signal_edges(void) {
    ++scenarios;
    reset_at(0u);
    start();
    CHECK(error_signals == 0u);
    primary_fault = true;
    task(now + 1u);
    CHECK(error_signals == 1u && heater);
    for (unsigned i = 0u; i < 20u; ++i)
        task(now + 500u);
    click(0u);
    click(0u);
    CHECK(error_signals == 1u); /* Blink, LCD and viewing cannot retrigger sound. */
    primary_fault = false;
    task(now + 1u);
    CHECK(error_signals == 1u);
    primary_fault = true;
    task(now + 1u);
    CHECK(error_signals == 2u); /* A cleared error can form a new episode. */
    adc_good = false;
    task(now + 250u);
    CHECK(error_signals == 3u && !heater && !fan);
    task(now + 10000u);
    CHECK(error_signals == 3u);
}
int main(void) {
    test_fan_menu_and_resume();
    test_error_signal_edges();
    test_menu();
    test_temperature_and_cutoff();
    test_plate_hysteresis_and_faults();
    test_off_wake_and_boot_hold();
    test_timer_pause_expiry_and_wrap();
    test_menu_timeout_and_bounce();
    test_held_menu_and_fault_latch();
    test_leds_do_not_flicker();
    test_time_presets_and_partial_hour();
    test_96_hour_countdown_and_wrap();
    test_power_restore_and_stopped_state();
    test_restore_wait_fault_and_save_failure();
    test_error_led_and_power_view();
    printf("controller: %u scenarios, %u checks passed\n", scenarios, checks);
    return 0;
}

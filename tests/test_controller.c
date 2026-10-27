#include "config.h"
#include "controller.h"
#include "hal.h"
#include "lcd.h"
#include "profiles.h"
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
static char shown_name[5];
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
enum { SCREEN_T_RH, SCREEN_TEMP, SCREEN_TIME, SCREEN_ERROR, SCREEN_MISSING, SCREEN_FAN, SCREEN_PROFILE };
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
void lcd_profile(const char *name) {
    screen = SCREEN_PROFILE;
    memcpy(shown_name, name, sizeof shown_name);
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

static void hold(unsigned key) {
    uint32_t at = now + 10u;
    contacts[key] = true;
    task(at);
    task(at + 30u);
    task(at + 1530u);
    contacts[key] = false;
    task(at + 1550u);
    task(at + 1580u);
}
static void enter_custom(void) {
    click(1u);
    CHECK(view()->menu == MENU_PROFILE && view()->profile == 0u);
    hold(1u);
    CHECK(view()->menu == MENU_TEMP);
}
static void choose_hours(unsigned hours) {
    CHECK(view()->menu == MENU_TIME);
    for (unsigned i = 0u; shown != hours * 3600u && i < 15u; ++i)
        click(2u);
    CHECK(shown == hours * 3600u);
}
static void start(void) {
    enter_custom();
    hold(1u);
    CHECK(view()->mode == MODE_DRY && view()->menu == MENU_NONE);
}
static void start_45_96_30(void) {
    enter_custom();
    click(2u);
    click(1u);
    choose_hours(96u);
    click(1u);
    click(2u);
    click(2u);
    click(2u);
    hold(1u);
    CHECK(view()->mode == MODE_DRY && view()->target_c == 45u && view()->hours == 96u &&
          view()->fan_percent == 30u && heater && fan);
}
static void choose_profile(unsigned index) {
    click(1u);
    CHECK(view()->menu == MENU_PROFILE && view()->profile == 0u);
    for (unsigned i = 0u; i < index; ++i)
        click(2u);
    hold(1u);
}
static void test_profiles_order_and_cancel(void) {
    static const char names[][5] = {"CUSt", "AbS", "ASA", "HIPS", "PA12", "PCtG", "PEtG", "PLA", "TPU"};
    ++scenarios;
    reset_at(0u);
    CHECK(view()->mode == MODE_VIEW && view()->target_c == 40u && view()->hours == 6u);
    CHECK(view()->menu == MENU_NONE && screen == SCREEN_T_RH);
    unsigned saved = saves;
    click(1u);
    for (unsigned i = 0u; i < 9u; ++i) {
        CHECK(screen == SCREEN_PROFILE && !strcmp(shown_name, names[i]));
        CHECK(view()->profile == i && view()->mode == MODE_VIEW && saves == saved);
        CHECK(!heater && !fan && persisted.target_c == 40u && persisted.hours == 6u);
        click(1u); /* Short M keeps this selection; UP alone browses. */
        CHECK(view()->menu == MENU_PROFILE && view()->profile == i && saves == saved);
        click(2u);
    }
    CHECK(!strcmp(shown_name, "CUSt") && view()->profile == 0u);
    click(2u);
    click(1u);
    CHECK(view()->menu == MENU_PROFILE && view()->profile == 1u && !heater);
    click(0u); /* POWER cancels the selection without showing diagnostics. */
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_VIEW && !heater && !fan);
    CHECK(saves == saved && view()->remaining_s == 21600u && screen != SCREEN_ERROR);
    click(1u);
    CHECK(!strcmp(shown_name, "CUSt") && view()->profile == 0u);
    CHECK(!strcmp(profile_get(255u)->name, "CUSt"));
}
static void test_all_preset_starts(void) {
    static const uint8_t temperatures[] = {0u, 70u, 70u, 70u, 70u, 65u, 60u, 45u, 60u};
    static const uint8_t hours[] = {0u, 6u, 8u, 3u, 4u, 8u, 6u, 6u, 6u};
    ++scenarios;
    for (unsigned i = 1u; i < 9u; ++i) {
        reset_at(0u);
        choose_profile(i);
        CHECK(view()->mode == MODE_DRY && view()->menu == MENU_NONE && heater && fan);
        CHECK(view()->target_c == temperatures[i] && view()->hours == hours[i]);
        CHECK(view()->remaining_s == (uint32_t)hours[i] * 3600u);
        CHECK(fan_percent == 100u && persisted.fan_percent == 100u && persisted.running);
        CHECK(persisted.target_c == temperatures[i] && persisted.hours == hours[i] && saves == 2u);
        task(now + 3000u); /* Release did not produce a second short M/menu. */
        CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY);
    }
    reset_at(0u);
    start_45_96_30();
    choose_profile(1u);
    CHECK(view()->target_c == 70u && view()->hours == 6u && fan_percent == 100u);
}
static void test_custom_drafts_and_all_values(void) {
    static const uint8_t hours[] = {6u, 8u, 10u, 12u, 18u, 24u, 32u, 48u, 72u, 96u, 1u, 2u, 3u, 4u, 5u};
    ++scenarios;
    reset_at(0u);
    enter_custom();
    CHECK(screen == SCREEN_TEMP && shown == 40u && !heater && !fan);
    for (unsigned t = 45u; t <= 70u; t += 5u) {
        click(2u);
        CHECK(shown == t && view()->target_c == 40u && saves == 1u);
    }
    click(2u);
    CHECK(shown == 40u);
    click(2u);
    click(1u);
    CHECK(view()->menu == MENU_TIME && screen == SCREEN_TIME);
    for (unsigned i = 0u; i < sizeof hours; ++i) {
        CHECK(shown == (uint32_t)hours[i] * 3600u);
        CHECK(view()->hours == 6u && view()->remaining_s == 21600u && saves == 1u);
        click(2u);
    }
    CHECK(shown == 21600u);
    choose_hours(1u);
    click(1u);
    CHECK(view()->menu == MENU_FAN && shown == 100u);
    for (unsigned percent = 10u; percent <= 100u; percent += 10u) {
        click(2u);
        CHECK(shown == percent && view()->fan_percent == 100u && !heater && !fan);
    }
    click(2u);
    click(2u);
    click(2u);
    CHECK(shown == 30u && persisted.target_c == 40u && !persisted.running);
    click(1u);
    CHECK(view()->menu == MENU_TEMP && shown == 45u && saves == 1u);
    click(1u);
    CHECK(view()->menu == MENU_TIME && shown == 3600u);
    click(1u);
    CHECK(view()->menu == MENU_FAN && shown == 30u);
    hold(1u);
    CHECK(view()->mode == MODE_DRY && view()->menu == MENU_NONE);
    CHECK(view()->target_c == 45u && view()->hours == 1u && view()->remaining_s == 3600u);
    CHECK(fan_percent == 30u && persisted.running && persisted.target_c == 45u &&
          persisted.hours == 1u && persisted.fan_percent == 30u);
}
static void test_running_browse_and_edit_are_passive(void) {
    ++scenarios;
    reset_at(0u);
    start_45_96_30();
    task(now + 60000u);
    uint32_t remaining = view()->remaining_s;
    unsigned saved = saves;
    click(1u);
    click(2u);
    task(now + 3000u);
    CHECK(view()->mode == MODE_DRY && heater && fan && fan_percent == 30u);
    CHECK(view()->target_c == 45u && view()->hours == 96u && saves == saved && persisted.running);
    CHECK(view()->remaining_s == remaining - 3u && view()->menu == MENU_PROFILE);
    click(0u);
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY && heater && fan);
    enter_custom();
    click(2u); /* Draft 50 C must not alter the active 45 C controller. */
    sample.temperature_x10 = 460;
    task(now + 250u);
    CHECK(!heater && fan && view()->target_c == 45u);
    click(1u);
    choose_hours(1u);
    click(1u);
    click(2u); /* Draft 40% must not alter the active 30%. */
    CHECK(shown == 40u && fan_percent == 30u && view()->hours == 96u);
    remaining = view()->remaining_s;
    task(now + 5000u);
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY);
    CHECK(view()->remaining_s == remaining - 5u && fan_percent == 30u && saves == saved);
    CHECK(persisted.target_c == 45u && persisted.hours == 96u && persisted.fan_percent == 30u);
}
static void test_timeout_never_starts(void) {
    ++scenarios;
    for (unsigned page = 0u; page < 4u; ++page) {
        reset_at(0u);
        click(1u);
        if (page) {
            hold(1u);
            for (unsigned i = 1u; i < page; ++i)
                click(1u);
        }
        click(2u); /* Include changed values on TEMP, TIME and FAN. */
        task(now + 5000u);
        CHECK(view()->menu == MENU_NONE && view()->mode == MODE_VIEW && !heater && !fan);
        CHECK(view()->target_c == 40u && view()->hours == 6u && view()->fan_percent == 100u);
        CHECK(saves == 1u && !persisted.running);
    }
}
static void test_hold_debounce_and_boot_keys(void) {
    ++scenarios;
    reset_at(0xFFFFFF00u);
    hold(1u); /* A hold begun on main must only open CUSt. */
    CHECK(view()->menu == MENU_PROFILE && view()->mode == MODE_VIEW && !heater);
    click(2u);
    uint32_t at = now + 10u;
    contacts[1] = true;
    task(at);
    task(at + 30u);
    task(at + 1529u);
    CHECK(view()->menu == MENU_PROFILE && view()->mode == MODE_VIEW && !heater);
    task(at + 1530u);
    CHECK(view()->mode == MODE_DRY && view()->menu == MENU_NONE && heater);
    task(at + 7000u); /* A held M is one action, not repeated starts. */
    CHECK(saves == 2u && view()->menu == MENU_NONE);
    contacts[1] = false;
    task(at + 7010u);
    task(at + 7040u);
    CHECK(saves == 2u && view()->mode == MODE_DRY && view()->menu == MENU_NONE);
    reset_at(0u);
    click(1u);
    click(2u);
    at = now + 10u;
    contacts[1] = true;
    task(at);
    task(at + 30u);
    contacts[1] = false;
    task(at + 1529u); /* Released one ms before the hold threshold. */
    task(at + 1559u);
    CHECK(view()->menu == MENU_PROFILE && view()->profile == 1u && !heater && !persisted.running);
    click(0u);
    CHECK(view()->menu == MENU_NONE && screen != SCREEN_ERROR);
    reset_at(0u);
    at = now;
    for (unsigned i = 0u; i < 20u; ++i) {
        contacts[1] = !contacts[1];
        task(at + i * 3u);
    }
    contacts[1] = false;
    task(at + 100u);
    task(at + 150u);
    CHECK(view()->menu == MENU_NONE);
    contacts[1] = true;
    controller_init(now);
    task(now + 10000u);
    CHECK(view()->menu == MENU_NONE && !heater);
    contacts[1] = false;
    task(now + 10u);
    task(now + 30u);
    CHECK(view()->menu == MENU_NONE);
    click(1u);
    hold(1u);
    CHECK(view()->menu == MENU_TEMP);
    hold(1u);
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY && heater); /* Long M starts from TEMP. */
}
static void test_temperature_plate_and_faults(void) {
    ++scenarios;
    reset_at(0u);
    choose_profile(1u);
    sample.temperature_x10 = 700;
    task(now + 250u);
    CHECK(heater);
    sample.temperature_x10 = 710;
    task(now + 250u);
    CHECK(!heater && fan);
    sample.temperature_x10 = 729;
    task(now + 250u);
    CHECK(view()->mode == MODE_DRY);
    click(1u);
    sample.temperature_x10 = 730;
    task(now + 250u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 4u && view()->menu == MENU_NONE);
    CHECK(!heater && !fan && !persisted.running);
    reset_at(0u);
    start();
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
    CHECK(view()->error == 2u && !heater && !fan);
    for (unsigned error = 1u; error <= 4u; ++error) {
        reset_at(0u);
        if (error == 1u) healthy = false;
        if (error == 2u) adc_good = false;
        if (error == 3u) sample.temperature_x10 = -100;
        if (error == 4u) sample.temperature_x10 = 730;
        choose_profile(1u);
        CHECK(view()->mode == MODE_FAULT && view()->error == error && !heater && !fan);
        CHECK(!persisted.running && view()->target_c == 40u && view()->hours == 6u);
    }
}
static void test_off_wake_and_draft_reset(void) {
    ++scenarios;
    reset_at(0u);
    start_45_96_30();
    enter_custom();
    click(2u);
    click(1u);
    choose_hours(1u);
    click(1u);
    click(2u);
    hold(0u);
    CHECK(view()->mode == MODE_OFF && view()->menu == MENU_NONE);
    CHECK(view()->target_c == 40u && view()->hours == 6u && view()->remaining_s == 21600u);
    CHECK(view()->fan_percent == 30u && persisted.powered_off && !persisted.running);
    controller_init(0u);
    now = 0u;
    task(0u);
    CHECK(view()->mode == MODE_OFF && !light && !heater && !fan);
    click(1u);
    hold(1u);
    click(2u);
    CHECK(view()->mode == MODE_OFF && !light);
    click(0u);
    CHECK(view()->mode == MODE_VIEW && light && !heater && !fan);
    CHECK(view()->remaining_s == 21600u && view()->fan_percent == 30u);
    contacts[0] = true;
    controller_init(now);
    task(now + 3000u);
    CHECK(view()->mode == MODE_VIEW && light);
    contacts[0] = false;
    task(now + 10u);
    task(now + 30u);
    CHECK(view()->mode == MODE_VIEW);
}
static void test_partial_hours_and_long_countdown(void) {
    ++scenarios;
    reset_at(0xFFFFF000u);
    start();
    task(now + 1000u);
    enter_custom();
    click(1u);
    CHECK(shown < 21600u && shown > 21590u);
    click(2u);
    CHECK(shown == 21600u); /* First greater preset, not eight hours. */
    click(2u);
    CHECK(shown == 28800u);
    choose_hours(96u);
    hold(1u);
    uint32_t started = now;
    CHECK(view()->remaining_s == 345600u);
    for (unsigned hour = 1u; hour < 96u; ++hour) {
        task(started + hour * 3600000u);
        CHECK(view()->mode == MODE_DRY && view()->remaining_s == (96u - hour) * 3600u);
    }
    task(started + 345599000u);
    CHECK(view()->remaining_s == 1u);
    task(started + 345600000u);
    CHECK(view()->mode == MODE_OFF && !heater && !fan && !light);
    reset_at(0u);
    enter_custom();
    click(1u);
    choose_hours(1u);
    hold(1u);
    task(now + 3600000u);
    CHECK(view()->mode == MODE_OFF && persisted.remaining_s == 0u && persisted.powered_off);
}
static void test_power_restore_and_pending_browse(void) {
    ++scenarios;
    reset_at(0u);
    start_45_96_30();
    task(now + 60000u);
    uint32_t checkpoint = persisted.remaining_s;
    healthy = false;
    now = 0u;
    controller_init(now);
    task(0u);
    click(1u); /* Browsing does not cancel automatic resume. */
    CHECK(view()->mode == MODE_VIEW && view()->menu == MENU_PROFILE && persisted.running);
    healthy = true;
    task(now + 250u);
    CHECK(view()->mode == MODE_DRY && view()->menu == MENU_PROFILE && heater && fan);
    CHECK(view()->remaining_s == checkpoint && fan_percent == 30u);
    click(0u);
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY);
    reset_at(0u);
    start();
    healthy = false;
    now = 0u;
    controller_init(now);
    task(4999u);
    CHECK(!heater);
    task(5000u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 1u && !persisted.running);
}
static void test_storage_failures_do_not_apply_drafts(void) {
    ++scenarios;
    reset_at(0u);
    save_ok = false;
    choose_profile(1u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 5u && !heater);
    CHECK(view()->target_c == 40u && view()->hours == 6u);
    reset_at(0u);
    start_45_96_30();
    save_ok = false;
    choose_profile(1u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 5u && !heater && !fan);
    CHECK(view()->target_c == 45u && view()->hours == 96u && view()->fan_percent == 30u);
    CHECK(!persisted.running);
    reset_at(0u);
    start();
    save_ok = false;
    task(now + 60000u);
    CHECK(view()->mode == MODE_FAULT && view()->error == 5u && !persisted.running && !heater);
}
static void test_errors_and_signals(void) {
    ++scenarios;
    reset_at(0u);
    start();
    CHECK(error_signals == 0u);
    primary_fault = true;
    uint32_t at = now + 1u;
    task(at);
    CHECK(error_signals == 1u && leds[0] && heater && fan);
    task(at + 499u);
    CHECK(leds[0]);
    task(at + 500u);
    CHECK(!leds[0] && heater && fan);
    task(at + 1000u);
    CHECK(leds[0] && error_signals == 1u);
    click(0u);
    CHECK(screen == SCREEN_ERROR && shown == 6u && view()->mode == MODE_DRY);
    uint32_t remaining = view()->remaining_s;
    task(now + 5000u);
    CHECK(screen != SCREEN_ERROR && view()->remaining_s == remaining - 5u && heater);
    primary_fault = false;
    task(now + 1u);
    CHECK(leds[0]);
    primary_fault = true;
    task(now + 1u);
    CHECK(error_signals == 2u);
    click(0u);
    adc_good = false;
    task(now + 250u);
    CHECK(view()->mode == MODE_FAULT && shown == 2u && error_signals == 3u);
    click(0u);
    CHECK(shown == 6u && !heater);
    click(0u);
    CHECK(shown == 2u);
    adc_good = true;
    primary_fault = false;
    task(now + 250u);
    click(0u);
    CHECK(shown == 2u && view()->mode == MODE_FAULT); /* Latched trip. */
    click(1u);
    CHECK(view()->menu == MENU_PROFILE && view()->mode == MODE_FAULT && !heater);
    click(0u);
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_FAULT && view()->error == 2u);
    choose_profile(1u);
    CHECK(view()->mode == MODE_DRY && view()->error == 0u && heater);
    reset_at(0xFFFFFF00u);
    primary_fault = true;
    task(now + 1u);
    click(0u);
    CHECK(shown == 6u);
    task(now + 5000u);
    CHECK(screen != SCREEN_ERROR);
}
static void test_display_rotation_and_stable_leds(void) {
    ++scenarios;
    reset_at(0u);
    unsigned changes = led_changes[0];
    uint32_t at = now;
    for (unsigned i = 1u; i <= 1000u; ++i)
        task(at + i);
    CHECK(led_changes[0] == changes && !leds[1] && !leds[2]);
    start();
    task(now + 4000u);
    CHECK(screen == SCREEN_TIME);
    task(now + 4000u);
    CHECK(screen == SCREEN_T_RH);
    click(1u);
    changes = led_changes[1];
    at = now;
    for (unsigned i = 1u; i <= 1000u; ++i)
        task(at + i);
    CHECK(led_changes[0] == 1u && leds[1] && led_changes[1] == changes);
    CHECK(screen == SCREEN_PROFILE && heater);
    click(0u);
    CHECK(screen == SCREEN_T_RH && view()->menu == MENU_NONE && heater);
    click(0u); /* Only a second POWER on main opens diagnostics. */
    CHECK(screen == SCREEN_ERROR && shown == 0u && heater);
    task(now + 5000u);
    CHECK(screen != SCREEN_ERROR && view()->menu == MENU_NONE && heater);
    click(1u);
    task(now + 5000u);
    CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY);
}
static void test_target_view_is_read_only_and_expires(void) {
    ++scenarios;
    reset_at(0xFFFFFF00u);
    unsigned saved = saves;
    uint32_t at = now + 10u;
    contacts[2] = true;
    task(at);
    task(at + 30u);
    CHECK(screen == SCREEN_TEMP && shown == 40u && view()->menu == MENU_NONE);
    contacts[2] = false;
    task(at + 80u);
    task(at + 110u);
    task(at + 5029u);
    CHECK(screen == SCREEN_TEMP && shown == 40u && saves == saved && !heater);
    task(at + 5030u);
    CHECK(screen == SCREEN_T_RH && view()->mode == MODE_VIEW && saves == saved);
    choose_profile(7u); /* PLA: target 45 C, active fan 100%. */
    task(now + 4000u);
    CHECK(screen == SCREEN_TIME);
    uint32_t remaining = view()->remaining_s;
    click(2u);
    CHECK(screen == SCREEN_TEMP && shown == 45u && heater && fan);
    CHECK(view()->menu == MENU_NONE && view()->target_c == 45u);
    task(now + 4000u);
    CHECK(screen == SCREEN_TEMP && shown == 45u && view()->remaining_s < remaining);
    click(2u); /* Repeated UP renews the five-second view. */
    task(now + 4000u);
    CHECK(screen == SCREEN_TEMP && shown == 45u && heater);
    task(now + 1000u);
    CHECK(screen == SCREEN_TIME && view()->mode == MODE_DRY && heater);
    click(2u);
    click(0u);
    CHECK(screen == SCREEN_ERROR && shown == 0u && heater);
    click(2u);
    CHECK(screen == SCREEN_TEMP && shown == 45u && heater);
    click(1u);
    CHECK(screen == SCREEN_PROFILE && !strcmp(shown_name, "CUSt") && heater);
    click(2u);
    CHECK(!strcmp(shown_name, "AbS")); /* UP still browses inside the menu. */
    click(0u);
    healthy = false;
    task(now + 1u);
    CHECK(view()->mode == MODE_FAULT && !heater && !fan);
    click(2u);
    CHECK(screen == SCREEN_TEMP && shown == 45u && view()->mode == MODE_FAULT);
    CHECK(!persisted.running); /* Viewing a target cannot clear a trip. */
    hold(0u);
    CHECK(view()->mode == MODE_OFF && !light && view()->target_c == 40u);
}

static void test_custom_start_from_any_parameter(void) {
    ++scenarios;
    for (unsigned page = 0u; page < 3u; ++page) {
        reset_at(0u);
        enter_custom();
        click(2u); /* 45 C */
        click(1u);
        choose_hours(1u);
        click(1u);
        click(2u);
        click(2u);
        click(2u); /* 30% */
        click(1u); /* FAN -> TEMP, preserving the whole draft. */
        CHECK(view()->menu == MENU_TEMP && shown == 45u && saves == 1u);
        for (unsigned i = 0u; i < page; ++i)
            click(1u);
        CHECK(view()->mode == MODE_VIEW && view()->target_c == 40u && !heater && !fan);
        hold(1u);
        CHECK(view()->menu == MENU_NONE && view()->mode == MODE_DRY && heater && fan);
        CHECK(view()->target_c == 45u && view()->hours == 1u && fan_percent == 30u);
        CHECK(view()->remaining_s == 3600u && saves == 2u && persisted.running);
        task(now + 1000u);
        CHECK(view()->menu == MENU_NONE && view()->remaining_s == 3599u && saves == 2u);
    }
}
static void test_power_cancels_each_menu_without_applying(void) {
    ++scenarios;
    for (unsigned running = 0u; running < 2u; ++running)
        for (unsigned page = 0u; page < 4u; ++page) {
            reset_at(0u);
            if (running)
                start_45_96_30();
            uint8_t target = view()->target_c, hours = view()->hours, speed = view()->fan_percent;
            unsigned saved = saves;
            uint32_t remaining = view()->remaining_s;
            if (page == 0u)
                click(1u);
            else {
                enter_custom();
                click(2u); /* Draft target only. */
                for (unsigned i = 1u; i < page; ++i)
                    click(1u);
            }
            click(2u); /* Change selected profile or current draft parameter. */
            click(0u);
            CHECK(view()->menu == MENU_NONE && screen != SCREEN_ERROR);
            CHECK(view()->mode == (running ? MODE_DRY : MODE_VIEW));
            CHECK(view()->target_c == target && view()->hours == hours && view()->fan_percent == speed);
            CHECK(saves == saved && persisted.running == (running != 0u));
            CHECK(view()->remaining_s <= remaining && (running || view()->remaining_s == remaining));
            CHECK(fan == (running != 0u) && (!running || fan_percent == speed));
            click(0u);
            CHECK(screen == SCREEN_ERROR && shown == 0u); /* POWER on main retains diagnostics. */
            enter_custom();
            CHECK(shown == target); /* Cancelled draft cannot leak into the next entry. */
            click(0u);
            CHECK(view()->menu == MENU_NONE && screen != SCREEN_ERROR);
        }
}
static void test_power_hold_boundary_1500_ms(void) {
    ++scenarios;
    reset_at(0xFFFFFF00u);
    start_45_96_30();
    enter_custom();
    click(2u);
    uint32_t at = now + 10u;
    contacts[0] = true;
    task(at);
    task(at + 30u);
    task(at + 1529u);
    CHECK(view()->mode == MODE_DRY && view()->menu == MENU_TEMP && heater && light);
    task(at + 1530u);
    CHECK(view()->mode == MODE_OFF && !heater && !fan && !light);
    CHECK(view()->target_c == 40u && view()->hours == 6u && view()->fan_percent == 30u);
    CHECK(persisted.powered_off && !persisted.running);
    contacts[0] = false;
    task(at + 1550u);
    task(at + 1580u);
    CHECK(view()->mode == MODE_OFF && !light); /* No short POWER wake on release. */
}

int main(void) {
    test_custom_start_from_any_parameter();
    test_power_cancels_each_menu_without_applying();
    test_power_hold_boundary_1500_ms();
    test_target_view_is_read_only_and_expires();
    test_profiles_order_and_cancel();
    test_all_preset_starts();
    test_custom_drafts_and_all_values();
    test_running_browse_and_edit_are_passive();
    test_timeout_never_starts();
    test_hold_debounce_and_boot_keys();
    test_temperature_plate_and_faults();
    test_off_wake_and_draft_reset();
    test_partial_hours_and_long_countdown();
    test_power_restore_and_pending_browse();
    test_storage_failures_do_not_apply_drafts();
    test_errors_and_signals();
    test_display_rotation_and_stable_leds();
    printf("controller: %u scenarios, %u checks passed\n", scenarios, checks);
    return 0;
}

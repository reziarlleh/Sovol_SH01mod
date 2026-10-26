#include "controller.h"
#include "config.h"
#include "hal.h"
#include "lcd.h"
#include "profiles.h"
#include "sensor.h"
#include "storage.h"
typedef struct {
    bool raw, stable, armed, long_sent;
    uint32_t changed, pressed;
} key_t;
static key_t keys[3];
static controller_state_t state;
static uint32_t last_second, last_lcd, next_adc, last_screen;
static uint32_t edit_seconds;
static uint32_t last_edit;
static uint8_t edit_target, edit_hours, edit_fan;
static bool dirty, plate_ok, plate_latched, adc_checked;
static bool resume_pending;
static bool error_view, target_view;
static uint8_t displayed_error;
static uint16_t error_mask;
static uint32_t error_view_at, error_blink_at;
static uint32_t target_view_at;
static uint32_t resume_started, last_checkpoint;
static uint16_t plate_adc;
static const uint8_t time_presets_h[] = {SH01_TIME_PRESET_VALUES};
/* While editing a running cycle, choose the next offered duration above the
 * remaining time, including a partial hour; the last preset wraps to one. */
static uint8_t next_time_hours(uint32_t seconds) {
    unsigned i;
    for (i = 0u; i < sizeof time_presets_h / sizeof time_presets_h[0]; ++i)
        if ((uint32_t)time_presets_h[i] * 3600u > seconds)
            return time_presets_h[i];
    return SH01_TIME_MIN_H;
}
static bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
    return (uint32_t)(now - since) >= interval;
}
static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static void outputs(bool heater, bool fan) {
    state.heater = heater;
    state.fan = fan;
    hal_heater(heater);
    hal_fan_percent(fan ? state.fan_percent : 0u);
}
/* One owner for LED state; LCD refresh must never pulse the button lights. */
static void update_leds(uint32_t now) {
    bool enabled = state.mode != MODE_OFF;
    bool blink_on =
        !error_mask || ((uint32_t)(now - error_blink_at) / SH01_ERROR_BLINK_MS) % 2u == 0u;
    hal_led(0u, enabled && blink_on);
    hal_led(1u, enabled && (state.menu != MENU_NONE || keys[1].stable));
    hal_led(2u, enabled && keys[2].stable);
}
static void off(bool reset_cycle) {
    outputs(false, false);
    resume_pending = false;
    hal_sound_stop();
    state.mode = MODE_OFF;
    state.menu = MENU_NONE;
    state.profile = 0u;
    error_view = false;
    target_view = false;
    error_mask = 0u;
    update_leds(0u);
    lcd_enable(false);
    if (reset_cycle) {
        state.target_c = SH01_TEMP_MIN_C;
        state.hours = SH01_TIME_DEFAULT_H;
        state.remaining_s = (uint32_t)state.hours * 3600u;
    }
    /* Disarm the old checkpoint before appending the OFF state.
     * An interrupted append must never restart a manually stopped heater. */
    bool stopped = storage_disarm();
    storage_cycle_t cycle = {
        state.remaining_s, state.target_c, state.hours, false, state.fan_percent, true};
    state.error = storage_save(&cycle) && stopped ? 0u : 5u;
}
static void wake(uint32_t now) {
    storage_cycle_t cycle = {state.remaining_s, state.target_c, state.hours, false,
                             state.fan_percent, false};
    if (!storage_save(&cycle))
        state.error = 5u;
    state.mode = state.error == 5u ? MODE_FAULT : MODE_VIEW;
    state.menu = MENU_NONE;
    state.time_screen = false;
    last_screen = now;
    lcd_enable(true);
    dirty = true;
}
static void fault(uint8_t error, uint32_t now) {
    outputs(false, false);
    resume_pending = false;
    if (!storage_disarm())
        error = 5u;
    state.error = error;
    state.mode = MODE_FAULT;
    state.menu = MENU_NONE;
    if (error_view) {
        displayed_error = error;
        error_view_at = now;
    }
    dirty = true;
}
static uint8_t safety(uint32_t now) {
    if (!sensor_ok(now))
        return 1u;
    if (!plate_ok || plate_adc >= 37200u || plate_adc < 100u)
        return 2u;
    if (sensor_overheated(now))
        return 4u;
    if (sensor_get()->temperature_x10 <= -100)
        return 3u;
    return 0u;
}
static uint16_t current_errors(uint32_t now) {
    uint16_t mask = state.error ? (uint16_t)(1u << state.error) : 0u;
    bool valid = sensor_ok(now);
    if (!valid)
        mask |= 1u << 1;
    if (adc_checked && (!plate_ok || plate_adc >= 37200u || plate_adc < 100u))
        mask |= 1u << 2;
    if (valid && sensor_get()->temperature_x10 <= -100)
        mask |= 1u << 3;
    if (valid && sensor_overheated(now))
        mask |= 1u << 4;
    if (sensor_primary_fault(now))
        mask |= 1u << 6;
    return mask;
}
static void show_error(uint32_t now) {
    uint16_t mask = current_errors(now);
    unsigned first = error_view && !elapsed(now, error_view_at, SH01_ERROR_VIEW_MS)
                         ? displayed_error % 6u + 1u
                         : 1u;
    displayed_error = 0u;
    for (unsigned i = 0u; i < 6u; ++i) {
        unsigned code = (first - 1u + i) % 6u + 1u;
        if (mask & (1u << code)) {
            displayed_error = (uint8_t)code;
            break;
        }
    }
    error_view = true;
    error_view_at = now;
    dirty = true;
}
static void begin_drying(uint8_t target, uint8_t hours, uint32_t seconds, uint8_t fan,
                         uint32_t now) {
    uint8_t error = safety(now);
    if (error)
        fault(error, now);
    else {
        storage_cycle_t cycle = {seconds, target, hours, true, fan, false};
        if (!storage_save(&cycle)) {
            fault(5u, now);
            return;
        }
        /* Commit the complete new cycle first. A power cut while merely
         * browsing/editing must still restore the previous working cycle. */
        resume_pending = false;
        state.target_c = target;
        state.hours = hours;
        state.fan_percent = fan;
        state.error = 0u;
        state.mode = MODE_DRY;
        state.menu = MENU_NONE;
        state.remaining_s = seconds;
        last_second = now;
        last_checkpoint = now;
        state.time_screen = false;
        last_screen = now;
        plate_latched = false;
        dirty = true;
    }
}
/* POWER and M distinguish a release from a 2s hold. A held M must not first
 * execute its short action (which would close the selected profile).
 * UP acts once on a debounced press; boot-held keys must be released first. */
static unsigned key_event(unsigned id, uint32_t now) {
    key_t *k = &keys[id];
    bool pressed = hal_key(id);
    unsigned event = 0u;
    if (pressed != k->raw) {
        k->raw = pressed;
        k->changed = now;
    }
    if (k->stable != k->raw && elapsed(now, k->changed, SH01_KEY_DEBOUNCE_MS)) {
        k->stable = k->raw;
        if (k->stable) {
            k->pressed = now;
            k->long_sent = false;
            if (id == 2u && k->armed)
                event = 1u;
        } else {
            if (id != 2u && k->armed && !k->long_sent)
                event = elapsed(k->changed, k->pressed, SH01_KEY_HOLD_MS) ? 2u : 1u;
            k->armed = true;
        }
    }
    if (id != 2u && k->armed && k->stable && k->raw && !k->long_sent &&
        elapsed(now, k->pressed, SH01_KEY_HOLD_MS)) {
        k->long_sent = true;
        event = 2u;
    }
    return event;
}
static void close_menu(uint32_t now) {
    state.menu = MENU_NONE;
    last_screen = now;
    dirty = true;
}
static void open_profiles(void) {
    state.profile = 0u;
    state.menu = MENU_PROFILE;
}
static void mode_key(bool held, uint32_t now) {
    if (state.menu == MENU_NONE) {
        /* A hold begun on the main screen can only open the list, never
         * enter CUSt or launch a cycle with the same physical press. */
        open_profiles();
    } else if (state.menu == MENU_PROFILE) {
        if (!held)
            close_menu(now);
        else if (state.profile == 0u) {
            edit_target = state.target_c;
            edit_hours = state.hours;
            edit_seconds = state.remaining_s ? state.remaining_s : (uint32_t)edit_hours * 3600u;
            edit_fan = state.fan_percent;
            state.menu = MENU_TEMP;
        } else {
            const drying_profile_t *p = profile_get(state.profile);
            begin_drying(p->target_c, p->hours, (uint32_t)p->hours * 3600u, 100u, now);
        }
    } else if (!held) {
        if (state.menu == MENU_TEMP)
            state.menu = MENU_TIME;
        else if (state.menu == MENU_TIME)
            state.menu = MENU_FAN;
        else if (state.menu == MENU_FAN)
            begin_drying(edit_target, edit_hours, edit_seconds, edit_fan, now);
    }
}
static void key_action(unsigned key, bool held, uint32_t now) {
    if (state.mode == MODE_OFF) {
        if (key == 0u)
            wake(now);
        return;
    }
    hal_click();
    dirty = true;
    last_edit = now;
    if (key == 0u) {
        target_view = false;
        show_error(now);
    } else if (key == 1u) {
        target_view = false;
        error_view = false;
        mode_key(held, now);
    } else if (key == 2u) {
        error_view = false;
        if (state.menu == MENU_NONE) {
            target_view = true;
            target_view_at = now;
        } else if (state.menu == MENU_PROFILE)
            state.profile = (uint8_t)((state.profile + 1u) % SH01_PROFILE_COUNT);
        else if (state.menu == MENU_TEMP)
            edit_target = edit_target >= SH01_TEMP_MAX_C
                              ? SH01_TEMP_MIN_C
                              : (uint8_t)(edit_target + SH01_TEMP_STEP_C);
        else if (state.menu == MENU_TIME) {
            edit_hours = next_time_hours(edit_seconds);
            edit_seconds = (uint32_t)edit_hours * 3600u;
        } else if (state.menu == MENU_FAN) {
            edit_fan = edit_fan >= 100u ? 10u : (uint8_t)(edit_fan + 10u);
        }
    }
}
static void render(uint32_t now) {
    if (state.mode == MODE_OFF)
        return;
    uint16_t mask = current_errors(now);
    if (mask != error_mask) {
        if (mask & (uint16_t)~error_mask)
            hal_error_signal();
        error_mask = mask;
        error_blink_at = now;
    }
    update_leds(now);
    if (error_view && elapsed(now, error_view_at, SH01_ERROR_VIEW_MS)) {
        error_view = false;
        last_screen = now;
        last_edit = now;
        dirty = true;
    }
    if (target_view && elapsed(now, target_view_at, SH01_TARGET_VIEW_MS)) {
        target_view = false;
        last_screen = now;
        dirty = true;
    }
    if (!error_view && !target_view && state.menu == MENU_NONE &&
        (state.mode == MODE_VIEW || state.mode == MODE_DRY || state.mode == MODE_FAULT) &&
        elapsed(now, last_screen, SH01_SCREEN_INTERVAL_MS)) {
        uint32_t pages = (uint32_t)(now - last_screen) / SH01_SCREEN_INTERVAL_MS;
        last_screen += pages * SH01_SCREEN_INTERVAL_MS;
        if (pages & 1u)
            state.time_screen = !state.time_screen;
        dirty = true;
    }
    if (!dirty && !elapsed(now, last_lcd, 200u))
        return;
    dirty = false;
    last_lcd = now;
    if (error_view)
        lcd_error(displayed_error);
    else if (target_view)
        lcd_set_temperature(state.target_c);
    else if (state.menu == MENU_PROFILE)
        lcd_profile(profile_get(state.profile)->name);
    else if (state.menu == MENU_TEMP)
        lcd_set_temperature(edit_target);
    else if (state.menu == MENU_TIME)
        lcd_time(edit_seconds, true, false);
    else if (state.menu == MENU_FAN)
        lcd_fan_percent(edit_fan);
    else if (state.time_screen)
        lcd_time(state.remaining_s, false, state.mode == MODE_DRY);
    else if (sensor_ok(now)) {
        const sensor_sample_t *s = sensor_get();
        lcd_temperature_humidity(s->temperature_x10, s->humidity_x10, state.mode == MODE_DRY);
    } else
        lcd_missing();
}
void controller_init(uint32_t now) {
    unsigned i;
    storage_cycle_t restored;
    state = (controller_state_t){0};
    state.mode = MODE_VIEW;
    state.target_c = SH01_TEMP_MIN_C;
    state.hours = SH01_TIME_DEFAULT_H;
    state.fan_percent = 100u;
    state.remaining_s = (uint32_t)state.hours * 3600u;
    storage_init();
    resume_pending = false;
    if (storage_load(&restored)) {
        state.target_c = restored.target_c;
        state.hours = restored.hours;
        state.fan_percent = restored.fan_percent;
        state.remaining_s = restored.remaining_s;
        if (restored.powered_off)
            state.mode = MODE_OFF;
        resume_pending = restored.running;
    } else {
        storage_cycle_t defaults = {state.remaining_s, state.target_c, state.hours, false,
                                    state.fan_percent, false};
        if (!storage_save(&defaults)) {
            state.mode = MODE_FAULT;
            state.error = 5u;
        }
    }
    resume_started = last_checkpoint = now;
    error_view = false;
    target_view = false;
    error_mask = 0u;
    displayed_error = 0u;
    error_view_at = error_blink_at = now;
    target_view_at = now;
    edit_seconds = state.remaining_s;
    edit_target = state.target_c;
    edit_hours = state.hours;
    edit_fan = state.fan_percent;
    last_edit = now;
    for (i = 0u; i < 3u; ++i) {
        keys[i] = (key_t){0};
        keys[i].raw = keys[i].stable = hal_key(i);
        keys[i].armed = !keys[i].raw;
        keys[i].changed = now;
    }
    last_second = last_lcd = next_adc = last_screen = now;
    dirty = true;
    adc_checked = false;
    plate_ok = false;
    plate_latched = false;
    outputs(false, false);
    hal_sound_stop();
    lcd_enable(state.mode != MODE_OFF);
    update_leds(now);
    render(now);
}
void controller_task(uint32_t now) {
    unsigned i;
    unsigned event[3];
    for (i = 0u; i < 3u; ++i) {
        event[i] = key_event(i, now);
    }
    if (event[0] == 2u) {
        if (state.mode == MODE_OFF) {
            wake(now);
            render(now);
        } else
            off(true);
        return;
    }
    if (due(now, next_adc)) {
        plate_ok = hal_adc(&plate_adc);
        adc_checked = true;
        next_adc = now + 250u;
    }
    for (i = 0u; i < 3u; ++i)
        if (event[i]) {
            key_action(i, event[i] == 2u, now);
            break;
        }
    if (resume_pending) {
        if (plate_ok && sensor_ok(now)) {
            uint8_t error = safety(now);
            resume_pending = false;
            if (error)
                fault(error, now);
            else {
                state.mode = MODE_DRY;
                last_second = last_checkpoint = now;
                state.time_screen = false;
                last_screen = now;
                dirty = true;
            }
        } else if (elapsed(now, resume_started, SH01_RESUME_WAIT_MS)) {
            fault(safety(now), now);
        }
    }
    /* Abandoning any page cancels only the UI draft. Never start a heater
     * on timeout, pause a running cycle, or time out a key still being held. */
    if (keys[1].raw || keys[2].raw)
        last_edit = now;
    if (!error_view && state.menu != MENU_NONE &&
        elapsed(now, last_edit, SH01_MENU_TIMEOUT_MS))
        close_menu(now);
    if (state.mode == MODE_DRY) {
        uint8_t error = safety(now);
        uint32_t seconds;
        if (error) {
            fault(error, now);
            render(now);
            return;
        }
        seconds = (uint32_t)(now - last_second) / 1000u;
        last_second += seconds * 1000u;
        state.remaining_s = seconds >= state.remaining_s ? 0u : state.remaining_s - seconds;
        if (!state.remaining_s) {
            off(false);
            return;
        }
        if (elapsed(now, last_checkpoint, SH01_CHECKPOINT_MS)) {
            storage_cycle_t cycle = {state.remaining_s, state.target_c, state.hours, true,
                                     state.fan_percent, false};
            if (!storage_save(&cycle)) {
                fault(5u, now);
                render(now);
                return;
            }
            last_checkpoint = now;
        }
        if (plate_adc < SH01_PLATE_OFF_ADC_X10)
            plate_latched = true;
        else if (plate_adc >= SH01_PLATE_ON_ADC_X10)
            plate_latched = false;
        /* Preserve the factory integer-C comparison; no invented PID. */
        outputs(sensor_get()->temperature_x10 / 10 <= (int16_t)state.target_c && !plate_latched,
                true);
    } else
        outputs(false, false);
    render(now);
}
const controller_state_t *controller_get(void) { return &state; }

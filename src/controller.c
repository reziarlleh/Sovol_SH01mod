#include "controller.h"
#include "config.h"
#include "hal.h"
#include "lcd.h"
#include "sensor.h"
#include "storage.h"
typedef struct {
    bool raw, stable, armed, long_sent;
    uint32_t changed, pressed;
} key_t;
static key_t keys[3];
static controller_state_t state;
static uint32_t last_second, last_lcd, next_adc, beep_until, last_screen;
static uint32_t edit_seconds;
static uint32_t last_edit;
static uint8_t edit_target;
static bool time_changed;
static bool dirty, beeping, plate_ok, plate_latched;
static bool resume_pending;
static bool error_view;
static uint8_t displayed_error;
static uint16_t error_mask;
static uint32_t error_view_at, error_blink_at;
static uint32_t resume_started, last_checkpoint;
static uint16_t plate_adc;
static const uint8_t time_presets_h[] = {SH01_TIME_PRESET_VALUES};
/* While editing a running cycle, choose the next offered duration above the
 * remaining time, including a partial hour; the last preset wraps to six. */
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
    hal_fan(fan);
}
static void beep(uint32_t now) {
    beeping = true;
    beep_until = now + 50u;
    hal_beep(true);
}
/* One owner for LED state; LCD refresh must never pulse the button lights. */
static void update_leds(uint32_t now) {
    bool enabled = state.mode != MODE_OFF;
    bool blink_on =
        !error_mask || ((uint32_t)(now - error_blink_at) / SH01_ERROR_BLINK_MS) % 2u == 0u;
    hal_led(0u, enabled && blink_on);
    hal_led(1u, enabled && (state.mode == MODE_TEMP || state.mode == MODE_TIME || keys[1].stable));
    hal_led(2u, enabled && keys[2].stable);
}
static void off(void) {
    outputs(false, false);
    resume_pending = false;
    hal_beep(false);
    beeping = false;
    state.mode = MODE_OFF;
    state.error = storage_disarm() ? 0u : 5u;
    error_view = false;
    error_mask = 0u;
    update_leds(0u);
    lcd_enable(false);
}
static void fault(uint8_t error, uint32_t now) {
    outputs(false, false);
    resume_pending = false;
    if (!storage_disarm())
        error = 5u;
    state.error = error;
    state.mode = MODE_FAULT;
    if (error_view) {
        displayed_error = error;
        error_view_at = now;
    }
    dirty = true;
    beep(now);
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
    if (!plate_ok || plate_adc >= 37200u || plate_adc < 100u)
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
static void begin_drying(uint32_t now) {
    uint8_t error = safety(now);
    if (error)
        fault(error, now);
    else {
        storage_cycle_t cycle = {edit_seconds, state.target_c, state.hours, true};
        if (!storage_save(&cycle)) {
            fault(5u, now);
            return;
        }
        state.mode = MODE_DRY;
        state.remaining_s = edit_seconds;
        last_second = now;
        last_checkpoint = now;
        state.time_screen = false;
        last_screen = now;
        plate_latched = false;
        dirty = true;
    }
}
/* POWER short is emitted on release; its 2s hold has one global OFF action.
 * M/UP act once on a debounced press, including when held longer. */
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
            if (id != 0u && k->armed)
                event = 1u;
        } else {
            if (id == 0u && k->armed && !k->long_sent)
                event = elapsed(k->changed, k->pressed, SH01_KEY_HOLD_MS) ? 2u : 1u;
            k->armed = true;
        }
    }
    if (id == 0u && k->armed && k->stable && k->raw && !k->long_sent &&
        elapsed(now, k->pressed, SH01_KEY_HOLD_MS)) {
        k->long_sent = true;
        event = 2u;
    }
    return event;
}
static void short_key(unsigned key, uint32_t now) {
    if (resume_pending && key != 0u) {
        resume_pending = false;
        if (!storage_disarm()) {
            fault(5u, now);
            return;
        }
    }
    if (state.mode == MODE_OFF) {
        if (key == 0u) {
            state.mode = state.error == 5u ? MODE_FAULT : MODE_VIEW;
            state.time_screen = false;
            last_screen = now;
            lcd_enable(true);
            dirty = true;
        }
        return;
    }
    beep(now);
    dirty = true;
    last_edit = now;
    if (key == 0u) {
        show_error(now);
    } else if (key == 1u) {
        error_view = false;
        if (state.mode == MODE_FAULT) {
            if (safety(now) || !storage_disarm()) {
                show_error(now);
                return;
            }
            state.error = 0u;
        }
        if (state.mode == MODE_VIEW || state.mode == MODE_DRY || state.mode == MODE_FAULT) {
            edit_seconds = state.mode == MODE_DRY || state.mode == MODE_FAULT
                               ? state.remaining_s
                               : (uint32_t)state.hours * 3600u;
            edit_target = state.target_c;
            time_changed = false;
            outputs(false, false);
            if (state.mode == MODE_DRY && !storage_disarm()) {
                fault(5u, now);
                return;
            }
            state.mode = MODE_TEMP;
        } else if (state.mode == MODE_TEMP) {
            state.target_c = edit_target;
            state.mode = MODE_TIME;
        } else if (state.mode == MODE_TIME) {
            begin_drying(now);
        }
    } else if (key == 2u) {
        error_view = false;
        if (state.mode == MODE_TEMP)
            edit_target = edit_target >= SH01_TEMP_MAX_C
                              ? SH01_TEMP_MIN_C
                              : (uint8_t)(edit_target + SH01_TEMP_STEP_C);
        else if (state.mode == MODE_TIME) {
            state.hours = next_time_hours(edit_seconds);
            edit_seconds = (uint32_t)state.hours * 3600u;
            time_changed = true;
        }
    }
}
static void render(uint32_t now) {
    if (state.mode == MODE_OFF)
        return;
    uint16_t mask = current_errors(now);
    if (mask != error_mask) {
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
    if (!error_view &&
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
    else if (state.mode == MODE_TEMP)
        lcd_set_temperature(edit_target);
    else if (state.mode == MODE_TIME)
        lcd_time(edit_seconds, true, false);
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
    state.hours = SH01_TIME_MIN_H;
    state.remaining_s = (uint32_t)state.hours * 3600u;
    storage_init();
    resume_pending = false;
    if (storage_load(&restored)) {
        state.target_c = restored.target_c;
        state.hours = restored.hours;
        state.remaining_s =
            restored.running ? restored.remaining_s : (uint32_t)restored.hours * 3600u;
        resume_pending = restored.running;
    } else {
        storage_cycle_t defaults = {state.remaining_s, state.target_c, state.hours, false};
        if (!storage_save(&defaults)) {
            state.mode = MODE_FAULT;
            state.error = 5u;
        }
    }
    resume_started = last_checkpoint = now;
    error_view = false;
    error_mask = 0u;
    displayed_error = 0u;
    error_view_at = error_blink_at = now;
    edit_seconds = state.remaining_s;
    edit_target = state.target_c;
    time_changed = false;
    last_edit = now;
    for (i = 0u; i < 3u; ++i) {
        keys[i] = (key_t){0};
        keys[i].raw = keys[i].stable = hal_key(i);
        keys[i].armed = !keys[i].raw;
        keys[i].changed = now;
    }
    last_second = last_lcd = next_adc = last_screen = now;
    dirty = true;
    beeping = false;
    plate_ok = false;
    plate_latched = false;
    outputs(false, false);
    hal_beep(false);
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
            state.mode = state.error == 5u ? MODE_FAULT : MODE_VIEW;
            last_screen = now;
            lcd_enable(true);
            dirty = true;
            render(now);
        } else
            off();
        return;
    }
    if (beeping && due(now, beep_until)) {
        beeping = false;
        hal_beep(false);
    }
    if (due(now, next_adc)) {
        plate_ok = hal_adc(&plate_adc);
        next_adc = now + 250u;
    }
    for (i = 0u; i < 3u; ++i)
        if (event[i] == 1u) {
            short_key(i, now);
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
    /* Menu timeout: abandoned TEMP/TIME returns to view;
     * changing TIME permits automatic start after five seconds. */
    if (!error_view && (state.mode == MODE_TEMP || state.mode == MODE_TIME) &&
        elapsed(now, last_edit, SH01_MENU_TIMEOUT_MS)) {
        if (state.mode == MODE_TIME && time_changed)
            begin_drying(now);
        else {
            state.mode = MODE_VIEW;
            state.time_screen = false;
            last_screen = now;
            dirty = true;
        }
    }
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
            off();
            return;
        }
        if (elapsed(now, last_checkpoint, SH01_CHECKPOINT_MS)) {
            storage_cycle_t cycle = {state.remaining_s, state.target_c, state.hours, true};
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

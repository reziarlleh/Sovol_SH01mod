#include "controller.h"
#include "config.h"
#include "hal.h"
#include "lcd.h"
#include "sensor.h"
typedef struct {
    bool raw, stable, armed, long_sent;
    uint32_t changed, pressed;
} key_t;
static key_t keys[3];
static controller_state_t state;
static uint32_t last_second, last_lcd, next_adc, beep_until;
static uint32_t edit_seconds;
static uint32_t last_edit;
static uint8_t edit_target;
static bool time_changed;
static bool dirty, beeping, plate_ok, plate_latched;
static uint16_t plate_adc;
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
static void off(void) {
    outputs(false, false);
    hal_beep(false);
    beeping = false;
    state.mode = MODE_OFF;
    state.error = 0u;
    hal_led(0u, false);
    hal_led(1u, false);
    hal_led(2u, false);
    lcd_enable(false);
}
static void fault(uint8_t error, uint32_t now) {
    outputs(false, false);
    state.error = error;
    state.mode = MODE_FAULT;
    dirty = true;
    beep(now);
}
static uint8_t safety(uint32_t now) {
    const sensor_sample_t *s = sensor_get();
    if (!sensor_ok(now))
        return 1u;
    if (!plate_ok || plate_adc >= 37200u || plate_adc < 100u)
        return 2u;
    if (s->temperature_x10 <= -100)
        return 3u;
    if (s->temperature_x10 >= SH01_CHAMBER_FAULT_X10)
        return 4u;
    return 0u;
}
static void begin_drying(uint32_t now) {
    uint8_t error = safety(now);
    if (error)
        fault(error, now);
    else {
        state.mode = MODE_DRY;
        state.remaining_s = edit_seconds;
        last_second = now;
        state.time_screen = false;
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
    if (state.mode == MODE_OFF) {
        if (key == 0u) {
            state.mode = MODE_VIEW;
            state.time_screen = false;
            lcd_enable(true);
            dirty = true;
        }
        return;
    }
    beep(now);
    dirty = true;
    last_edit = now;
    if (key == 0u) {
        if (state.mode == MODE_FAULT) {
            if (safety(now) == 0u) {
                state.error = 0u;
                state.mode = MODE_VIEW;
            }
        } else
            state.time_screen = !state.time_screen;
    } else if (key == 1u && state.mode != MODE_FAULT) {
        if (state.mode == MODE_VIEW || state.mode == MODE_DRY) {
            edit_seconds =
                state.mode == MODE_DRY ? state.remaining_s : (uint32_t)state.hours * 3600u;
            edit_target = state.target_c;
            time_changed = false;
            outputs(false, false);
            state.mode = MODE_TEMP;
        } else if (state.mode == MODE_TEMP) {
            state.target_c = edit_target;
            state.mode = MODE_TIME;
        } else if (state.mode == MODE_TIME) {
            begin_drying(now);
        }
    } else if (key == 2u) {
        if (state.mode == MODE_TEMP)
            edit_target = edit_target >= SH01_TEMP_MAX_C
                              ? SH01_TEMP_MIN_C
                              : (uint8_t)(edit_target + SH01_TEMP_STEP_C);
        else if (state.mode == MODE_TIME) {
            uint32_t hours = edit_seconds / 3600u;
            hours = hours >= SH01_TIME_MAX_H ? SH01_TIME_MIN_H : hours + 1u;
            if (hours < SH01_TIME_MIN_H)
                hours = SH01_TIME_MIN_H;
            state.hours = (uint8_t)hours;
            edit_seconds = hours * 3600u;
            time_changed = true;
        }
    }
}
static void render(uint32_t now) {
    const sensor_sample_t *s = sensor_get();
    if (state.mode == MODE_OFF)
        return;
    if (!dirty && !elapsed(now, last_lcd, 200u))
        return;
    dirty = false;
    last_lcd = now;
    hal_led(0u, true);
    hal_led(1u, state.mode == MODE_TEMP || state.mode == MODE_TIME);
    if (state.mode == MODE_FAULT)
        lcd_error(state.error);
    else if (state.mode == MODE_TEMP)
        lcd_set_temperature(edit_target);
    else if (state.mode == MODE_TIME)
        lcd_time(edit_seconds, true, false);
    else if (state.time_screen)
        lcd_time(state.remaining_s, false, state.mode == MODE_DRY);
    else if (sensor_ok(now))
        lcd_temperature_humidity(s->temperature_x10, s->humidity_x10, state.mode == MODE_DRY);
    else
        lcd_missing();
}
void controller_init(uint32_t now) {
    unsigned i;
    state = (controller_state_t){0};
    state.mode = MODE_VIEW;
    state.target_c = SH01_TEMP_MIN_C;
    state.hours = SH01_TIME_MIN_H;
    state.remaining_s = (uint32_t)state.hours * 3600u;
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
    last_second = last_lcd = next_adc = now;
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
        hal_led(i, keys[i].stable && state.mode != MODE_OFF);
    }
    if (event[0] == 2u) {
        if (state.mode == MODE_OFF) {
            state.mode = MODE_VIEW;
            lcd_enable(true);
            dirty = true;
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
    /* Menu timeout: abandoned TEMP/TIME returns to view;
     * changing TIME permits automatic start after five seconds. */
    if ((state.mode == MODE_TEMP || state.mode == MODE_TIME) &&
        elapsed(now, last_edit, SH01_MENU_TIMEOUT_MS)) {
        if (state.mode == MODE_TIME && time_changed)
            begin_drying(now);
        else {
            state.mode = MODE_VIEW;
            state.time_screen = false;
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

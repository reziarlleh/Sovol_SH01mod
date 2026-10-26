#include "actuators.h"
#include "config.h"

static volatile uint8_t fan_steps, phase, sound_phase;
static volatile uint16_t boost_left, sound_left;
static volatile bool fan_high;

static void fan_level(bool high) {
    if (fan_high != high) {
        fan_high = high;
        hal_fan_drive(high);
    }
}
void actuators_fan(uint8_t percent) {
    uint8_t steps = percent >= 100u ? 10u : percent / 10u;
    if (steps == fan_steps)
        return;
    if (!fan_steps && steps) {
        phase = 0u;
        boost_left = SH01_FAN_START_MS;
    }
    fan_steps = steps;
    if (!steps)
        boost_left = 0u;
    fan_level(steps && (boost_left || phase < steps));
}
void actuators_silence(void) {
    sound_left = 0u;
    sound_phase = 0u;
    hal_tone_drive(0u);
}
void actuators_click(void) {
    if (sound_phase >= 2u)
        return; /* A key cannot truncate the error signal. */
    sound_phase = 1u;
    sound_left = 50u;
    hal_tone_drive(UINT16_MAX);
}
void actuators_error_signal(void) {
    if (sound_phase >= 2u)
        return; /* Coalesce errors appearing within the same signal. */
    sound_phase = 2u;
    sound_left = 150u;
    hal_tone_drive(1000u);
}
void actuators_tick_1ms(void) {
    if (fan_steps) {
        if (boost_left)
            --boost_left;
        else
            phase = (uint8_t)((phase + 1u) % 10u);
        fan_level(boost_left || phase < fan_steps);
    }
    if (!sound_left || --sound_left)
        return;
    if (sound_phase == 2u) {
        sound_phase = 3u;
        sound_left = 50u;
        hal_tone_drive(0u);
    } else if (sound_phase == 3u) {
        sound_phase = 4u;
        sound_left = 150u;
        hal_tone_drive(2000u);
    } else
        actuators_silence();
}

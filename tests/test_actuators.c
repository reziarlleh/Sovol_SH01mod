#include "actuators.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, tone_changes;
static bool fan;
static uint16_t tone;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL actuators:%u %s\n", __LINE__, #x);                               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void hal_fan_drive(bool high) { fan = high; }
void hal_tone_drive(uint16_t hz) {
    tone = hz;
    ++tone_changes;
}
static void ticks(unsigned count) {
    while (count--)
        actuators_tick_1ms();
}
static void test_pwm(void) {
    for (unsigned percent = 10u; percent <= 100u; percent += 10u) {
        actuators_fan(0u);
        CHECK(!fan);
        actuators_fan((uint8_t)percent);
        CHECK(fan);
        for (unsigned i = 0u; i < SH01_FAN_START_MS; ++i) {
            CHECK(fan);
            actuators_fan((uint8_t)percent); /* Main loop calls do not restart boost. */
            actuators_tick_1ms();
        }
        unsigned high = 0u;
        for (unsigned i = 0u; i < 1000u; ++i) {
            actuators_fan((uint8_t)percent);
            if (fan)
                ++high;
            actuators_tick_1ms();
        }
        CHECK(high == percent * 10u);
    }
    actuators_fan(20u); /* Changing a running fan must not force another boost. */
    unsigned high = 0u;
    for (unsigned i = 0u; i < 1000u; ++i) {
        if (fan)
            ++high;
        actuators_tick_1ms();
    }
    CHECK(high == 200u);
    actuators_fan(0u);
    CHECK(!fan);
    ticks(1000u);
    CHECK(!fan);
}
static void test_sound(void) {
    actuators_silence();
    CHECK(tone == 0u);
    actuators_click();
    CHECK(tone == UINT16_MAX);
    ticks(49u);
    CHECK(tone == UINT16_MAX);
    ticks(1u);
    CHECK(tone == 0u);
    actuators_click();
    actuators_error_signal();
    CHECK(tone == 1000u);
    ticks(149u);
    CHECK(tone == 1000u);
    unsigned changes = tone_changes;
    actuators_click();
    actuators_error_signal();
    CHECK(tone_changes == changes && tone == 1000u);
    ticks(1u);
    CHECK(tone == 0u);
    ticks(49u);
    CHECK(tone == 0u);
    actuators_click();
    CHECK(tone == 0u);
    ticks(1u);
    CHECK(tone == 2000u);
    ticks(149u);
    CHECK(tone == 2000u);
    ticks(1u);
    CHECK(tone == 0u);
    changes = tone_changes;
    ticks(10000u);
    CHECK(tone_changes == changes && tone == 0u);
    actuators_error_signal();
    ticks(10u);
    actuators_silence();
    CHECK(tone == 0u);
    changes = tone_changes;
    ticks(10000u);
    CHECK(tone_changes == changes);
}
int main(void) {
    test_pwm();
    test_sound();
    printf("actuators: PWM, start boost, immediate OFF, two tones, no repeat: %u checks passed\n",
           checks);
    return 0;
}

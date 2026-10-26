#include "config.h"
#include "sensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, init_count, trigger_count, frames, recoveries, unsupported;
static uint32_t now, init_at, trigger_at;
static bool nack, always_busy, bad_frame, calibration_stuck, calibrated = true;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL sensor:%u %s\n", __LINE__, #x);                                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void bus_recover(void) { ++recoveries; }
bus_error_t bus_last_error(void) { return nack ? BUS_ADDRESS_NACK : BUS_OK; }
bool bus_write(uint8_t addr, const uint8_t *bytes, size_t length) {
    if (addr >= 0x44u && addr <= 0x46u)
        return false; /* This regression fixture has only its stock AHT20. */
    CHECK(addr == 0x38u && length == 3u);
    if (nack)
        return false;
    if (bytes[0] == 0xBEu) {
        CHECK(bytes[1] == 8u && bytes[2] == 0u);
        ++init_count;
        init_at = now;
    } else if (bytes[0] == 0xACu) {
        CHECK(bytes[1] == 0x33u && bytes[2] == 0u && calibrated);
        ++trigger_count;
        trigger_at = now;
    } else {
        /* A device is allowed to NACK commands absent from its datasheet. */
        ++unsupported;
        return false;
    }
    return true;
}
bool bus_read(uint8_t addr, uint8_t *bytes, size_t length) {
    if (addr >= 0x44u && addr <= 0x46u)
        return false;
    CHECK(addr == 0x38u && (length == 1u || length == 6u));
    if (nack)
        return false;
    bool busy = always_busy || (trigger_count && (uint32_t)(now - trigger_at) < 200u);
    if (!calibrated && !calibration_stuck && init_count && (uint32_t)(now - init_at) >= 350u)
        calibrated = true;
    if (length == 1u) {
        bytes[0] = (uint8_t)((busy ? 0x80u : 0u) | (calibrated ? 0x18u : 0u));
        return true;
    }
    CHECK(!busy);
    ++frames;
    memset(bytes, 0, length);
    if (!bad_frame) {
        uint32_t rh = 471860u, t = 393216u; /* 45.0% RH, 25.0C */
        bytes[0] = 8u;
        bytes[1] = (uint8_t)(rh >> 12);
        bytes[2] = (uint8_t)(rh >> 4);
        bytes[3] = (uint8_t)((rh << 4) | (t >> 16));
        bytes[4] = (uint8_t)(t >> 8);
        bytes[5] = (uint8_t)t;
    }
    return true;
}
static void run(uint32_t start, uint32_t duration) {
    for (uint32_t i = 0u; i <= duration; ++i) {
        now = start + i;
        sensor_task(now);
    }
}
static void reset(uint32_t epoch) {
    init_count = trigger_count = frames = unsupported = 0u;
    calibrated = true;
    nack = always_busy = bad_frame = calibration_stuck = false;
    now = epoch;
    sensor_init(now);
}
int main(void) {
    reset(0u);
    run(0u, 800u);
    CHECK(recoveries == 1u && unsupported == 0u && init_count == 0u && trigger_count == 1u &&
          frames == 1u);
    CHECK(sensor_ok(now));
    CHECK(sensor_get()->temperature_x10 == 250);
    CHECK(sensor_get()->humidity_x10 == 450u);
    uint32_t good = sensor_get()->last_good_ms;
    CHECK(sensor_ok(good + 3000u) && !sensor_ok(good + 3001u));
    run(801u, 1200u);
    CHECK(frames == 2u && unsupported == 0u);
    reset(0u);
    calibrated = false;
    run(0u, 900u);
    CHECK(sensor_ok(now) && init_count == 1u && unsupported == 0u && frames == 1u);
    reset(0u);
    nack = true;
    run(0u, 1100u);
    CHECK(!sensor_ok(now) && sensor_get()->failures == 2u);
    nack = false;
    run(1101u, 1900u);
    CHECK(sensor_ok(now));
    reset(0u);
    always_busy = true;
    run(0u, 1000u);
    CHECK(frames == 0u && sensor_get()->failures == 1u);
    reset(0u);
    calibrated = false;
    calibration_stuck = true;
    run(0u, 1000u);
    CHECK(init_count == 1u && frames == 0u && !sensor_ok(now));
    reset(0u);
    bad_frame = true;
    run(0u, 800u);
    CHECK(frames == 1u && !sensor_ok(now));
    reset(0xFFFFFE00u);
    run(0xFFFFFE00u, 800u);
    CHECK(sensor_ok(now) && frames == 1u);
    printf("sensor: delayed calibration, BUSY, frame, NACK, timeout, stale, "
           "rollover: %u "
           "checks passed\n",
           checks);
    return 0;
}

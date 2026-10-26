#include "sensor.h"
#include "config.h"
typedef enum { POWER_WAIT, NORMAL_WAIT, INIT_WAIT, IDLE, MEASURE_WAIT } state_t;
static state_t state;
static uint32_t due_at, started_at;
static sensor_sample_t sample;
static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static void retry(uint32_t now) {
    sample.ready = false;
    if (sample.failures < 255u)
        ++sample.failures;
    state = POWER_WAIT;
    due_at = now + 1000u;
}
static bool status_read(void) { return bus_read(0x38u, &sample.last_status, 1u); }
void sensor_init(uint32_t now) {
    sample = (sensor_sample_t){0};
    state = POWER_WAIT;
    due_at = now + 100u;
    started_at = now;
    bus_recover();
}
void sensor_task(uint32_t now) {
    static const uint8_t normal[3] = {0xA8u, 0u, 0u};
    static const uint8_t init[3] = {0xE1u, 8u, 0u};
    static const uint8_t measure[3] = {0xACu, 0x33u, 0u};
    if (!due(now, due_at))
        return;
    switch (state) {
    case POWER_WAIT:
        if (!bus_write(0x38u, normal, sizeof normal)) {
            retry(now);
            break;
        }
        state = NORMAL_WAIT;
        due_at = now + 40u;
        break;
    case NORMAL_WAIT:
        if (!bus_write(0x38u, init, sizeof init)) {
            retry(now);
            break;
        }
        state = INIT_WAIT;
        started_at = now;
        due_at = now + 20u;
        break;
    case INIT_WAIT:
        if (!status_read()) {
            retry(now);
            break;
        }
        if (sample.last_status & 0x80u) {
            if ((uint32_t)(now - started_at) >= SH01_SENSOR_WAIT_MS)
                retry(now);
            else
                due_at = now + 10u;
        } else {
            state = IDLE;
            due_at = now;
        }
        break;
    case IDLE:
        if (!bus_write(0x38u, measure, sizeof measure)) {
            retry(now);
            break;
        }
        state = MEASURE_WAIT;
        started_at = now;
        due_at = now + 80u;
        break;
    case MEASURE_WAIT:
        if (!status_read()) {
            retry(now);
            break;
        }
        if (sample.last_status & 0x80u) {
            if ((uint32_t)(now - started_at) >= SH01_SENSOR_WAIT_MS)
                retry(now);
            else
                due_at = now + 10u;
        } else {
            uint8_t bytes[6];
            uint32_t rh, t;
            int32_t t10;
            if (!bus_read(0x38u, bytes, sizeof bytes) || (bytes[0] & 0x80u)) {
                retry(now);
                break;
            }
            rh = ((uint32_t)bytes[1] << 12) | ((uint32_t)bytes[2] << 4) | (bytes[3] >> 4);
            t = ((uint32_t)(bytes[3] & 15u) << 16) | ((uint32_t)bytes[4] << 8) | bytes[5];
            t10 = (int32_t)((t * 2000u) >> 20) - 500;
            if (t10 < -400 || t10 > 1250) {
                retry(now);
                break;
            }
            sample.temperature_x10 = (int16_t)t10;
            sample.humidity_x10 = (uint16_t)((rh * 1000u) >> 20);
            sample.last_status = bytes[0];
            sample.last_good_ms = now;
            sample.ready = true;
            sample.failures = 0u;
            state = IDLE;
            due_at = now + SH01_SENSOR_PERIOD_MS;
        }
        break;
    }
}
const sensor_sample_t *sensor_get(void) { return &sample; }
bool sensor_ok(uint32_t now) {
    return sample.ready && (uint32_t)(now - sample.last_good_ms) <= SH01_SENSOR_STALE_MS;
}

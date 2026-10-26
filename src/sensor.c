#include "sensor.h"
#include "config.h"
/* AHT20: read status first, initialize with BE only when CAL is clear.
 * 0x71 is the address/read byte produced by bus_read(0x38), not a payload.
 * The six-byte read ends in NACK; the manufacturer's optional CRC is omitted.
 */
typedef enum { POWER_WAIT, INIT_WAIT, IDLE, MEASURE_WAIT } state_t;
static state_t state;
static uint32_t due_at, started_at;
static sensor_sample_t aht_sample;
static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static void retry(uint32_t now) {
    aht_sample.ready = false;
    if (aht_sample.failures < 255u)
        ++aht_sample.failures;
    state = POWER_WAIT;
    due_at = now + 1000u;
    started_at = due_at;
}
static bool status_read(void) { return bus_read(0x38u, &aht_sample.last_status, 1u); }
static void aht_init(uint32_t now) {
    aht_sample = (sensor_sample_t){0};
    state = POWER_WAIT;
    due_at = now + 100u;
    started_at = now;
}
static void aht_task(uint32_t now) {
    static const uint8_t init[3] = {0xBEu, 8u, 0u};
    static const uint8_t measure[3] = {0xACu, 0x33u, 0u};
    if (!due(now, due_at))
        return;
    switch (state) {
    case POWER_WAIT:
        if (!status_read()) {
            retry(now);
            break;
        }
        if (aht_sample.last_status & 0x80u) {
            if ((uint32_t)(now - started_at) >= SH01_SENSOR_WAIT_MS)
                retry(now);
            else
                due_at = now + 10u;
        } else if (aht_sample.last_status & 0x08u) {
            state = IDLE;
            due_at = now + 10u;
        } else if (!bus_write(0x38u, init, sizeof init)) {
            retry(now);
        } else {
            state = INIT_WAIT;
            started_at = now;
            due_at = now + 10u;
        }
        break;
    case INIT_WAIT:
        if (!status_read()) {
            retry(now);
            break;
        }
        if ((aht_sample.last_status & 0x80u) || !(aht_sample.last_status & 0x08u)) {
            if ((uint32_t)(now - started_at) >= SH01_SENSOR_WAIT_MS)
                retry(now);
            else
                due_at = now + 10u;
        } else {
            state = IDLE;
            due_at = now + 10u;
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
        if (aht_sample.last_status & 0x80u) {
            if ((uint32_t)(now - started_at) >= SH01_SENSOR_WAIT_MS)
                retry(now);
            else
                due_at = now + 10u;
        } else {
            uint8_t bytes[6];
            uint32_t rh, t;
            int32_t t10;
            if (!bus_read(0x38u, bytes, sizeof bytes) || (bytes[0] & 0x80u) ||
                !(bytes[0] & 0x08u)) {
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
            aht_sample.temperature_x10 = (int16_t)t10;
            aht_sample.humidity_x10 = (uint16_t)((rh * 1000u) >> 20);
            aht_sample.last_status = bytes[0];
            aht_sample.last_good_ms = now;
            aht_sample.ready = true;
            aht_sample.failures = 0u;
            state = IDLE;
            due_at = now + SH01_SENSOR_PERIOD_MS;
        }
        break;
    }
}

/* SHT4x uses an independent state machine on the same physical bus.
 * Reset/serial commands identify a candidate; both words require CRC.
 * No general-call reset and no on-sensor heater commands are sent. */
typedef enum { SHT_SCAN, SHT_RESET_WAIT, SHT_SERIAL_WAIT, SHT_IDLE, SHT_MEASURE_WAIT } sht_state_t;
static sht_state_t sht_state;
static sensor_sample_t sht_sample;
static uint32_t sht_due;
static uint8_t sht_address;
static uint8_t sht_good_frames;
static bool aht_paused;
static const sensor_sample_t *selected;
static sensor_source_t source;

static bool usable(const sensor_sample_t *s, uint32_t now) {
    return s->ready && (uint32_t)(now - s->last_good_ms) <= SH01_SENSOR_STALE_MS;
}
static uint8_t sht_crc(const uint8_t *bytes) {
    uint8_t crc = 0xFFu;
    for (unsigned i = 0u; i < 2u; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (uint8_t)(((uint32_t)crc << 1) ^ ((crc & 0x80u) ? 0x31u : 0u));
    }
    return crc;
}
static bool sht_frame(uint8_t *frame) {
    return bus_read(sht_address, frame, 6u) && sht_crc(frame) == frame[2] &&
           sht_crc(frame + 3u) == frame[5];
}
static bool sht_command(uint8_t command) { return bus_write(sht_address, &command, 1u); }
static void sht_next_address(uint32_t now) {
    sht_sample.ready = false;
    sht_good_frames = 0u;
    sht_state = SHT_SCAN;
    if (sht_address == 0x46u) {
        sht_address = 0x44u;
        sht_due = now + SH01_SENSOR_PERIOD_MS;
    } else {
        ++sht_address;
        sht_due = now + 2u;
    }
}
static void sht_failed(uint32_t now) {
    sht_good_frames = 0u;
    if (sht_sample.failures < 255u)
        ++sht_sample.failures;
    if (sht_sample.failures >= SH01_SENSOR_CONFIRM_FRAMES) {
        sht_sample.ready = false;
        sht_address = 0x44u;
        sht_state = SHT_SCAN;
    } else
        sht_state = SHT_IDLE;
    sht_due = now + SH01_SENSOR_PERIOD_MS;
}
static void sht_task(uint32_t now) {
    uint8_t frame[6];
    if (!due(now, sht_due))
        return;
    switch (sht_state) {
    case SHT_SCAN:
        if (!sht_command(0x94u))
            sht_next_address(now);
        else {
            sht_state = SHT_RESET_WAIT;
            sht_due = now + 2u;
        }
        break;
    case SHT_RESET_WAIT:
        if (!sht_command(0x89u))
            sht_next_address(now);
        else {
            sht_state = SHT_SERIAL_WAIT;
            sht_due = now + 2u;
        }
        break;
    case SHT_SERIAL_WAIT:
        if (!sht_frame(frame))
            sht_next_address(now);
        else {
            sht_state = SHT_IDLE;
            sht_due = now;
        }
        break;
    case SHT_IDLE:
        if (!sht_command(0xFDu))
            sht_failed(now);
        else {
            sht_state = SHT_MEASURE_WAIT;
            sht_due = now + 10u;
        }
        break;
    case SHT_MEASURE_WAIT:
        if (!sht_frame(frame)) {
            sht_failed(now);
            break;
        }
        uint32_t raw_t = (uint32_t)frame[0] * 256u + frame[1];
        uint32_t raw_rh = (uint32_t)frame[3] * 256u + frame[4];
        int32_t t10 = -450 + (int32_t)((raw_t * 1750u + 32767u) / 65535u);
        int32_t rh10 = -60 + (int32_t)((raw_rh * 1250u + 32767u) / 65535u);
        if (t10 < -400 || t10 > 1250) {
            sht_failed(now);
            break;
        }
        sht_sample.temperature_x10 = (int16_t)t10;
        sht_sample.humidity_x10 = (uint16_t)(rh10 < 0 ? 0 : rh10 > 1000 ? 1000 : rh10);
        sht_sample.last_good_ms = now;
        sht_sample.ready = true;
        sht_sample.failures = 0u;
        if (sht_good_frames < SH01_SENSOR_CONFIRM_FRAMES)
            ++sht_good_frames;
        sht_state = SHT_IDLE;
        sht_due = now + SH01_SENSOR_PERIOD_MS;
        break;
    }
}
static void select_source(uint32_t now) {
    bool primary_ok = usable(&sht_sample, now), reserve_ok = usable(&aht_sample, now);
    if (!primary_ok)
        sht_good_frames = 0u;
    /* A fresh previous primary frame bridges one bad transfer. Once on the
     * reserve, require consecutive good primary frames before returning. */
    if (primary_ok &&
        (source != SENSOR_AHT20 || !reserve_ok || sht_good_frames >= SH01_SENSOR_CONFIRM_FRAMES)) {
        selected = &sht_sample;
        source = SENSOR_SHT4X;
    } else {
        selected = &aht_sample;
        source = reserve_ok ? SENSOR_AHT20 : SENSOR_NONE;
    }
}
void sensor_init(uint32_t now) {
    aht_init(now);
    sht_sample = (sensor_sample_t){0};
    sht_address = 0x44u;
    sht_state = SHT_SCAN;
    sht_due = now + 100u;
    sht_good_frames = 0u;
    aht_paused = false;
    source = SENSOR_NONE;
    select_source(now);
    bus_recover();
}
void sensor_task(uint32_t now) {
    sht_task(now);
    bool need_reserve =
        source != SENSOR_SHT4X || sht_sample.failures != 0u || !usable(&sht_sample, now);
    if (need_reserve) {
        if (aht_paused) {
            /* Discard a conversion that may have completed before pausing:
             * it must never acquire a fresh timestamp hours later. */
            aht_init(now);
            aht_paused = false;
        }
        aht_task(now);
    } else {
        aht_sample.ready = false;
        aht_paused = true;
    }
    select_source(now);
}
const sensor_sample_t *sensor_get(void) { return selected; }
bool sensor_ok(uint32_t now) {
    select_source(now);
    return source != SENSOR_NONE;
}
sensor_source_t sensor_source(void) { return source; }
bool sensor_overheated(uint32_t now) {
    select_source(now);
    return source != SENSOR_NONE && selected->temperature_x10 >= SH01_CHAMBER_FAULT_X10;
}
bool sensor_primary_fault(uint32_t now) {
    select_source(now);
    return source == SENSOR_AHT20 || sht_sample.failures != 0u;
}

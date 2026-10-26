#include "config.h"
#include "hal.h"
#include "sensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now, wire_ms, command_ack_at, aht_ack_at;
static uint8_t command;
static bus_error_t last_error;
static unsigned checks, early_reads, measurements, aht_io, aht_interleaved, recoveries;
static unsigned busy_reads, crc_frames;
static bool missing, stuck, consumed, aht_measuring;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL sensor timing:%u %s (early=%u reserve=%u clock=%lu)\n",          \
                    __LINE__, #x, early_reads, aht_io, (unsigned long)now);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

uint32_t hal_millis(void) { return now; }
bus_error_t bus_last_error(void) { return last_error; }
void bus_recover(void) {
    ++recoveries;
    stuck = false;
    last_error = BUS_OK;
}
static uint8_t crc_word(uint16_t word) {
    uint32_t rem = (uint32_t)(word ^ 0xFF00u) << 8;
    for (int bit = 23; bit >= 8; --bit)
        if (rem & (1ul << bit))
            rem ^= 0x131ul << (bit - 8);
    return (uint8_t)rem;
}
static void word(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
    out[2] = crc_word(value);
}
static bool transaction(uint8_t address) {
    last_error = BUS_OK;
    if (address == 0x38u) {
        ++aht_io;
        if ((command == 0xFDu || command == 0x89u) && !consumed)
            ++aht_interleaved;
    }
    if (stuck) {
        last_error = BUS_SDA_LOW;
        now += wire_ms;
        return false;
    }
    if ((address != 0x38u && address != 0x44u) || (address == 0x44u && missing)) {
        last_error = BUS_ADDRESS_NACK;
        now += wire_ms;
        return false;
    }
    return true;
}
bool bus_write(uint8_t address, const uint8_t *bytes, size_t length) {
    if (!transaction(address))
        return false;
    now += wire_ms; /* ACK/STOP, not function entry, starts the sensor's clock. */
    if (address == 0x38u) {
        CHECK(length == 3u && bytes[0] == 0xACu);
        aht_measuring = true;
        aht_ack_at = now;
    } else {
        CHECK(length == 1u && (bytes[0] == 0x94u || bytes[0] == 0x89u || bytes[0] == 0xFDu));
        command = bytes[0];
        command_ack_at = now;
        consumed = false;
        if (command == 0xFDu)
            ++measurements;
    }
    return true;
}
bool bus_read(uint8_t address, uint8_t *bytes, size_t length) {
    if (!transaction(address))
        return false;
    if (address == 0x38u) {
        CHECK(length == 1u || length == 6u);
        bool busy = aht_measuring && (uint32_t)(now - aht_ack_at) < 80u;
        now += wire_ms;
        memset(bytes, 0, length);
        bytes[0] = busy ? 0x88u : 0x08u;
        if (length == 6u) {
            CHECK(!busy && aht_measuring);
            uint32_t rh = 471860u, temp = 445645u; /* 45%, approximately 35 C */
            bytes[1] = (uint8_t)(rh >> 12);
            bytes[2] = (uint8_t)(rh >> 4);
            bytes[3] = (uint8_t)((rh << 4) | (temp >> 16));
            bytes[4] = (uint8_t)(temp >> 8);
            bytes[5] = (uint8_t)temp;
            aht_measuring = false;
        }
        return true;
    }
    CHECK(length == 6u);
    uint32_t wait = command == 0x89u ? 1u : 9u;
    if ((uint32_t)(now - command_ack_at) < wait) {
        ++early_reads;
        last_error = BUS_ADDRESS_NACK;
        now += wire_ms;
        return false;
    }
    if (busy_reads && command == 0xFDu) {
        --busy_reads;
        last_error = BUS_ADDRESS_NACK;
        now += wire_ms;
        return false;
    }
    CHECK((command == 0x89u || command == 0xFDu) && !consumed);
    consumed = true;
    now += wire_ms;
    word(bytes, command == 0x89u ? 0xBEEFu : 26214u);
    word(bytes + 3u, command == 0x89u ? 0xBEEFu : 26738u);
    if (crc_frames && command == 0xFDu) {
        --crc_frames;
        bytes[5] ^= 1u;
    }
    return true;
}
static void run(uint32_t ms) {
    uint32_t start = now;
    while ((uint32_t)(now - start) < ms) {
        sensor_task(now);
        ++now;
    }
}
static void reset(uint32_t epoch, uint32_t transfer_ms) {
    now = epoch;
    wire_ms = transfer_ms;
    command = 0u;
    command_ack_at = aht_ack_at = epoch;
    early_reads = measurements = aht_io = aht_interleaved = recoveries = 0u;
    busy_reads = crc_frames = 0u;
    missing = stuck = consumed = aht_measuring = false;
    sensor_init(now);
}
int main(void) {
    /* This fails on 1.3.0: its 10 ms wait starts BEFORE a 6 ms command write,
     * so a fully functional sensor is read early and falls back to AHT20. */
    reset(0u, 6u);
    run(2500u);
    CHECK(early_reads == 0u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X && !sensor_primary_fault(now));
    CHECK(aht_io == 0u && aht_interleaved == 0u);
    for (uint32_t duration = 1u; duration <= 25u; duration += 3u) {
        reset(0u, duration);
        run(5000u);
        CHECK(early_reads == 0u && measurements >= 4u);
        CHECK(sensor_source() == SENSOR_SHT4X && aht_io == 0u);
        CHECK((uint32_t)(now - sensor_get()->last_good_ms) < 1500u);
    }
    reset(0u, 6u);
    run(800u);
    unsigned triggered = measurements;
    busy_reads = 2u;
    run(500u);
    CHECK(measurements == triggered + 1u && busy_reads == 0u);
    CHECK(sensor_source() == SENSOR_SHT4X && !sensor_primary_fault(now) && aht_io == 0u);
    /* A bad CRC consumes the response: retry must issue a NEW command. */
    crc_frames = 1u;
    run(1200u);
    CHECK(crc_frames == 0u && sensor_source() == SENSOR_SHT4X && !sensor_primary_fault(now));
    CHECK(aht_io == 0u);
    missing = true;
    run(1100u);
    CHECK(aht_io > 0u && sensor_primary_fault(now));
    run(800u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_AHT20);
    missing = false;
    run(4500u);
    CHECK(sensor_source() == SENSOR_SHT4X && !sensor_primary_fault(now));
    CHECK(aht_interleaved == 0u);
    unsigned previous_recoveries = recoveries;
    stuck = true;
    run(1300u);
    CHECK(recoveries > previous_recoveries && sensor_source() == SENSOR_SHT4X);
    reset(0xFFFFFF80u, 6u);
    run(3000u);
    CHECK(early_reads == 0u && sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    printf("sensor timing: blocking transfers, post-ACK wait, busy NACK, CRC consume, "
           "exclusive primary, confirmed failover, recovery, wrap passed (%u checks)\n",
           checks);
    return 0;
}

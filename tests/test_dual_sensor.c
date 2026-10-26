#include "config.h"
#include "sensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, measurements, serials, resets, aht_frames;
static uint32_t now, command_at;
static uint8_t present_address, command;
static bool aht_present, sht_present, serial_bad, t_crc_bad, rh_crc_bad, read_nack;
static int primary_t10, reserve_t10;
static uint16_t raw_humidity, raw_temperature;
static bool use_raw_temperature;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL dual_sensor:%u %s\n", __LINE__, #x);                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* Word-wise polynomial division; BEEF -> 92 is the datasheet check vector. */
static uint8_t crc_word(uint16_t word) {
    uint32_t remainder = ((uint32_t)(word ^ 0xFF00u)) << 8;
    for (int bit = 23; bit >= 8; --bit)
        if (remainder & (1ul << bit))
            remainder ^= 0x131ul << (bit - 8);
    return (uint8_t)remainder;
}
static void put_word(uint8_t *out, uint16_t word) {
    out[0] = (uint8_t)(word >> 8);
    out[1] = (uint8_t)word;
    out[2] = crc_word(word);
}
void bus_recover(void) {}
bus_error_t bus_last_error(void) { return BUS_ADDRESS_NACK; }
bool bus_write(uint8_t addr, const uint8_t *bytes, size_t length) {
    if (addr == 0x38u) {
        CHECK(length == 3u && bytes[0] == 0xACu && bytes[1] == 0x33u && bytes[2] == 0u);
        return aht_present;
    }
    CHECK(addr >= 0x44u && addr <= 0x46u && length == 1u);
    CHECK(bytes[0] == 0x94u || bytes[0] == 0x89u || bytes[0] == 0xFDu);
    if (!sht_present || addr != present_address)
        return false;
    command = bytes[0];
    command_at = now;
    if (command == 0x94u)
        ++resets;
    else if (command == 0x89u)
        ++serials;
    else
        ++measurements;
    return true;
}
bool bus_read(uint8_t addr, uint8_t *bytes, size_t length) {
    if (addr == 0x38u) {
        CHECK(length == 1u || length == 6u);
        if (!aht_present)
            return false;
        memset(bytes, 0, length);
        bytes[0] = 0x08u;
        if (length == 6u) {
            ++aht_frames;
            uint32_t rh = 471860u; /* 45% */
            uint32_t t = (uint32_t)(reserve_t10 + 500) * 1048576u / 2000u;
            bytes[1] = (uint8_t)(rh >> 12);
            bytes[2] = (uint8_t)(rh >> 4);
            bytes[3] = (uint8_t)((rh << 4) | (t >> 16));
            bytes[4] = (uint8_t)(t >> 8);
            bytes[5] = (uint8_t)t;
        }
        return true;
    }
    CHECK(addr >= 0x44u && addr <= 0x46u && length == 6u);
    if (!sht_present || addr != present_address || read_nack)
        return false;
    if (command == 0x89u) {
        CHECK((uint32_t)(now - command_at) >= 1u);
        put_word(bytes, 0xBEEFu);
        put_word(bytes + 3u, 0xBEEFu);
        if (serial_bad)
            bytes[5] ^= 1u;
    } else {
        CHECK(command == 0xFDu && (uint32_t)(now - command_at) >= 10u);
        uint16_t t = use_raw_temperature
                         ? raw_temperature
                         : (uint16_t)((uint32_t)(primary_t10 + 450) * 65535u / 1750u);
        put_word(bytes, t);
        put_word(bytes + 3u, raw_humidity);
        if (t_crc_bad)
            bytes[2] ^= 1u;
        if (rh_crc_bad)
            bytes[5] ^= 1u;
    }
    return true;
}
static void run(uint32_t duration) {
    for (uint32_t i = 0u; i < duration; ++i) {
        ++now;
        sensor_task(now);
    }
}
static void next_result(void) {
    unsigned before = measurements;
    for (unsigned i = 0u; i < 2000u && measurements == before; ++i)
        run(1u);
    CHECK(measurements == before + 1u);
    run(10u);
}
static void reset(uint32_t epoch) {
    measurements = serials = resets = aht_frames = 0u;
    sht_present = aht_present = true;
    serial_bad = t_crc_bad = rh_crc_bad = read_nack = use_raw_temperature = false;
    present_address = 0x44u;
    primary_t10 = 250;
    reserve_t10 = 350;
    raw_humidity = 26738u; /* approximately 45% */
    now = epoch;
    sensor_init(now);
}
int main(void) {
    CHECK(crc_word(0xBEEFu) == 0x92u);
    for (uint8_t address = 0x44u; address <= 0x46u; ++address) {
        reset(0u);
        present_address = address;
        run(800u);
        CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
        CHECK(sensor_get()->temperature_x10 == 250 && sensor_get()->humidity_x10 == 450u);
        CHECK(serials == 1u && resets == 1u && measurements == 1u && aht_frames == 0u);
        CHECK(!sensor_overheated(now));
        CHECK(!sensor_primary_fault(now));
    }
    reset(0u);
    aht_present = false;
    run(800u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    reset(0u);
    sht_present = false;
    run(800u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_AHT20);
    reset(0u);
    sht_present = aht_present = false;
    run(800u);
    CHECK(!sensor_ok(now) && sensor_source() == SENSOR_NONE);
    reset(0u);
    serial_bad = true;
    run(1800u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_AHT20 && measurements == 0u);
    serial_bad = false;
    run(3500u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    /* One bad frame never alternates sources. Confirm failure, then require
     * consecutive valid recovery frames before returning to the primary. */
    for (unsigned crc = 0u; crc < 2u; ++crc) {
        reset(0u);
        run(800u);
        t_crc_bad = crc == 0u;
        rh_crc_bad = crc == 1u;
        run(1500u);
        CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X && sensor_primary_fault(now));
        CHECK(sensor_get()->temperature_x10 == 250);
        run(2200u);
        CHECK(sensor_ok(now) && sensor_source() == SENSOR_AHT20);
        t_crc_bad = rh_crc_bad = false;
        run(3500u);
        CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    }
    reset(0u);
    run(800u);
    sht_present = false;
    run(3500u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_AHT20);
    sht_present = true;
    present_address = 0x46u; /* Rediscover after repeated failures, without reboot. */
    run(3500u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    aht_present = false;
    run(3500u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    read_nack = true;
    run(3500u);
    CHECK(!sensor_ok(now));
    read_nack = false;
    run(3500u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X);
    uint32_t last_good = sensor_get()->last_good_ms;
    CHECK(sensor_ok(last_good + SH01_SENSOR_STALE_MS));
    CHECK(!sensor_ok(last_good + SH01_SENSOR_STALE_MS + 1u));
    reset(0u);
    reserve_t10 = 740;
    run(800u);
    CHECK(sensor_source() == SENSOR_SHT4X && sensor_get()->temperature_x10 == 250);
    CHECK(!sensor_overheated(now) && aht_frames == 0u);
    sht_present = false;
    run(3500u);
    CHECK(sensor_source() == SENSOR_AHT20 && sensor_overheated(now));
    reset(0u);
    primary_t10 = 730;
    run(800u);
    CHECK(sensor_overheated(now));
    reset(0u);
    primary_t10 = 729;
    run(800u);
    CHECK(!sensor_overheated(now));
    reset(0u);
    raw_humidity = 0u;
    run(800u);
    CHECK(sensor_get()->humidity_x10 == 0u);
    raw_humidity = 65535u;
    run(3500u);
    CHECK(sensor_get()->humidity_x10 == 1000u);
    use_raw_temperature = true;
    raw_temperature = 0u; /* -45 C is outside the specified measurement range. */
    run(3500u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_AHT20);
    reset(0u);
    run(800u);
    t_crc_bad = true;
    run(3500u);
    CHECK(sensor_source() == SENSOR_AHT20);
    t_crc_bad = false;
    next_result();
    CHECK(sensor_source() == SENSOR_AHT20); /* One success cannot flip it back. */
    t_crc_bad = true;
    next_result();
    CHECK(sensor_source() == SENSOR_AHT20);
    t_crc_bad = false;
    next_result();
    CHECK(sensor_source() == SENSOR_AHT20);
    next_result();
    CHECK(sensor_source() == SENSOR_AHT20);
    next_result();
    CHECK(sensor_source() == SENSOR_SHT4X && !sensor_primary_fault(now));
    unsigned reserve_reads = aht_frames;
    run(2000u);
    CHECK(aht_frames == reserve_reads); /* Healthy primary never polls reserve. */
    t_crc_bad = true;
    next_result();
    CHECK(sensor_source() == SENSOR_SHT4X && sensor_primary_fault(now));
    t_crc_bad = false;
    next_result();
    CHECK(sensor_source() == SENSOR_SHT4X && !sensor_primary_fault(now));
    reset(0xFFFFFF80u);
    run(800u);
    CHECK(sensor_ok(now) && sensor_source() == SENSOR_SHT4X && !sensor_overheated(now));
    printf("dual sensor: scan, serial/CRC, priority, failover/recovery, "
           "active-source overheat, "
           "stale, range, rollover: %u checks passed\n",
           checks);
    return 0;
}

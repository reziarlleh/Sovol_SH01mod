#include "hal.h"
#include "sensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, reads, read_at, edges;
static bool inputs[256], input_direction, sda = true, scl_stuck, sda_stuck;
static struct {
    bool input, value;
} trace[256];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL bus:%u %s\n", __LINE__, #x);                                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void hal_delay_us(uint32_t us) { CHECK(us >= 10u); }
void hal_scl(bool high) {
    if (high) {
        CHECK(edges < 256u);
        trace[edges].input = input_direction;
        trace[edges++].value = sda;
    }
}
bool hal_scl_read(void) { return !scl_stuck; }
void hal_sda_output(bool high) {
    sda = high;
    input_direction = false;
}
void hal_sda_input(void) { input_direction = true; }
bool hal_sda_read(void) {
    if (sda_stuck)
        return false;
    CHECK(read_at < reads);
    if (read_at > 0u)
        CHECK(input_direction); /* ACK/data must release direction, not merely write HIGH. */
    return inputs[read_at++];
}
static void reset(void) {
    reads = read_at = edges = 0u;
    input_direction = scl_stuck = sda_stuck = false;
    sda = true;
    memset(trace, 0, sizeof trace);
}
static void queue(bool value) { inputs[reads++] = value; }
static void queue_byte(uint8_t value) {
    for (unsigned i = 0; i < 8u; ++i)
        queue((value & (128u >> i)) != 0u);
}
static void check_byte(unsigned edge, uint8_t value) {
    for (unsigned i = 0; i < 8u; ++i) {
        CHECK(!trace[edge + i].input);
        CHECK(trace[edge + i].value == ((value & (128u >> i)) != 0u));
    }
}
int main(void) {
    uint8_t cmd[3] = {0xACu, 0x33u, 0u}, out[6], data[6] = {8u, 0x72u, 0x34u, 0x12u, 0xABu, 0xCDu};
    reset();
    queue(true);
    for (unsigned i = 0; i < 4u; ++i)
        queue(false);
    CHECK(bus_write(0x38u, cmd, 3u) && bus_last_error() == BUS_OK && read_at == reads);
    check_byte(1u, 0x70u);
    for (unsigned i = 0; i < 3u; ++i)
        check_byte(10u + 9u * i, cmd[i]);
    for (unsigned i = 0; i < 4u; ++i)
        CHECK(trace[9u + 9u * i].input);
    reset();
    queue(true);
    queue(false);
    for (unsigned i = 0; i < 6u; ++i)
        queue_byte(data[i]);
    CHECK(bus_read(0x38u, out, 6u) && memcmp(out, data, 6u) == 0 && read_at == reads);
    check_byte(1u, 0x71u);
    for (unsigned i = 0; i < 6u; ++i) {
        for (unsigned b = 0; b < 8u; ++b)
            CHECK(trace[10u + 9u * i + b].input);
        CHECK(!trace[18u + 9u * i].input && trace[18u + 9u * i].value == (i == 5u));
    }
    reset();
    queue(true);
    queue(true);
    CHECK(!bus_write(0x38u, cmd, 3u) && bus_last_error() == BUS_ADDRESS_NACK);
    reset();
    queue(true);
    queue(false);
    queue(true);
    CHECK(!bus_write(0x38u, cmd, 3u) && bus_last_error() == BUS_DATA_NACK);
    reset();
    scl_stuck = true;
    CHECK(!bus_read(0x38u, out, 6u) && bus_last_error() == BUS_SCL_LOW);
    reset();
    sda_stuck = true;
    CHECK(!bus_read(0x38u, out, 6u) && bus_last_error() == BUS_SDA_LOW);
    reset();
    bus_recover();
    CHECK(edges == 10u && sda && !input_direction);
    printf("wire bus: address, bytes, input ACK/data, final NACK, faults, recovery: %u "
           "checks passed\n",
           checks);
    return 0;
}

#include "hal.h"
#include "sensor.h"
static bus_error_t error;
static void pause_bus(void) { hal_delay_us(10u); }
static void data(bool high) {
    hal_sda_output(high);
    pause_bus();
}
static void low(void) {
    hal_scl(false);
    pause_bus();
}
static bool high(void) {
    unsigned i;
    hal_scl(true);
    pause_bus();
    for (i = 0u; i < 100u; ++i) {
        if (hal_scl_read())
            return true;
        pause_bus();
    }
    error = BUS_SCL_LOW;
    return false;
}
static void release_data(void) {
    hal_sda_input();
    pause_bus();
}
static bool start(void) {
    data(true);
    if (!high())
        return false;
    if (!hal_sda_read()) {
        error = BUS_SDA_LOW;
        return false;
    }
    data(false);
    low();
    return true;
}
static void stop(void) {
    low();
    data(false);
    (void)high();
    data(true);
}
static bool send(uint8_t value) {
    unsigned i;
    bool ack;
    for (i = 0u; i < 8u; ++i) {
        data((value & 128u) != 0u);
        if (!high()) {
            low();
            return false;
        }
        low();
        value <<= 1;
    }
    release_data();
    if (!high()) {
        low();
        return false;
    }
    ack = !hal_sda_read();
    low();
    return ack;
}
static bool receive(uint8_t *out, bool ack) {
    unsigned i;
    uint8_t value = 0u;
    release_data();
    for (i = 0u; i < 8u; ++i) {
        if (!high()) {
            low();
            return false;
        }
        value = (uint8_t)((value << 1) | (hal_sda_read() ? 1u : 0u));
        low();
    }
    data(!ack);
    if (!high()) {
        low();
        return false;
    }
    low();
    release_data();
    *out = value;
    return true;
}
void bus_recover(void) {
    unsigned i;
    error = BUS_OK;
    data(true);
    for (i = 0u; i < 9u; ++i) {
        low();
        if (!high())
            break;
    }
    stop();
}
bus_error_t bus_last_error(void) { return error; }
bool bus_write(uint8_t address, const uint8_t *bytes, size_t length) {
    size_t i;
    error = BUS_OK;
    if (length && !bytes)
        return false;
    if (!start()) {
        stop();
        return false;
    }
    if (!send((uint8_t)(address << 1))) {
        if (error == BUS_OK)
            error = BUS_ADDRESS_NACK;
        stop();
        return false;
    }
    for (i = 0u; i < length; ++i)
        if (!send(bytes[i])) {
            if (error == BUS_OK)
                error = BUS_DATA_NACK;
            stop();
            return false;
        }
    stop();
    return error == BUS_OK;
}
bool bus_read(uint8_t address, uint8_t *bytes, size_t length) {
    size_t i;
    error = BUS_OK;
    if (!length)
        return true;
    if (!bytes)
        return false;
    if (!start()) {
        stop();
        return false;
    }
    if (!send((uint8_t)((address << 1) | 1u))) {
        if (error == BUS_OK)
            error = BUS_ADDRESS_NACK;
        stop();
        return false;
    }
    for (i = 0u; i < length; ++i)
        if (!receive(&bytes[i], i + 1u < length)) {
            stop();
            return false;
        }
    stop();
    return error == BUS_OK;
}

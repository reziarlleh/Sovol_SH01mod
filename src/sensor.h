#ifndef SH01_SENSOR_H
#define SH01_SENSOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { BUS_OK, BUS_SCL_LOW, BUS_SDA_LOW, BUS_ADDRESS_NACK, BUS_DATA_NACK } bus_error_t;
bool bus_write(uint8_t address, const uint8_t *data, size_t length);
bool bus_read(uint8_t address, uint8_t *data, size_t length);
void bus_recover(void);
bus_error_t bus_last_error(void);
typedef struct {
    int16_t temperature_x10;
    uint16_t humidity_x10;
    uint32_t last_good_ms;
    uint8_t last_status;
    uint8_t failures;
    bool ready;
} sensor_sample_t;
typedef enum { SENSOR_NONE, SENSOR_SHT4X, SENSOR_AHT20 } sensor_source_t;
void sensor_init(uint32_t now);
void sensor_task(uint32_t now);
const sensor_sample_t *sensor_get(void);
bool sensor_ok(uint32_t now);
sensor_source_t sensor_source(void);
bool sensor_overheated(uint32_t now);
bool sensor_primary_fault(uint32_t now);
#endif

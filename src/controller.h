#ifndef SH01_CONTROLLER_H
#define SH01_CONTROLLER_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { MODE_OFF, MODE_VIEW, MODE_TEMP, MODE_TIME, MODE_DRY, MODE_FAULT } factory_mode_t;
typedef struct {
    factory_mode_t mode;
    uint8_t target_c, hours, error;
    uint32_t remaining_s;
    bool heater, fan, time_screen;
} controller_state_t;
void controller_init(uint32_t now);
void controller_task(uint32_t now);
const controller_state_t *controller_get(void);
#endif

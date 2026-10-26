#ifndef SH01_CONTROLLER_H
#define SH01_CONTROLLER_H
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    MODE_OFF,
    MODE_VIEW,
    MODE_DRY,
    MODE_FAULT
} controller_mode_t;

/* UI is an overlay: browsing/editing cannot pause or change the active cycle. */
typedef enum { MENU_NONE, MENU_PROFILE, MENU_TEMP, MENU_TIME, MENU_FAN } controller_menu_t;
typedef struct {
    controller_mode_t mode;
    controller_menu_t menu;
    uint8_t target_c, hours, error, fan_percent, profile;
    uint32_t remaining_s;
    bool heater, fan, time_screen;
} controller_state_t;
void controller_init(uint32_t now);
void controller_task(uint32_t now);
const controller_state_t *controller_get(void);
#endif

#include "config.h"
#include "controller.h"
#include "ddl.h"
#include "hal.h"
#include "lcd.h"
#include "sensor.h"
#include "sysctrl.h"
#include "wdt.h"
const char firmware_version[] __attribute__((used, section(".firmware_info"))) = FW_VERSION;

static void failed(void) {
    hal_heater(false);
    hal_fan(false);
    for (;;) {
    }
}
int32_t main(void) {
    bool ready = hal_early_init();
    if (!hal_clock_init())
        failed();
    __enable_irq();
    if (Sysctrl_SetPeripheralGate(SysctrlPeripheralWdt, TRUE) != Ok ||
        Wdt_Init(WdtResetEn, WdtT3s28) != Ok)
        failed();
    Wdt_Start();
    sensor_init(hal_millis());
    /* Recovery grace for FT232/SWD header before P27/P31 become LCD/FAN. */
    while (hal_millis() < 2500u) {
        if (!hal_clock_ok())
            failed();
        Wdt_Feed();
        sensor_task(hal_millis());
    }
    if (!ready || !hal_lcd_pins_init())
        failed();
    lcd_init();
    controller_init(hal_millis());
    for (;;) {
        if (!hal_clock_ok())
            failed();
        Wdt_Feed();
        sensor_task(hal_millis());
        controller_task(hal_millis());
    }
}

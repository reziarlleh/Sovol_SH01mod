#ifndef SH01_HAL_H
#define SH01_HAL_H
#include <stdbool.h>
#include <stdint.h>
bool hal_early_init(void);
bool hal_lcd_pins_init(void);
bool hal_clock_init(void);
bool hal_clock_ok(void);
uint32_t hal_millis(void);
void hal_delay_us(uint32_t us);
void hal_heater(bool on);
void hal_fan(bool on);
void hal_beep(bool on);
void hal_backlight(bool on);
void hal_led(unsigned key, bool on);
bool hal_key(unsigned key);
bool hal_adc(uint16_t *mean_x10);
void hal_scl(bool high);
bool hal_scl_read(void);
void hal_sda_output(bool high);
void hal_sda_input(void);
bool hal_sda_read(void);
void hal_lcd_cs(bool high);
void hal_lcd_wr(bool high);
void hal_lcd_data(bool high);
#endif

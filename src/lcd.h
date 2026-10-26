#ifndef SH01_LCD_H
#define SH01_LCD_H
#include <stdbool.h>
#include <stdint.h>
void lcd_init(void);
void lcd_enable(bool on);
void lcd_temperature_humidity(int16_t temperature_x10, uint16_t humidity_x10, bool work);
void lcd_missing(void);
void lcd_set_temperature(uint8_t temperature);
void lcd_time(uint32_t seconds, bool setting, bool work);
void lcd_error(uint8_t code);
void lcd_fan_percent(uint8_t percent);
#endif

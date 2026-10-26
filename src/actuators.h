#ifndef SH01_ACTUATORS_H
#define SH01_ACTUATORS_H
#include <stdbool.h>
#include <stdint.h>
/* Called under the HAL interrupt lock, or by the 1 ms SysTick ISR. */
void actuators_tick_1ms(void);
void actuators_fan(uint8_t percent);
void actuators_click(void);
void actuators_error_signal(void);
void actuators_silence(void);
/* Board callbacks: 0 = silence, UINT16_MAX = original key click. */
void hal_fan_drive(bool high);
void hal_tone_drive(uint16_t hz);
#endif

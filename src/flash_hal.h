#ifndef SH01_FLASH_HAL_H
#define SH01_FLASH_HAL_H
#include <stdbool.h>
#include <stdint.h>
uint32_t flash_hal_read(uint32_t address);
bool flash_hal_write(uint32_t address, uint32_t value);
bool flash_hal_erase(uint32_t address);
#endif

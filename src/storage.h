#ifndef SH01_STORAGE_H
#define SH01_STORAGE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t remaining_s;
    uint8_t target_c, hours;
    bool running;
} storage_cycle_t;

void storage_init(void);
bool storage_load(storage_cycle_t *cycle);
bool storage_save(const storage_cycle_t *cycle);
bool storage_disarm(void);
#endif

#ifndef SH01_PROFILES_H
#define SH01_PROFILES_H
#include <stdint.h>

#define SH01_PROFILE_COUNT 9u
typedef struct {
    char name[5];
    uint8_t target_c, hours;
} drying_profile_t;

/* Index zero is CUSt: its values come from the active settings, not this table. */
const drying_profile_t *profile_get(uint8_t index);
#endif

#include "profiles.h"

static const drying_profile_t profiles[SH01_PROFILE_COUNT] = {
    {"CUSt", 0u, 0u},  {"AbS", 70u, 6u},  {"ASA", 70u, 8u},
    {"HIPS", 70u, 3u}, {"PA12", 70u, 4u}, {"PCtG", 65u, 8u},
    {"PEtG", 60u, 6u}, {"PLA", 45u, 6u},  {"TPU", 60u, 6u},
};

const drying_profile_t *profile_get(uint8_t index) {
    return &profiles[index < SH01_PROFILE_COUNT ? index : 0u];
}

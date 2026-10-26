#include "storage.h"
#include "config.h"
#include "flash_hal.h"
#include <stddef.h>

/* 25 records per sector, eight sectors. Header is committed last.
 * A separate erased stop word is programmed LOW before stopping: even a
 * partial stop write disables this record without altering its CRC. */
#define RECORD_BYTES 20u
#define SLOTS_PER_PAGE (SH01_JOURNAL_PAGE_BYTES / RECORD_BYTES)
#define SLOT_COUNT (SLOTS_PER_PAGE * SH01_JOURNAL_PAGES)
#define RECORD_MAGIC 0x31524853u /* SHR1: persistent format 1 */
#define NO_SLOT UINT32_MAX
static const uint8_t presets[] = {SH01_TIME_STORAGE_VALUES};
_Static_assert(sizeof presets <= 16u, "Duration index has only four bits");
static uint32_t latest_slot, sequence;
static storage_cycle_t saved;

static uint32_t slot_address(uint32_t slot) {
    return SH01_JOURNAL_BASE + slot / SLOTS_PER_PAGE * SH01_JOURNAL_PAGE_BYTES +
           slot % SLOTS_PER_PAGE * RECORD_BYTES;
}
static uint32_t crc32(uint32_t seq, uint32_t packed) {
    uint32_t crc = UINT32_MAX;
    const uint32_t words[] = {RECORD_MAGIC, seq, packed};
    for (unsigned w = 0u; w < 3u; ++w)
        for (unsigned byte = 0u; byte < 4u; ++byte) {
            crc ^= (words[w] >> (byte * 8u)) & 255u;
            for (unsigned bit = 0u; bit < 8u; ++bit)
                crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    return ~crc;
}
static bool legacy_temperature(unsigned code) {
    return code >= SH01_TEMP_MIN_C && code <= SH01_TEMP_MAX_C &&
           (code - SH01_TEMP_MIN_C) % SH01_TEMP_STEP_C == 0u;
}
/* Keep every old 100% record byte-compatible. Other speeds use the 63
 * remaining codes, skipping the seven legacy temperatures. Old firmware
 * rejects these codes and its newer-record fence prevents stale resume. */
static unsigned setting_code(const storage_cycle_t *cycle) {
    if (cycle->fan_percent == 100u)
        return cycle->target_c;
    unsigned code =
        (cycle->target_c - SH01_TEMP_MIN_C) / SH01_TEMP_STEP_C * 9u + cycle->fan_percent / 10u - 1u;
    for (unsigned t = SH01_TEMP_MIN_C; t <= SH01_TEMP_MAX_C; t += SH01_TEMP_STEP_C)
        if (code >= t)
            ++code;
    return code;
}
static bool decode_setting(unsigned code, storage_cycle_t *cycle) {
    if (legacy_temperature(code)) {
        cycle->target_c = (uint8_t)code;
        cycle->fan_percent = 100u;
        return true;
    }
    unsigned index = code;
    for (unsigned t = SH01_TEMP_MIN_C; t <= SH01_TEMP_MAX_C; t += SH01_TEMP_STEP_C)
        if (code > t)
            --index;
    if (index >= 63u)
        return false;
    cycle->target_c = (uint8_t)(SH01_TEMP_MIN_C + index / 9u * SH01_TEMP_STEP_C);
    cycle->fan_percent = (uint8_t)((index % 9u + 1u) * 10u);
    return true;
}
static bool pack(const storage_cycle_t *cycle, uint32_t *value) {
    unsigned i;
    if (!cycle || cycle->target_c < SH01_TEMP_MIN_C || cycle->target_c > SH01_TEMP_MAX_C ||
        (cycle->target_c - SH01_TEMP_MIN_C) % SH01_TEMP_STEP_C != 0u || cycle->fan_percent < 10u ||
        cycle->fan_percent > 100u || cycle->fan_percent % 10u)
        return false;
    for (i = 0u; i < sizeof presets; ++i)
        if (presets[i] == cycle->hours)
            break;
    if (i == sizeof presets || cycle->remaining_s > (uint32_t)cycle->hours * 3600u ||
        (cycle->running && (cycle->remaining_s == 0u || cycle->powered_off)))
        return false;
    *value = cycle->remaining_s | (uint32_t)setting_code(cycle) << 19 | (uint32_t)i << 26 |
             (cycle->running ? 1ul << 30 : 0u) | (cycle->powered_off ? 1ul << 31 : 0u);
    return true;
}
static bool decode(uint32_t slot, storage_cycle_t *cycle, uint32_t *seq) {
    uint32_t address = slot_address(slot), packed, index, unused;
    if (flash_hal_read(address) != RECORD_MAGIC)
        return false;
    *seq = flash_hal_read(address + 4u);
    packed = flash_hal_read(address + 8u);
    if (flash_hal_read(address + 12u) != crc32(*seq, packed))
        return false;
    index = (packed >> 26) & 15u;
    if (index >= sizeof presets)
        return false;
    cycle->remaining_s = packed & 0x7FFFFu;
    if (!decode_setting((packed >> 19) & 127u, cycle))
        return false;
    cycle->hours = presets[index];
    cycle->running = (packed & (1ul << 30)) != 0u;
    cycle->powered_off = (packed & (1ul << 31)) != 0u;
    if (!pack(cycle, &unused))
        return false;
    if (flash_hal_read(address + 16u) != UINT32_MAX)
        cycle->running = false;
    return true;
}
void storage_init(void) {
    storage_cycle_t cycle;
    uint32_t seq, marked_sequence = 0u;
    bool marked = false;
    latest_slot = NO_SLOT;
    sequence = 0u;
    saved = (storage_cycle_t){0};
    for (uint32_t slot = 0u; slot < SLOT_COUNT; ++slot) {
        if (flash_hal_read(slot_address(slot)) == RECORD_MAGIC) {
            uint32_t candidate = flash_hal_read(slot_address(slot) + 4u);
            if (!marked || (int32_t)(candidate - marked_sequence) > 0) {
                marked = true;
                marked_sequence = candidate;
            }
        }
        if (decode(slot, &cycle, &seq) &&
            (latest_slot == NO_SLOT || (int32_t)(seq - sequence) > 0)) {
            latest_slot = slot;
            sequence = seq;
            saved = cycle;
        }
    }
    /* A committed newer record with bad CRC is corruption, not a torn
     * uncommitted append. Do not silently resume an older active cycle. */
    if (marked && (latest_slot == NO_SLOT || (int32_t)(marked_sequence - sequence) > 0)) {
        sequence = marked_sequence;
        saved.running = false;
    }
}
bool storage_load(storage_cycle_t *cycle) {
    if (latest_slot == NO_SLOT || !cycle)
        return false;
    *cycle = saved;
    return true;
}
static bool blank(uint32_t address, uint32_t length) {
    for (uint32_t offset = 0u; offset < length; offset += 4u)
        if (flash_hal_read(address + offset) != UINT32_MAX)
            return false;
    return true;
}
bool storage_save(const storage_cycle_t *cycle) {
    uint32_t slot, address, packed, seq;
    if (!pack(cycle, &packed))
        return false;
    slot = latest_slot == NO_SLOT ? 0u : (latest_slot + 1u) % SLOT_COUNT;
    /* Skip torn slots; at a sector boundary recycle only a different sector
     * from the most recent committed record. */
    for (uint32_t tried = 0u; tried < SLOT_COUNT; ++tried) {
        address = slot_address(slot);
        if (slot % SLOTS_PER_PAGE == 0u && !blank(address, SH01_JOURNAL_PAGE_BYTES)) {
            if (latest_slot != NO_SLOT && slot / SLOTS_PER_PAGE == latest_slot / SLOTS_PER_PAGE)
                return false;
            if (!flash_hal_erase(address))
                return false;
        }
        if (blank(address, RECORD_BYTES))
            break;
        slot = (slot + 1u) % SLOT_COUNT;
        if (tried + 1u == SLOT_COUNT)
            return false;
    }
    address = slot_address(slot);
    seq = sequence + 1u;
    if (!flash_hal_write(address + 4u, seq) || !flash_hal_write(address + 8u, packed) ||
        !flash_hal_write(address + 12u, crc32(seq, packed)) ||
        !flash_hal_write(address, RECORD_MAGIC))
        return false;
    storage_cycle_t check;
    uint32_t check_seq;
    if (!decode(slot, &check, &check_seq) || check_seq != seq || check.running != cycle->running ||
        check.target_c != cycle->target_c || check.hours != cycle->hours ||
        check.remaining_s != cycle->remaining_s || check.fan_percent != cycle->fan_percent ||
        check.powered_off != cycle->powered_off)
        return false;
    latest_slot = slot;
    sequence = seq;
    saved = check;
    return true;
}
bool storage_disarm(void) {
    /* Rescan also covers a failed write whose commit reached Flash before
     * the backend reported an error. Never leave that newer record armed. */
    storage_init();
    if (latest_slot == NO_SLOT || !saved.running)
        return true;
    uint32_t address = slot_address(latest_slot) + 16u;
    if (!flash_hal_write(address, 0u) && flash_hal_read(address) == UINT32_MAX)
        return false;
    saved.running = false;
    return true;
}

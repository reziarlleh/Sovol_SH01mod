#include "flash_hal.h"
#include "config.h"
#include "ddl.h"
#include "flash.h"
#include "sysctrl.h"

static bool prepared;
static bool address_ok(uint32_t address) {
    return address >= SH01_JOURNAL_BASE && address <= 0x7FFCu && (address & 3u) == 0u;
}
static bool prepare(void) {
    uint32_t hz;
    if (prepared)
        return true;
    hz = Sysctrl_GetHClkFreq();
    if (hz != 4000000u && hz != 8000000u && hz != 16000000u && hz != 24000000u && hz != 32000000u)
        return false;
    prepared = Flash_Init((uint8_t)(hz / 4000000u)) == Ok;
    return prepared;
}
static void finish(uint32_t irq_state) {
    /* Vendor errors can leave the controller unlocked/in program mode. */
    Flash_LockAll();
    M0P_FLASH->BYPASS = 0x5A5Au;
    M0P_FLASH->BYPASS = 0xA5A5u;
    M0P_FLASH->CR_f.OP = 0u;
    __DSB();
    __ISB();
    __set_PRIMASK(irq_state);
}
uint32_t flash_hal_read(uint32_t address) {
    return address_ok(address) ? *(volatile const uint32_t *)address : 0u;
}
bool flash_hal_write(uint32_t address, uint32_t value) {
    uint32_t irq_state;
    bool ok;
    if (!address_ok(address) || (flash_hal_read(address) & value) != value)
        return false;
    irq_state = __get_PRIMASK();
    __disable_irq();
    ok = prepare();
    if (ok) {
        Flash_ClearIntFlag(FlashPCInt);
        Flash_ClearIntFlag(FlashSlockInt);
        ok = Flash_WriteWord(address, value) == Ok && !Flash_GetIntFlag(FlashPCInt) &&
             !Flash_GetIntFlag(FlashSlockInt);
    }
    finish(irq_state);
    return ok && flash_hal_read(address) == value;
}
bool flash_hal_erase(uint32_t address) {
    uint32_t irq_state;
    bool ok;
    if (!address_ok(address) || (address % SH01_JOURNAL_PAGE_BYTES) != 0u)
        return false;
    irq_state = __get_PRIMASK();
    __disable_irq();
    ok = prepare();
    if (ok) {
        Flash_ClearIntFlag(FlashPCInt);
        Flash_ClearIntFlag(FlashSlockInt);
        ok = Flash_SectorErase(address) == Ok && !Flash_GetIntFlag(FlashPCInt) &&
             !Flash_GetIntFlag(FlashSlockInt);
    }
    finish(irq_state);
    if (!ok)
        return false;
    for (uint32_t offset = 0u; offset < SH01_JOURNAL_PAGE_BYTES; offset += 4u)
        if (flash_hal_read(address + offset) != UINT32_MAX)
            return false;
    return true;
}

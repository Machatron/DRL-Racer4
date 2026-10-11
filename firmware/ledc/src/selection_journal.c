#include "ledc_abi.h"
#define HALF(a) (*(volatile uint16_t *)(a))
#define NVM_BASE 0x41004000u
#define CMCC_BASE 0x41006000u
#define SEL_FIRST 0x2a000u
#define SEL_END 0x2c000u
#define SEL_READY WORD(0x200138f0u)
#define SEL_RECORD ((uint8_t *)0x200138f4u)
#define GIF_CHECKED ((uint32_t *)0x20013904u)
#define GIF_ENUM_READY WORD(0x20013a84u)

static int valid_selection(const uint8_t *r)
{
    return u32(r) && r[6] == 0x73 && r[7] == 1 && crc32(r, 12) == u32(r + 12) &&
           ((r[4] == 1 && r[5] < 32) || (r[4] == 2 && r[5] <= 69) || (r[4] == 5 && r[5] == 0));
}

static int nvm_ready(void)
{
    for (unsigned i = 0; i < 1000000; i++)
        if (HALF(NVM_BASE + 0x12) & 1)
            return 1;
    return 0;
}

static int nvm_command(unsigned command, unsigned address)
{
    if (!nvm_ready())
        return 0;
    HALF(NVM_BASE + 0x10) = 0x4fu;
    WORD(NVM_BASE + 0x14) = address;
    HALF(NVM_BASE + 4) = 0xa500u | command;
    __asm volatile("dsb\n\tisb" ::: "memory");
    return nvm_ready() && !(HALF(NVM_BASE + 0x10) & 0x4eu);
}

static int cache_off(void)
{
    WORD(CMCC_BASE + 8) = 0;
    __asm volatile("dsb\n\tisb" ::: "memory");
    for (unsigned i = 0; i < 1000000; i++)
        if (!(WORD(CMCC_BASE + 12) & 1)) {
            WORD(CMCC_BASE + 0x20) = 1;
            __asm volatile("dsb\n\tisb" ::: "memory");
            return 1;
        }
    return 0;
}

static void cache_restore(unsigned enabled)
{
    // Invalidating the NVM cache leaves stale CPU cache lines. Clear both before readback.
    WORD(CMCC_BASE + 0x20) = 1;
    __asm volatile("dsb\n\tisb" ::: "memory");
    WORD(CMCC_BASE + 8) = enabled;
    __asm volatile("dsb\n\tisb" ::: "memory");
}

int selection_program(unsigned address, const uint8_t *record)
{
    if (address < SEL_FIRST || address > SEL_END - 16 || (address & 15) || !valid_selection(record))
        return 0;
    for (unsigned i = 0; i < 16; i++)
        if (BYTE(address + i) != 255)
            return 0;
    if (!nvm_ready())
        return 0;
    uint16_t control = HALF(NVM_BASE);
    unsigned primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    unsigned cache = WORD(CMCC_BASE + 12) & 1;
    HALF(NVM_BASE) = (control & ~0x30u) | 0xc000u;
    int ok = cache_off() && nvm_command(0x15, address);
    if (ok) {
        for (unsigned i = 0; i < 16; i += 4)
            WORD(address + i) = u32(record + i);
        __asm volatile("dsb" ::: "memory");
        ok = nvm_command(4, address);
    }
    if (ok)
        for (unsigned i = 0; i < 16; i++)
            if (BYTE(address + i) != record[i])
                ok = 0;
    HALF(NVM_BASE) = control;
    cache_restore(cache);
    __asm volatile("dsb\n\tisb\n\tmsr primask, %0" ::"r"(primask) : "memory");
    return ok;
}

int selection_erase(void)
{
    if (!nvm_ready())
        return 0;
    uint16_t control = HALF(NVM_BASE);
    unsigned primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    unsigned cache = WORD(CMCC_BASE + 12) & 1;
    HALF(NVM_BASE) = (control & ~0x30u) | 0xc000u;
    int ok = cache_off() && nvm_command(1, SEL_FIRST);
    if (ok)
        for (unsigned a = SEL_FIRST; a < SEL_END; a += 4)
            if (WORD(a) != 0xffffffffu)
                ok = 0;
    HALF(NVM_BASE) = control;
    cache_restore(cache);
    __asm volatile("dsb\n\tisb\n\tmsr primask, %0" ::"r"(primask) : "memory");
    return ok;
}

static int latest_selection(uint8_t *out)
{
    if (!SEL_READY) {
        uint8_t r[16];
        int found = latest_qspi_selection(SEL_RECORD) >= 0;
        for (unsigned a = SEL_FIRST; a < SEL_END; a += 16) {
            for (unsigned i = 0; i < 16; i++)
                r[i] = BYTE(a + i);
            if (valid_selection(r) && (!found || (int32_t)(u32(r) - u32(SEL_RECORD)) > 0)) {
                for (unsigned i = 0; i < 16; i++)
                    SEL_RECORD[i] = r[i];
                found = 1;
            }
        }

        // Early boot reaches this before QSPI is mounted. Leave the cache unset so it retries after mounting.
        if (BYTE(LED_FILESYSTEM_MOUNTED))
            SEL_READY = found ? 2 : 1;
        if (!found)
            return -1;
    }
    if (SEL_READY == 1)
        return -1;
    for (unsigned i = 0; i < 16; i++)
        out[i] = SEL_RECORD[i];
    return 0;
}

static int save_selection(const uint8_t *record)
{
    unsigned address;
    for (address = SEL_FIRST; address < SEL_END; address += 16) {
        unsigned i;
        for (i = 0; i < 16; i++)
            if (BYTE(address + i) != 255)
                break;
        if (i == 16)
            break;
    }
    if (address == SEL_END) {

        uint8_t old[16];
        int bank = latest_qspi_selection(old);
        if (SEL_READY != 2)
            return 0;
        if (bank < 0 || u32(old) != u32(SEL_RECORD)) {
            char path[] = "/ECPAT0.BIN";
            path[6] = '0' + (bank == 0 ? 1 : 0);
            if (!save_exact(path, SEL_RECORD, 16))
                return 0;
        }
        // Save the last valid selection in QSPI before erasing the internal flash journal.
        if (!selection_erase())
            return 0;
        address = SEL_FIRST;
    }
    if (!selection_program(address, record))
        return 0;
    for (unsigned i = 0; i < 16; i++)
        SEL_RECORD[i] = record[i];
    SEL_READY = 2;
    return 1;
}

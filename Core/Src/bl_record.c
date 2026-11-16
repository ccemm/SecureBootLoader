/**
  * @file    bl_record.c
  * @brief   Image manifest and the A/B boot record kept in data EEPROM.
  */
#include <stddef.h>
#include <string.h>
#include "bl_record.h"
#include "bl_config.h"
#include "bl_crc.h"
#include "bl_debug.h"
#include "stm32l1xx_hal.h"

#define REC_CRC_LEN   offsetof(bl_boot_record_t, crc32)
#define REC_WORDS     (sizeof(bl_boot_record_t) / 4U)

static const uint32_t copy_addr[2] = { BL_RECORD_A_ADDR, BL_RECORD_B_ADDR };

static bool copy_valid(uint32_t addr)
{
    const bl_boot_record_t *r = (const bl_boot_record_t *)addr;

    // Erased EEPROM (all zeros) fails this check
    return crc32_iso_hdlc((const uint8_t *)r, REC_CRC_LEN) == r->crc32;
}

/* Index of the current copy, or -1 if neither is valid */
static int current_copy(void)
{
    bool va = copy_valid(copy_addr[0]);
    bool vb = copy_valid(copy_addr[1]);

    if (va && vb)
        return (((const bl_boot_record_t *)copy_addr[0])->write_counter >=
                ((const bl_boot_record_t *)copy_addr[1])->write_counter) ? 0 : 1;
    if (va)
        return 0;
    if (vb)
        return 1;
    return -1;
}

bool bl_record_read(bl_boot_record_t *rec)
{
    int cur = current_copy();

    if (cur < 0)
        return false;
    memcpy(rec, (const void *)copy_addr[cur], sizeof(*rec));
    return true;
}

bool bl_record_write(const bl_manifest_t *manifest, uint32_t state)
{
    bl_boot_record_t rec;
    int cur = current_copy();
    uint32_t target = copy_addr[(cur == 0) ? 1 : 0];
    bool ok = true;

    memset(&rec, 0, sizeof(rec));
    rec.manifest = *manifest;
    rec.state = state;
    if (cur >= 0) {
        const bl_boot_record_t *old = (const bl_boot_record_t *)copy_addr[cur];
        rec.write_counter = old->write_counter + 1U;
        rec.rollback_counter = old->rollback_counter;
    } else {
        rec.write_counter = 1U;
    }
    rec.crc32 = crc32_iso_hdlc((const uint8_t *)&rec, REC_CRC_LEN);

    HAL_FLASHEx_DATAEEPROM_Unlock();
    for (uint32_t i = 0; i < REC_WORDS && ok; i++) {
        uint32_t addr = target + 4U * i;
        uint32_t word;

        memcpy(&word, (const uint8_t *)&rec + 4U * i, 4);
        if (*(volatile uint32_t *)addr == word)
            continue;                           // skip unchanged words, saves time and wear
        ok = (HAL_FLASHEx_DATAEEPROM_Program(FLASH_TYPEPROGRAMDATA_WORD, addr, word) == HAL_OK);
    }
    HAL_FLASHEx_DATAEEPROM_Lock();

    if (ok)
        ok = (memcmp((const void *)target, &rec, sizeof(rec)) == 0);

    bl_log("Boot record copy %c: state=%lu counter=%lu %s",
           (target == BL_RECORD_A_ADDR) ? 'A' : 'B', state, rec.write_counter,
           ok ? "written" : "WRITE FAILED");
    return ok;
}

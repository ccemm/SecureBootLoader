/**
  * @file    bl_boot.c
  * @brief   Boot decision helpers and the jump to the application.
  */
#include "bl_boot.h"
#include "bl_config.h"
#include "bl_crc.h"
#include "bl_debug.h"
#include "main.h"

#define BOOT_FLAG (*(volatile uint32_t *)BL_BOOT_FLAG_ADDR)

typedef void (*pFunction)(void);

bool bl_boot_flag_check_and_clear(void)
{
    bool requested = (BOOT_FLAG == BL_BOOT_FLAG_MAGIC);

    BOOT_FLAG = 0;
    return requested;
}

bool bl_force_pin_active(void)
{
    // B1 on the Discovery board has an external pull-down, pressed = high
    return HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_SET;
}

bool bl_vectors_valid(uint32_t image_addr)
{
    uint32_t appStack = *(volatile uint32_t *)image_addr;         // word 0: initial MSP
    uint32_t appEntry = *(volatile uint32_t *)(image_addr + 4);   // word 1: Reset_Handler

    // Erased flash reads 0x00000000 on STM32L1, which fails both checks
    if (appStack < BL_SRAM_START || appStack > BL_SRAM_END)
        return false;
    if (appEntry < BL_APP_START || appEntry >= BL_APP_END || (appEntry & 1U) == 0U)
        return false;

    return true;
}

bl_app_state_t bl_app_check(bl_boot_record_t *rec)
{
    const bl_manifest_t *m = &rec->manifest;
    uint32_t crc;

    if (!bl_record_read(rec) || rec->state == REC_EMPTY) {
        bl_log("App check: no boot record");
        return APP_NONE;
    }
    if (rec->state != REC_VALID) {
        bl_log("App check: update was not completed (record state %lu)", rec->state);
        return APP_INVALID;
    }
    if (m->load_address != BL_APP_START || m->image_size == 0U || m->image_size > BL_APP_MAX_SIZE) {
        bl_log("App check: manifest address/size out of range");
        return APP_INVALID;
    }

    crc = crc32_iso_hdlc((const uint8_t *)BL_APP_START, m->image_size);
    if (crc != m->image_crc32) {
        bl_log("App check: CRC mismatch, flash 0x%08lX, manifest 0x%08lX", crc, m->image_crc32);
        return APP_INVALID;
    }
    if (!bl_vectors_valid(BL_APP_START)) {
        bl_log("App check: invalid vector table");
        return APP_INVALID;
    }

    bl_log("App check: valid, version 0x%08lX, %lu bytes", m->image_version, m->image_size);
    return APP_VALID;
}

bool bl_app_is_valid(void)
{
    bl_boot_record_t rec;
    bl_app_state_t app = bl_app_check(&rec);

#if BL_DEV_ACCEPT_UNRECORDED_APP
    // GET_APP_INFO still reports "none"; only the boot decision is relaxed
    if (app == APP_NONE && bl_vectors_valid(BL_APP_START)) {
        bl_log("DEV: no boot record, accepting app programmed without the bootloader");
        return true;
    }
#endif

    return app == APP_VALID;
}

void bl_jump_to_app(void)
{
    uint32_t appStack = *(volatile uint32_t *)BL_APP_START;
    pFunction appResetHandler = (pFunction)(*(volatile uint32_t *)(BL_APP_START + 4));

    bl_log("Starting application");

    __disable_irq();

    // Put peripherals back in their reset state
    HAL_RCC_DeInit();
    HAL_DeInit();

    // Stop SysTick
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;   // HAL_RCC_DeInit re-armed SysTick, drop any pending tick

    // Disable all IRQs and clear pending ones
    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    // Relocate the vector table
    SCB->VTOR = BL_APP_START;
    __DSB();
    __ISB();

    // Set the app's main stack pointer
    __set_MSP(appStack);

    __enable_irq();

    // Jump
    appResetHandler();

    while (1);   // should never get here
}

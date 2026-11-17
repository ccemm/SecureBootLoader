/**
  * @file    bl_boot.c
  * @brief   Boot decision helpers and the jump to the application.
  */
#include "bl_boot.h"
#include "bl_config.h"
#include "bl_crc.h"
#include "bl_debug.h"
#include "bl_ecdsa.h"
#include "bl_sha256.h"
#include "main.h"
#include <stddef.h>
#include <string.h>

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

#if BL_DEBUG
static void log_hash(const char *label, const uint8_t *h)
{
    char hex[2U * SHA256_SIZE + 1U];

    for (uint32_t i = 0; i < SHA256_SIZE; i++) {
        hex[2U * i]      = "0123456789abcdef"[h[i] >> 4];
        hex[2U * i + 1U] = "0123456789abcdef"[h[i] & 0x0FU];
    }
    hex[2U * SHA256_SIZE] = '\0';
    bl_log("%s %s", label, hex);
}
#else
#define log_hash(label, h)  ((void)0)
#endif

bool bl_image_sha256_ok(const bl_manifest_t *m)
{
    uint8_t digest[SHA256_SIZE];
    uint32_t t0 = HAL_GetTick();

    sha256((const uint8_t *)BL_APP_START, m->image_size, digest);
    if (memcmp(digest, m->image_sha256, SHA256_SIZE) != 0) {
        bl_log("SHA-256 mismatch:");
        log_hash("  flash   ", digest);
        log_hash("  manifest", m->image_sha256);
        return false;
    }

    bl_log("SHA-256 ok (%lu ms)", HAL_GetTick() - t0);
    log_hash("  flash   ", digest);
    return true;
}

bool bl_manifest_signature_ok(const bl_manifest_t *m)
{
#if BL_SECURITY_LEVEL >= 1
    uint32_t t0 = HAL_GetTick();
    bool ok;

    if ((m->flags & BL_FLAG_SIGNED) == 0U) {
        bl_log("Signature: manifest is not signed");
        return false;
    }
    ok = bl_ecdsa_verify(bl_public_key, (const uint8_t *)m, offsetof(bl_manifest_t, signature), m->signature);
    bl_log("Signature %s (%lu ms)", ok ? "ok" : "INVALID", HAL_GetTick() - t0);
    return ok;
#else
    (void)m;
    return true;
#endif
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
    // The record is in EEPROM, which the application can write, so the stored
    // manifest is re-authenticated on every check, not only at UPDATE_BEGIN
    if (!bl_manifest_signature_ok(m)) {
        bl_log("App check: manifest signature not valid");
        return APP_INVALID;
    }

    crc = crc32_iso_hdlc((const uint8_t *)BL_APP_START, m->image_size);
    if (crc != m->image_crc32) {
        bl_log("App check: CRC mismatch, flash 0x%08lX, manifest 0x%08lX", crc, m->image_crc32);
        return APP_INVALID;
    }
    if (!bl_image_sha256_ok(m)) {
        bl_log("App check: SHA-256 mismatch");
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

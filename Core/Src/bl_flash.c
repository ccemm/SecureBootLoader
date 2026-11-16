/**
  * @file    bl_flash.c
  * @brief   Application flash programming (erase + half-page write + read-back).
  *
  * HAL_FLASHEx_HalfPageProgram runs from SRAM (.RamFunc) with interrupts
  * disabled for ~3 ms per half page. No flash reads may happen meanwhile,
  * which includes debugger reads: do not use Live Expressions or breakpoints
  * while an update is being written.
  */
#include <string.h>
#include "bl_flash.h"
#include "bl_config.h"
#include "stm32l1xx_hal.h"

#define HALF_PAGE_SIZE   (FLASH_PAGE_SIZE / 2U)

bl_flash_result_t bl_flash_write_page(uint32_t addr, const uint32_t *data)
{
    FLASH_EraseInitTypeDef erase = {
        .TypeErase   = FLASH_TYPEERASE_PAGES,
        .PageAddress = addr,
        .NbPages     = 1,
    };
    uint32_t page_error;
    bl_flash_result_t res = BL_FLASH_OK;

    // Never trust the caller's offset: this is what keeps the bootloader safe
    if ((addr % FLASH_PAGE_SIZE) != 0U || addr < BL_APP_START || addr > BL_APP_END - FLASH_PAGE_SIZE)
        return BL_FLASH_BAD_ADDRESS;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_MASK);

    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK)
        res = BL_FLASH_ERASE_FAILED;
    else if (HAL_FLASHEx_HalfPageProgram(addr, (uint32_t *)data) != HAL_OK ||
             HAL_FLASHEx_HalfPageProgram(addr + HALF_PAGE_SIZE, (uint32_t *)data + HALF_PAGE_SIZE / 4U) != HAL_OK)
        res = BL_FLASH_WRITE_FAILED;

    HAL_FLASH_Lock();

    if (res == BL_FLASH_OK && memcmp((const void *)addr, data, FLASH_PAGE_SIZE) != 0)
        res = BL_FLASH_WRITE_FAILED;

    return res;
}

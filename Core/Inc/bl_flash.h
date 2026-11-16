/**
  * @file    bl_flash.h
  * @brief   Application flash programming (erase + half-page write + read-back).
  */
#ifndef BL_FLASH_H
#define BL_FLASH_H

#include <stdint.h>

typedef enum {
    BL_FLASH_OK,
    BL_FLASH_BAD_ADDRESS,
    BL_FLASH_ERASE_FAILED,
    BL_FLASH_WRITE_FAILED,
} bl_flash_result_t;

/* Erase the 256-byte page at addr, program it with data (64 words, must be in
   SRAM) and read it back. addr must be page aligned and inside the application
   region; anything else is refused before the flash is unlocked. */
bl_flash_result_t bl_flash_write_page(uint32_t addr, const uint32_t *data);

#endif /* BL_FLASH_H */

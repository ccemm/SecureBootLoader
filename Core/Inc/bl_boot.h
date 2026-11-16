/**
  * @file    bl_boot.h
  * @brief   Boot decision helpers and the jump to the application.
  */
#ifndef BL_BOOT_H
#define BL_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include "bl_record.h"

/* Application state, values as reported by GET_APP_INFO */
typedef enum {
    APP_NONE    = 0,   /* no boot record */
    APP_VALID   = 1,
    APP_INVALID = 2,   /* update in progress / interrupted, or CRC mismatch */
} bl_app_state_t;

/* True if the application requested the bootloader via the SRAM flag. Clears the flag. */
bool bl_boot_flag_check_and_clear(void);

/* True if the forced-entry pin (user button B1 / PA0) is held */
bool bl_force_pin_active(void);

/* True if the first two words of an image look like a vector table for this device */
bool bl_vectors_valid(uint32_t image_addr);

/* Check the installed application against the boot record: state VALID,
   CRC-32 of the flash image and vector table. rec receives the current record. */
bl_app_state_t bl_app_check(bl_boot_record_t *rec);

/* Boot decision: bl_app_check() == APP_VALID, or, with BL_DEV_ACCEPT_UNRECORDED_APP,
   no record at all plus a valid vector table */
bool bl_app_is_valid(void);

/* De-initialise everything and start the application. Does not return. */
void bl_jump_to_app(void);

#endif /* BL_BOOT_H */

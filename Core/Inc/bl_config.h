/**
  * @file    bl_config.h
  * @brief   Bootloader build configuration and memory map
  *          (UART Firmware Update Protocol v1.0, simple bootloader).
  */
#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/* Bootloader version reported by GET_DEVICE_INFO */
#define BL_VERSION_MAJOR        1U
#define BL_VERSION_MINOR        0U
#define BL_VERSION_PATCH        0U

/* Protocol version implemented */
#define BL_PROTO_MAJOR          1U
#define BL_PROTO_MINOR          0U

/* Product/board identifier, images must carry the same value in manifest.hw_id */
#define BL_HW_ID                0x0001U   /* STM32L100C-Discovery */

/* Build options */
#define BL_DEBUG                1         /* debug log on USART2 */

/* Security level reported by CONNECT.
   0 = integrity only (CRC-32 + SHA-256)
   1 = signed images required: UPDATE_BEGIN and every boot check the ECDSA P-256
       signature of the manifest with the public key in bl_public_key.c */
#define BL_SECURITY_LEVEL       1
#define BL_ENABLE_READ_MEMORY   1         /* READ_MEMORY command (development builds) */

/* Development only: if there is NO boot record at all (app programmed with
   ST-LINK, never installed through the bootloader) and the vector table looks
   valid, start the app anyway. A failed or interrupted update always leaves an
   "in progress" record, so it is still never started. Set to 0 for production. */
#define BL_DEV_ACCEPT_UNRECORDED_APP  1

/* Memory map (section 2 of the protocol spec) */
#define BL_APP_START            0x08008000UL
#define BL_APP_END              0x08040000UL   /* end of 256 KB flash */
#define BL_APP_MAX_SIZE         (BL_APP_END - BL_APP_START)
#define BL_SRAM_START           0x20000000UL
#define BL_SRAM_END             0x20004000UL   /* 16 KB SRAM */
#define BL_EEPROM_START         0x08080000UL
#define BL_EEPROM_END           0x08081000UL   /* 4 KB data EEPROM */

/* Boot metadata: two 160-byte boot record copies (A/B).
   0x08080000 - 0x080801FF is reserved for the bootloader (512 B, the spec's
   256 B cannot hold two copies); the application may use 0x08080200 onwards. */
#define BL_RECORD_A_ADDR        0x08080000UL
#define BL_RECORD_B_ADDR        0x080800A0UL

/* Boot request flag in the last 16 bytes of SRAM (excluded from both linker scripts) */
#define BL_BOOT_FLAG_ADDR       0x20003FF0UL
#define BL_BOOT_FLAG_MAGIC      0xB00710ADUL

/* Image manifest */
#define BL_MANIFEST_MAGIC       0x464D4C42UL   /* "BLMF" in memory */
#define BL_MANIFEST_VERSION     1U
#define BL_FLAG_SIGNED          (1UL << 0)
#define BL_FLAG_ENCRYPTED       (1UL << 1)

/* Protocol limits and timing */
#define BL_MAX_PAYLOAD          264U
#define BL_BLOCK_SIZE           256U
#define BL_INTERBYTE_TIMEOUT_MS 50U
#define BL_UPDATE_TIMEOUT_MS    30000U   /* host silent this long during an update -> abort, back to IDLE */

#endif /* BL_CONFIG_H */

/**
  * @file    bl_record.h
  * @brief   Image manifest and the A/B boot record kept in data EEPROM.
  */
#ifndef BL_RECORD_H
#define BL_RECORD_H

#include <stdbool.h>
#include <stdint.h>

/* 128-byte image manifest (spec section 7) */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* 0x464D4C42 */
    uint16_t manifest_version;  /* 1 */
    uint16_t hw_id;
    uint32_t flags;             /* bit0 SIGNED, bit1 ENCRYPTED */
    uint32_t image_version;
    uint32_t security_counter;
    uint32_t load_address;
    uint32_t image_size;
    uint32_t image_crc32;
    uint8_t  image_sha256[32];
    uint8_t  signature[64];     /* covers bytes 0..63 */
} bl_manifest_t;

_Static_assert(sizeof(bl_manifest_t) == 128, "manifest must be 128 bytes");

/* Boot record states */
enum {
    REC_EMPTY       = 0,
    REC_IN_PROGRESS = 1,   /* written by UPDATE_BEGIN: the image in flash must not be started */
    REC_VALID       = 2,   /* written by UPDATE_COMMIT */
};

/* 160-byte boot record, two copies in data EEPROM; the valid copy with the
   higher write_counter is the current one */
typedef struct __attribute__((packed)) {
    bl_manifest_t manifest;
    uint32_t state;
    uint32_t write_counter;
    uint32_t rollback_counter;  /* unused by the simple bootloader, carried over */
    uint32_t reserved[4];
    uint32_t crc32;             /* CRC-32 of all preceding bytes */
} bl_boot_record_t;

_Static_assert(sizeof(bl_boot_record_t) == 160, "boot record must be 160 bytes");

/* Copy the current boot record into rec. Returns false if neither copy is valid. */
bool bl_record_read(bl_boot_record_t *rec);

/* Write a new record (manifest + state) into the inactive copy and verify it.
   If power fails during the write the previous copy stays current. */
bool bl_record_write(const bl_manifest_t *manifest, uint32_t state);

#endif /* BL_RECORD_H */

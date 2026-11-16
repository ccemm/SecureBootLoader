/**
  * @file    bl_crc.h
  * @brief   CRC routines used by the bootloader protocol.
  */
#ifndef BL_CRC_H
#define BL_CRC_H

#include <stdint.h>

#define CRC16_INIT  0xFFFFU

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR */
uint16_t crc16_ccitt(const uint8_t *d, uint32_t n, uint16_t crc);

/* CRC-32/ISO-HDLC, same result as zlib / Python binascii.crc32 */
uint32_t crc32_iso_hdlc(const uint8_t *d, uint32_t n);

#endif /* BL_CRC_H */

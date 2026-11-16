/**
  * @file    bl_crc.c
  * @brief   CRC routines used by the bootloader protocol.
  */
#include "bl_crc.h"

uint16_t crc16_ccitt(const uint8_t *d, uint32_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Nibble table for the reflected polynomial 0xEDB88320: 64 bytes of flash,
   about 150 ms for a full 224 KB image at 32 MHz */
static const uint32_t crc32_tab[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t crc32_iso_hdlc(const uint8_t *d, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFU;

    while (n--) {
        crc ^= *d++;
        crc = (crc >> 4) ^ crc32_tab[crc & 0x0FU];
        crc = (crc >> 4) ^ crc32_tab[crc & 0x0FU];
    }
    return crc ^ 0xFFFFFFFFU;
}

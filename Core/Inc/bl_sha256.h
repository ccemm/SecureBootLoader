/**
  * @file    bl_sha256.h
  * @brief   SHA-256 (FIPS 180-4), used to check the image against manifest.image_sha256.
  */
#ifndef BL_SHA256_H
#define BL_SHA256_H

#include <stdbool.h>
#include <stdint.h>

#define SHA256_SIZE  32U

/* SHA-256 of n bytes at d. The data is read in place, so it can be the image in flash. */
void sha256(const uint8_t *d, uint32_t n, uint8_t out[SHA256_SIZE]);

/* Known-answer test with the FIPS 180-4 example messages (one and two padding blocks) */
bool sha256_self_test(void);

#endif /* BL_SHA256_H */

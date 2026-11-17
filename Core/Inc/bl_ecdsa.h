/**
  * @file    bl_ecdsa.h
  * @brief   ECDSA P-256 (secp256r1) signature verification with SHA-256, via micro-ecc.
  */
#ifndef BL_ECDSA_H
#define BL_ECDSA_H

#include <stdbool.h>
#include <stdint.h>

#define ECDSA_PUBKEY_SIZE  64U   /* X || Y, big-endian (uncompressed point without the 0x04 prefix) */
#define ECDSA_SIG_SIZE     64U   /* r || s, big-endian, 32 bytes each (not DER) */

/* Firmware signing public key, defined in bl_public_key.c */
extern const uint8_t bl_public_key[ECDSA_PUBKEY_SIZE];

/* True if sig is a valid ECDSA-SHA256 signature of msg[0..n) for pubkey */
bool bl_ecdsa_verify(const uint8_t pubkey[ECDSA_PUBKEY_SIZE], const uint8_t *msg, uint32_t n,
                     const uint8_t sig[ECDSA_SIG_SIZE]);

/* Known-answer test with a fixed key and signature made by OpenSSL; also checks
   that a corrupted signature is rejected */
bool bl_ecdsa_self_test(void);

#endif /* BL_ECDSA_H */

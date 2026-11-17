/**
  * @file    bl_ecdsa.c
  * @brief   ECDSA P-256 signature verification with SHA-256, via micro-ecc v1.1
  *          (ThirdParty/micro-ecc, BSD 2-clause).
  *
  * micro-ecc is compiled here rather than as its own source file, so its
  * configuration stays in this project and the library files stay unmodified.
  * Only secp256r1 verification is needed; the other curves are left out.
  */
#include <string.h>
#include "bl_ecdsa.h"
#include "bl_sha256.h"

#define uECC_SUPPORTS_secp160r1        0
#define uECC_SUPPORTS_secp192r1        0
#define uECC_SUPPORTS_secp224r1        0
#define uECC_SUPPORTS_secp256r1        1
#define uECC_SUPPORTS_secp256k1        0
#define uECC_SUPPORT_COMPRESSED_POINT  0

// The ARM inline assembly in micro-ecc needs more registers than -O0 leaves free,
// so the library is always built optimised, also in Debug builds
#pragma GCC push_options
#pragma GCC optimize ("O2", "omit-frame-pointer")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../../ThirdParty/micro-ecc/uECC.c"
#pragma GCC diagnostic pop
#pragma GCC pop_options

bool bl_ecdsa_verify(const uint8_t pubkey[ECDSA_PUBKEY_SIZE], const uint8_t *msg, uint32_t n,
                     const uint8_t sig[ECDSA_SIG_SIZE])
{
    uint8_t hash[SHA256_SIZE];

    sha256(msg, n, hash);
    return uECC_verify(pubkey, hash, sizeof(hash), sig, uECC_secp256r1()) == 1;
}

bool bl_ecdsa_self_test(void)
{
    // Test key pair generated with OpenSSL only for this test (not the signing key).
    // Signature: openssl dgst -sha256 -sign over the 3 bytes "abc", converted from DER to r || s.
    static const uint8_t pub[ECDSA_PUBKEY_SIZE] = {
        0xfa, 0xbf, 0x42, 0x1c, 0x63, 0xb3, 0x54, 0x4d, 0x36, 0xc3, 0x2a, 0xa8, 0xf3, 0x53, 0x24, 0x99,
        0x11, 0x7f, 0x11, 0x22, 0xa8, 0x2a, 0x70, 0x23, 0x5a, 0x62, 0xfb, 0x0e, 0xf3, 0x48, 0x25, 0xd5,
        0xa3, 0x65, 0xaa, 0x4e, 0x56, 0x37, 0x67, 0xd2, 0xa2, 0x05, 0xea, 0x24, 0xd2, 0x11, 0x77, 0x51,
        0x64, 0x08, 0x1e, 0x4d, 0xe3, 0x71, 0x65, 0xeb, 0x8f, 0x4b, 0x1a, 0x85, 0x0d, 0x58, 0x44, 0xfb,
    };
    static const uint8_t sig[ECDSA_SIG_SIZE] = {
        0x79, 0x6d, 0x18, 0x85, 0x5d, 0xef, 0x73, 0xf1, 0x1c, 0xfd, 0xff, 0xeb, 0xb3, 0xf6, 0x91, 0xe8,
        0xb8, 0xad, 0xe7, 0x6f, 0xc0, 0x27, 0x96, 0xcb, 0x32, 0xce, 0xaa, 0x78, 0x6f, 0xf4, 0xef, 0x97,
        0x2e, 0xe4, 0xfd, 0x07, 0xcb, 0xc2, 0x5d, 0x96, 0x8c, 0x34, 0xdd, 0x5e, 0xdf, 0x5b, 0x6f, 0xe4,
        0xa7, 0x86, 0xed, 0xd2, 0xa4, 0x64, 0x2d, 0xca, 0x36, 0x24, 0xc1, 0xc5, 0xe2, 0x99, 0xce, 0xa0,
    };
    static const uint8_t msg[] = { 'a', 'b', 'c' };
    uint8_t bad[ECDSA_SIG_SIZE];

    if (!bl_ecdsa_verify(pub, msg, sizeof(msg), sig))
        return false;

    memcpy(bad, sig, sizeof(bad));
    bad[ECDSA_SIG_SIZE - 1U] ^= 0x01U;
    return !bl_ecdsa_verify(pub, msg, sizeof(msg), bad);
}

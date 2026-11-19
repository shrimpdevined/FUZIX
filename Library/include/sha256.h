/*
 * sha256.h - SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256
 *
 * Small, dependency free implementation used by the password hashing
 * code in libc.  The API is deliberately private to the C library; the
 * public password interface is crypt(3).
 */

#ifndef _SHA256_H
#define _SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LENGTH	32
#define SHA256_BLOCK_LENGTH	64

struct sha256_ctx {
	uint32_t	state[8];
	uint32_t	count;			/* bytes fed so far */
	uint8_t		block[SHA256_BLOCK_LENGTH];
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LENGTH]);

void hmac_sha256(const void *key, size_t keylen,
		 const void *data, size_t datalen,
		 uint8_t out[SHA256_DIGEST_LENGTH]);

void pbkdf2_sha256(const void *pass, size_t passlen,
		   const void *salt, size_t saltlen,
		   uint32_t iterations,
		   uint8_t *out, size_t outlen);

#endif /* _SHA256_H */

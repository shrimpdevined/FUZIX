/*
 * sha256.c - SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256
 *
 * FIPS 180-4 SHA-256, RFC 2104 HMAC and RFC 8018 PBKDF2.  Only 32 bit
 * arithmetic is used so the code runs on the smallest supported CPUs.
 *
 * This file is part of FUZIX.
 */

#include <string.h>
#include "sha256.h"

static const uint32_t K[64] = {
	0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
	0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
	0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
	0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
	0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
	0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
	0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
	0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
	0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
	0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
	0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
	0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
	0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
	0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
	0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
	0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

#define ROR(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t *data)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t t1, t2, s0, s1;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)data[i * 4] << 24) |
		       ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) |
		       (uint32_t)data[i * 4 + 3];

	for (i = 16; i < 64; i++) {
		s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^
		     (w[i - 15] >> 3);
		s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^
		     (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		s0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
		     ((e & f) ^ (~e & g)) + K[i] + w[i];
		s1 = (a & b) ^ (a & c) ^ (b & c);
		t2 = s0 + s1;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
	ctx->state[0] = 0x6a09e667UL;
	ctx->state[1] = 0xbb67ae85UL;
	ctx->state[2] = 0x3c6ef372UL;
	ctx->state[3] = 0xa54ff53aUL;
	ctx->state[4] = 0x510e527fUL;
	ctx->state[5] = 0x9b05688cUL;
	ctx->state[6] = 0x1f83d9abUL;
	ctx->state[7] = 0x5be0cd19UL;
	ctx->count = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t fill, left;

	left = ctx->count & 63;
	ctx->count += (uint32_t)len;

	if (left) {
		fill = 64 - left;
		if (len < fill) {
			memcpy(ctx->block + left, p, len);
			return;
		}
		memcpy(ctx->block + left, p, fill);
		sha256_transform(ctx, ctx->block);
		p += fill;
		len -= fill;
	}

	while (len >= 64) {
		sha256_transform(ctx, p);
		p += 64;
		len -= 64;
	}

	if (len)
		memcpy(ctx->block, p, len);
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LENGTH])
{
	uint32_t bits_hi, bits_lo;
	uint32_t used;
	int i;

	bits_hi = ctx->count >> 29;
	bits_lo = ctx->count << 3;
	used = ctx->count & 63;

	ctx->block[used++] = 0x80;

	if (used > 56) {
		memset(ctx->block + used, 0, 64 - used);
		sha256_transform(ctx, ctx->block);
		used = 0;
	}
	memset(ctx->block + used, 0, 56 - used);

	ctx->block[56] = (uint8_t)(bits_hi >> 24);
	ctx->block[57] = (uint8_t)(bits_hi >> 16);
	ctx->block[58] = (uint8_t)(bits_hi >> 8);
	ctx->block[59] = (uint8_t)bits_hi;
	ctx->block[60] = (uint8_t)(bits_lo >> 24);
	ctx->block[61] = (uint8_t)(bits_lo >> 16);
	ctx->block[62] = (uint8_t)(bits_lo >> 8);
	ctx->block[63] = (uint8_t)bits_lo;

	sha256_transform(ctx, ctx->block);

	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)ctx->state[i];
	}
}

void hmac_sha256(const void *key, size_t keylen,
		 const void *data, size_t datalen,
		 uint8_t out[SHA256_DIGEST_LENGTH])
{
	uint8_t k[SHA256_BLOCK_LENGTH];
	uint8_t pad[SHA256_BLOCK_LENGTH];
	uint8_t inner[SHA256_DIGEST_LENGTH];
	struct sha256_ctx ctx;
	size_t i;

	memset(k, 0, sizeof(k));
	if (keylen > SHA256_BLOCK_LENGTH) {
		sha256_init(&ctx);
		sha256_update(&ctx, key, keylen);
		sha256_final(&ctx, k);
	} else if (keylen)
		memcpy(k, key, keylen);

	for (i = 0; i < SHA256_BLOCK_LENGTH; i++)
		pad[i] = (uint8_t)(k[i] ^ 0x36);
	sha256_init(&ctx);
	sha256_update(&ctx, pad, sizeof(pad));
	if (datalen)
		sha256_update(&ctx, data, datalen);
	sha256_final(&ctx, inner);

	for (i = 0; i < SHA256_BLOCK_LENGTH; i++)
		pad[i] = (uint8_t)(k[i] ^ 0x5c);
	sha256_init(&ctx);
	sha256_update(&ctx, pad, sizeof(pad));
	sha256_update(&ctx, inner, sizeof(inner));
	sha256_final(&ctx, out);
}

void pbkdf2_sha256(const void *pass, size_t passlen,
		   const void *salt, size_t saltlen,
		   uint32_t iterations,
		   uint8_t *out, size_t outlen)
{
	uint8_t u[SHA256_DIGEST_LENGTH];
	uint8_t t[SHA256_DIGEST_LENGTH];
	uint8_t sbuf[68];
	uint32_t block = 1;
	uint32_t i, j;
	size_t pos = 0;
	size_t use;

	if (iterations == 0)
		iterations = 1;

	/* RFC 8018 allows arbitrarily long salts.  Keep the common case
	 * (a 16 byte salt) on the stack and hash anything longer. */
	if (saltlen > sizeof(sbuf) - 4) {
		hmac_sha256("salt", 4, salt, saltlen, sbuf);
		saltlen = SHA256_DIGEST_LENGTH;
	} else {
		memcpy(sbuf, salt, saltlen);
	}

	while (pos < outlen) {
		sbuf[saltlen] = (uint8_t)(block >> 24);
		sbuf[saltlen + 1] = (uint8_t)(block >> 16);
		sbuf[saltlen + 2] = (uint8_t)(block >> 8);
		sbuf[saltlen + 3] = (uint8_t)block;

		hmac_sha256(pass, passlen, sbuf, saltlen + 4, u);
		memcpy(t, u, sizeof(t));

		for (i = 1; i < iterations; i++) {
			hmac_sha256(pass, passlen, u, sizeof(u), u);
			for (j = 0; j < sizeof(t); j++)
				t[j] ^= u[j];
		}

		use = outlen - pos;
		if (use > sizeof(t))
			use = sizeof(t);
		memcpy(out + pos, t, use);
		pos += use;
		block++;
	}

	memset(u, 0, sizeof(u));
	memset(t, 0, sizeof(t));
	memset(sbuf, 0, sizeof(sbuf));
}

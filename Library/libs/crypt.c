/*
 * crypt.c - password hashing for FUZIX
 *
 * The traditional FUZIX crypt() was a home grown TEA construction.  It
 * was neither a password stretching function nor compatible with any
 * other system, so it provided no meaningful protection against an
 * attacker who obtained /etc/passwd or /etc/shadow.
 *
 * This implementation uses PBKDF2-HMAC-SHA-256 (RFC 8018) with a
 * salt stored alongside the cost parameter in the widely used modular
 * crypt format:
 *
 *	$pbkdf2-sha256$<iterations>$<salt>$<derived key>
 *
 * Salt and derived key use the traditional crypt base64 alphabet
 * ("./0-9A-Za-z") without padding.  Because the iteration count and
 * salt are part of the stored string, the cost can be raised in the
 * future without invalidating existing passwords.
 *
 * crypt(3) follows the usual UNIX convention: given a stored hash as
 * the "salt" argument it recomputes the hash, and the caller compares
 * the result.  When the salt argument is not a modular crypt string a
 * fresh salt is generated, which is what crypt_generate() uses.
 *
 * This file is part of FUZIX.
 */

#include <features.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "sha256.h"

#define PBKDF2_SCHEME		"$pbkdf2-sha256$"
#define PBKDF2_DEFAULT_ITER	10000
#define PBKDF2_MAX_ITER		1000000
#define PBKDF2_SALT_BYTES	16

static const char b64_alphabet[] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/*
 *	Base64 encoding using the crypt alphabet, no padding.
 */
static void b64_encode(const uint8_t *in, size_t len, char *out)
{
	size_t i = 0;
	size_t o = 0;

	while (i + 2 < len) {
		out[o++] = b64_alphabet[in[i] >> 2];
		out[o++] = b64_alphabet[((in[i] & 0x03) << 4) |
					 (in[i + 1] >> 4)];
		out[o++] = b64_alphabet[((in[i + 1] & 0x0f) << 2) |
					 (in[i + 2] >> 6)];
		out[o++] = b64_alphabet[in[i + 2] & 0x3f];
		i += 3;
	}
	if (len - i == 1) {
		out[o++] = b64_alphabet[in[i] >> 2];
		out[o++] = b64_alphabet[(in[i] & 0x03) << 4];
	} else if (len - i == 2) {
		out[o++] = b64_alphabet[in[i] >> 2];
		out[o++] = b64_alphabet[((in[i] & 0x03) << 4) |
					 (in[i + 1] >> 4)];
		out[o++] = b64_alphabet[(in[i + 1] & 0x0f) << 2];
	}
	out[o] = '\0';
}

static int b64_value(char c)
{
	const char *p = strchr(b64_alphabet, c);

	if (p == NULL || c == '\0')
		return -1;
	return (int)(p - b64_alphabet);
}

static int b64_decode(const char *in, size_t len, uint8_t *out, size_t outlen)
{
	uint32_t acc = 0;
	int bits = 0;
	size_t i, o = 0;

	for (i = 0; i < len; i++) {
		int v = b64_value(in[i]);

		if (v < 0)
			return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= outlen)
				return -1;
			out[o++] = (uint8_t)(acc >> bits);
		}
	}
	return (int)o;
}

/*
 *	Parse "$pbkdf2-sha256$<iter>$<salt>[$<hash>]".
 */
static int parse_hash(const char *stored, uint32_t *iterations,
		      uint8_t *salt, size_t *saltlen)
{
	const char *p, *q;
	char num[16];
	int n, got;

	if (strncmp(stored, PBKDF2_SCHEME, sizeof(PBKDF2_SCHEME) - 1))
		return -1;

	p = stored + sizeof(PBKDF2_SCHEME) - 1;
	q = strchr(p, '$');
	if (q == NULL)
		return -1;
	n = (int)(q - p);
	if (n <= 0 || n >= (int)sizeof(num))
		return -1;
	memcpy(num, p, n);
	num[n] = '\0';
	*iterations = (uint32_t)strtoul(num, NULL, 10);
	if (*iterations == 0 || *iterations > PBKDF2_MAX_ITER)
		return -1;

	p = q + 1;
	q = strchr(p, '$');
	if (q == NULL)
		q = p + strlen(p);
	got = b64_decode(p, (size_t)(q - p), salt, 32);
	if (got < 1)
		return -1;
	*saltlen = (size_t)got;
	return 0;
}

static void format_hash(char *out, size_t outlen, uint32_t iterations,
			const uint8_t *salt, size_t saltlen,
			const uint8_t *key, size_t keylen)
{
	char sbuf[64];
	char kbuf[64];

	b64_encode(salt, saltlen, sbuf);
	b64_encode(key, keylen, kbuf);
	snprintf(out, outlen, "%s%lu$%s$%s", PBKDF2_SCHEME,
		 (unsigned long)iterations, sbuf, kbuf);
}

/*
 *	Salt generation.
 *
 * FUZIX has no /dev/urandom yet.  Use it when the running system
 * provides one, otherwise stir everything cheap that differs between
 * calls (time, pid, stack address and a counter) through SHA-256.
 * This is not a replacement for a hardware random number generator;
 * the kernel port should grow a proper entropy source.
 */
static uint32_t entropy_counter;

static void fill_random(uint8_t *out, size_t len)
{
	uint8_t seed[64];
	time_t now;
	pid_t pid;
	uint32_t counter;
	size_t got = 0;
	size_t o;
	int fd;
	struct sha256_ctx ctx;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		while (got < len) {
			int n = read(fd, out + got, len - got);

			if (n <= 0)
				break;
			got += (size_t)n;
		}
		close(fd);
		if (got == len)
			return;
	}

	memset(seed, 0, sizeof(seed));
	now = time(NULL);
	pid = getpid();
	counter = ++entropy_counter;

	o = 0;
	memcpy(seed + o, &now, sizeof(now));
	o += sizeof(now);
	memcpy(seed + o, &pid, sizeof(pid));
	o += sizeof(pid);
	memcpy(seed + o, &counter, sizeof(counter));
	o += sizeof(counter);
	memcpy(seed + o, &out, sizeof(out));

	sha256_init(&ctx);
	sha256_update(&ctx, seed, sizeof(seed));
	sha256_final(&ctx, seed);

	while (got < len && got < sizeof(seed)) {
		out[got] = seed[got];
		got++;
	}
	memset(seed, 0, sizeof(seed));
}

/*
 *	crypt - hash a password.
 *
 *	If salt is a stored modular crypt string the same parameters and
 *	salt are used, otherwise a new salt is generated.  Returns a
 *	pointer to a static buffer, or NULL for an unsupported hash.
 */
char *crypt(const char *key, const char *salt)
{
	static char result[128];
	uint8_t saltbuf[32];
	uint8_t dk[SHA256_DIGEST_LENGTH];
	size_t saltlen = PBKDF2_SALT_BYTES;
	uint32_t iterations = PBKDF2_DEFAULT_ITER;

	if (salt[0] == '$') {
		if (parse_hash(salt, &iterations, saltbuf, &saltlen) < 0) {
			errno = EINVAL;
			return NULL;
		}
	} else {
		fill_random(saltbuf, saltlen);
	}

	pbkdf2_sha256(key, strlen(key),
		      saltbuf, saltlen, iterations, dk, sizeof(dk));

	format_hash(result, sizeof(result), iterations, saltbuf, saltlen,
		    dk, sizeof(dk));
	memset(dk, 0, sizeof(dk));
	return result;
}

/*
 *	crypt_check - constant time password verification.
 *
 *	Returns 1 when key matches the stored modular crypt hash, 0
 *	otherwise.  Only the modular format is accepted; an account with
 *	a legacy or malformed hash can never authenticate.
 */
int crypt_check(const char *key, const char *stored)
{
	char storedbuf[128];
	char *calc;
	size_t i, n;
	uint8_t diff = 0;

	if (key == NULL || stored == NULL || stored[0] != '$')
		return 0;

	n = strlen(stored);
	if (n >= sizeof(storedbuf))
		return 0;
	memcpy(storedbuf, stored, n + 1);

	calc = crypt(key, storedbuf);
	if (calc == NULL)
		return 0;

	if (strlen(calc) != n)
		return 0;
	for (i = 0; i < n; i++)
		diff |= (uint8_t)(calc[i] ^ storedbuf[i]);
	return diff == 0;
}

/*
 *	crypt_generate - produce a new password hash with a fresh salt.
 */
char *crypt_generate(const char *key)
{
	return crypt(key, "");
}

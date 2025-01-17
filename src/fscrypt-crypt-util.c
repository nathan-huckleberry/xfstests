// SPDX-License-Identifier: GPL-2.0+
/*
 * fscrypt-crypt-util.c - utility for verifying fscrypt-encrypted data
 *
 * Copyright 2019 Google LLC
 */

/*
 * This program implements all crypto algorithms supported by fscrypt (a.k.a.
 * ext4, f2fs, and ubifs encryption), for the purpose of verifying the
 * correctness of the ciphertext stored on-disk.  See usage() below.
 *
 * All algorithms are implemented in portable C code to avoid depending on
 * libcrypto (OpenSSL), and because some fscrypt-supported algorithms aren't
 * available in libcrypto anyway (e.g. Adiantum), or are only supported in
 * recent versions (e.g. HKDF-SHA512).  For simplicity, all crypto code here
 * tries to follow the mathematical definitions directly, without optimizing for
 * performance or worrying about following security best practices such as
 * mitigating side-channel attacks.  So, only use this program for testing!
 */

#include <asm/byteorder.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/types.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM_NAME "fscrypt-crypt-util"

/*
 * Define to enable the tests of the crypto code in this file.  If enabled, you
 * must link this program with OpenSSL (-lcrypto) v1.1.0 or later, and your
 * kernel needs CONFIG_CRYPTO_USER_API_SKCIPHER=y, CONFIG_CRYPTO_ADIANTUM=y, and
 * CONFIG_CRYPTO_HCTR2=y.
 */
#undef ENABLE_ALG_TESTS

#define NUM_ALG_TEST_ITERATIONS	10000

static void usage(FILE *fp)
{
	fputs(
"Usage: " PROGRAM_NAME " [OPTION]... CIPHER MASTER_KEY\n"
"\n"
"Utility for verifying fscrypt-encrypted data.  This program encrypts\n"
"(or decrypts) the data on stdin using the given CIPHER with the given\n"
"MASTER_KEY (or a key derived from it, if a KDF is specified), and writes the\n"
"resulting ciphertext (or plaintext) to stdout.\n"
"\n"
"CIPHER can be AES-256-XTS, AES-256-CTS-CBC, AES-128-CBC-ESSIV, AES-128-CTS-CBC,\n"
"Adiantum, or AES-256-HCTR2.  MASTER_KEY must be a hex string long enough for\n"
"the cipher."
"\n"
"WARNING: this program is only meant for testing, not for \"real\" use!\n"
"\n"
"Options:\n"
"  --block-number=BNUM         Starting block number for IV generation.\n"
"                                Default: 0\n"
"  --block-size=BLOCK_SIZE     Encrypt each BLOCK_SIZE bytes independently.\n"
"                                Default: 4096 bytes\n"
"  --decrypt                   Decrypt instead of encrypt\n"
"  --file-nonce=NONCE          File's nonce as a 32-character hex string\n"
"  --fs-uuid=UUID              The filesystem UUID as a 32-character hex string.\n"
"                                Required for --iv-ino-lblk-32 and\n"
"                                --iv-ino-lblk-64; otherwise is unused.\n"
"  --help                      Show this help\n"
"  --inode-number=INUM         The file's inode number.  Required for\n"
"                                --iv-ino-lblk-32 and --iv-ino-lblk-64;\n"
"                                otherwise is unused.\n"
"  --iv-ino-lblk-32            Similar to --iv-ino-lblk-64, but selects the\n"
"                                32-bit variant.\n"
"  --iv-ino-lblk-64            Use the format where the IVs include the inode\n"
"                                number and the same key is shared across files.\n"
"                                Requires --kdf=HKDF-SHA512, --fs-uuid,\n"
"                                --inode-number, and --mode-num.\n"
"  --kdf=KDF                   Key derivation function to use: AES-128-ECB,\n"
"                                HKDF-SHA512, or none.  Default: none\n"
"  --mode-num=NUM              Derive per-mode key using mode number NUM\n"
"  --padding=PADDING           If last block is partial, zero-pad it to next\n"
"                                PADDING-byte boundary.  Default: BLOCK_SIZE\n"
	, fp);
}

/*----------------------------------------------------------------------------*
 *                                 Utilities                                  *
 *----------------------------------------------------------------------------*/

#define ARRAY_SIZE(A)		(sizeof(A) / sizeof((A)[0]))
#define MIN(x, y)		((x) < (y) ? (x) : (y))
#define MAX(x, y)		((x) > (y) ? (x) : (y))
#define ROUND_DOWN(x, y)	((x) & ~((y) - 1))
#define ROUND_UP(x, y)		(((x) + (y) - 1) & ~((y) - 1))
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define STATIC_ASSERT(e)	((void)sizeof(char[1 - 2*!(e)]))

typedef __u8			u8;
typedef __u16			u16;
typedef __u32			u32;
typedef __u64			u64;

#define cpu_to_le32		__cpu_to_le32
#define cpu_to_be32		__cpu_to_be32
#define cpu_to_le64		__cpu_to_le64
#define cpu_to_be64		__cpu_to_be64
#define le32_to_cpu		__le32_to_cpu
#define be32_to_cpu		__be32_to_cpu
#define le64_to_cpu		__le64_to_cpu
#define be64_to_cpu		__be64_to_cpu

#define DEFINE_UNALIGNED_ACCESS_HELPERS(type, native_type)	\
static inline native_type __attribute__((unused))		\
get_unaligned_##type(const void *p)				\
{								\
	__##type x;						\
								\
	memcpy(&x, p, sizeof(x));				\
	return type##_to_cpu(x);				\
}								\
								\
static inline void __attribute__((unused))			\
put_unaligned_##type(native_type v, void *p)			\
{								\
	__##type x = cpu_to_##type(v);				\
								\
	memcpy(p, &x, sizeof(x));				\
}

DEFINE_UNALIGNED_ACCESS_HELPERS(le32, u32)
DEFINE_UNALIGNED_ACCESS_HELPERS(be32, u32)
DEFINE_UNALIGNED_ACCESS_HELPERS(le64, u64)
DEFINE_UNALIGNED_ACCESS_HELPERS(be64, u64)

static inline bool is_power_of_2(unsigned long v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

static inline u32 rol32(u32 v, int n)
{
	return (v << n) | (v >> (32 - n));
}

static inline u32 ror32(u32 v, int n)
{
	return (v >> n) | (v << (32 - n));
}

static inline u64 rol64(u64 v, int n)
{
	return (v << n) | (v >> (64 - n));
}

static inline u64 ror64(u64 v, int n)
{
	return (v >> n) | (v << (64 - n));
}

static inline void xor(u8 *res, const u8 *a, const u8 *b, size_t count)
{
	while (count--)
		*res++ = *a++ ^ *b++;
}

static void __attribute__((noreturn, format(printf, 2, 3)))
do_die(int err, const char *format, ...)
{
	va_list va;

	va_start(va, format);
	fputs("[" PROGRAM_NAME "] ERROR: ", stderr);
	vfprintf(stderr, format, va);
	if (err)
		fprintf(stderr, ": %s", strerror(errno));
	putc('\n', stderr);
	va_end(va);
	exit(1);
}

#define die(format, ...)	do_die(0,     (format), ##__VA_ARGS__)
#define die_errno(format, ...)	do_die(errno, (format), ##__VA_ARGS__)

static __attribute__((noreturn)) void
assertion_failed(const char *expr, const char *file, int line)
{
	die("Assertion failed: %s at %s:%d", expr, file, line);
}

#define ASSERT(e) ({ if (!(e)) assertion_failed(#e, __FILE__, __LINE__); })

static void *xmalloc(size_t size)
{
	void *p = malloc(size);

	ASSERT(p != NULL);
	return p;
}

static int hexchar2bin(char c)
{
	if (c >= 'a' && c <= 'f')
		return 10 + c - 'a';
	if (c >= 'A' && c <= 'F')
		return 10 + c - 'A';
	if (c >= '0' && c <= '9')
		return c - '0';
	return -1;
}

static int hex2bin(const char *hex, u8 *bin, int max_bin_size)
{
	size_t len = strlen(hex);
	size_t i;

	if (len & 1)
		return -1;
	len /= 2;
	if (len > max_bin_size)
		return -1;

	for (i = 0; i < len; i++) {
		int high = hexchar2bin(hex[2 * i]);
		int low = hexchar2bin(hex[2 * i + 1]);

		if (high < 0 || low < 0)
			return -1;
		bin[i] = (high << 4) | low;
	}
	return len;
}

static size_t xread(int fd, void *buf, size_t count)
{
	const size_t orig_count = count;

	while (count) {
		ssize_t res = read(fd, buf, count);

		if (res < 0)
			die_errno("read error");
		if (res == 0)
			break;
		buf += res;
		count -= res;
	}
	return orig_count - count;
}

static void full_write(int fd, const void *buf, size_t count)
{
	while (count) {
		ssize_t res = write(fd, buf, count);

		if (res < 0)
			die_errno("write error");
		buf += res;
		count -= res;
	}
}

#ifdef ENABLE_ALG_TESTS
static void rand_bytes(u8 *buf, size_t count)
{
	while (count--)
		*buf++ = rand();
}

#include <linux/if_alg.h>
#include <sys/socket.h>
#define SOL_ALG 279
static void af_alg_crypt(int algfd, int op, const u8 *key, size_t keylen,
			 const u8 *iv, size_t ivlen,
			 const u8 *src, u8 *dst, size_t datalen)
{
	size_t controllen = CMSG_SPACE(sizeof(int)) +
			    CMSG_SPACE(sizeof(struct af_alg_iv) + ivlen);
	u8 *control = xmalloc(controllen);
	struct iovec iov = { .iov_base = (u8 *)src, .iov_len = datalen };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = controllen,
	};
	struct cmsghdr *cmsg;
	struct af_alg_iv *algiv;
	int reqfd;

	memset(control, 0, controllen);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	cmsg->cmsg_level = SOL_ALG;
	cmsg->cmsg_type = ALG_SET_OP;
	*(int *)CMSG_DATA(cmsg) = op;

	cmsg = CMSG_NXTHDR(&msg, cmsg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct af_alg_iv) + ivlen);
	cmsg->cmsg_level = SOL_ALG;
	cmsg->cmsg_type = ALG_SET_IV;
	algiv = (struct af_alg_iv *)CMSG_DATA(cmsg);
	algiv->ivlen = ivlen;
	memcpy(algiv->iv, iv, ivlen);

	if (setsockopt(algfd, SOL_ALG, ALG_SET_KEY, key, keylen) != 0)
		die_errno("can't set key on AF_ALG socket");

	reqfd = accept(algfd, NULL, NULL);
	if (reqfd < 0)
		die_errno("can't accept() AF_ALG socket");
	if (sendmsg(reqfd, &msg, 0) != datalen)
		die_errno("can'datalen, t sendmsg() AF_ALG request socket");
	if (xread(reqfd, dst, datalen) != datalen)
		die("short read from AF_ALG request socket");
	close(reqfd);

	free(control);
}
#endif

/*----------------------------------------------------------------------------*
 *                          Finite field arithmetic                           *
 *----------------------------------------------------------------------------*/

#define gf128mul_dat(q) { \
	q(0x00), q(0x01), q(0x02), q(0x03), q(0x04), q(0x05), q(0x06), q(0x07),\
	q(0x08), q(0x09), q(0x0a), q(0x0b), q(0x0c), q(0x0d), q(0x0e), q(0x0f),\
	q(0x10), q(0x11), q(0x12), q(0x13), q(0x14), q(0x15), q(0x16), q(0x17),\
	q(0x18), q(0x19), q(0x1a), q(0x1b), q(0x1c), q(0x1d), q(0x1e), q(0x1f),\
	q(0x20), q(0x21), q(0x22), q(0x23), q(0x24), q(0x25), q(0x26), q(0x27),\
	q(0x28), q(0x29), q(0x2a), q(0x2b), q(0x2c), q(0x2d), q(0x2e), q(0x2f),\
	q(0x30), q(0x31), q(0x32), q(0x33), q(0x34), q(0x35), q(0x36), q(0x37),\
	q(0x38), q(0x39), q(0x3a), q(0x3b), q(0x3c), q(0x3d), q(0x3e), q(0x3f),\
	q(0x40), q(0x41), q(0x42), q(0x43), q(0x44), q(0x45), q(0x46), q(0x47),\
	q(0x48), q(0x49), q(0x4a), q(0x4b), q(0x4c), q(0x4d), q(0x4e), q(0x4f),\
	q(0x50), q(0x51), q(0x52), q(0x53), q(0x54), q(0x55), q(0x56), q(0x57),\
	q(0x58), q(0x59), q(0x5a), q(0x5b), q(0x5c), q(0x5d), q(0x5e), q(0x5f),\
	q(0x60), q(0x61), q(0x62), q(0x63), q(0x64), q(0x65), q(0x66), q(0x67),\
	q(0x68), q(0x69), q(0x6a), q(0x6b), q(0x6c), q(0x6d), q(0x6e), q(0x6f),\
	q(0x70), q(0x71), q(0x72), q(0x73), q(0x74), q(0x75), q(0x76), q(0x77),\
	q(0x78), q(0x79), q(0x7a), q(0x7b), q(0x7c), q(0x7d), q(0x7e), q(0x7f),\
	q(0x80), q(0x81), q(0x82), q(0x83), q(0x84), q(0x85), q(0x86), q(0x87),\
	q(0x88), q(0x89), q(0x8a), q(0x8b), q(0x8c), q(0x8d), q(0x8e), q(0x8f),\
	q(0x90), q(0x91), q(0x92), q(0x93), q(0x94), q(0x95), q(0x96), q(0x97),\
	q(0x98), q(0x99), q(0x9a), q(0x9b), q(0x9c), q(0x9d), q(0x9e), q(0x9f),\
	q(0xa0), q(0xa1), q(0xa2), q(0xa3), q(0xa4), q(0xa5), q(0xa6), q(0xa7),\
	q(0xa8), q(0xa9), q(0xaa), q(0xab), q(0xac), q(0xad), q(0xae), q(0xaf),\
	q(0xb0), q(0xb1), q(0xb2), q(0xb3), q(0xb4), q(0xb5), q(0xb6), q(0xb7),\
	q(0xb8), q(0xb9), q(0xba), q(0xbb), q(0xbc), q(0xbd), q(0xbe), q(0xbf),\
	q(0xc0), q(0xc1), q(0xc2), q(0xc3), q(0xc4), q(0xc5), q(0xc6), q(0xc7),\
	q(0xc8), q(0xc9), q(0xca), q(0xcb), q(0xcc), q(0xcd), q(0xce), q(0xcf),\
	q(0xd0), q(0xd1), q(0xd2), q(0xd3), q(0xd4), q(0xd5), q(0xd6), q(0xd7),\
	q(0xd8), q(0xd9), q(0xda), q(0xdb), q(0xdc), q(0xdd), q(0xde), q(0xdf),\
	q(0xe0), q(0xe1), q(0xe2), q(0xe3), q(0xe4), q(0xe5), q(0xe6), q(0xe7),\
	q(0xe8), q(0xe9), q(0xea), q(0xeb), q(0xec), q(0xed), q(0xee), q(0xef),\
	q(0xf0), q(0xf1), q(0xf2), q(0xf3), q(0xf4), q(0xf5), q(0xf6), q(0xf7),\
	q(0xf8), q(0xf9), q(0xfa), q(0xfb), q(0xfc), q(0xfd), q(0xfe), q(0xff) \
}

#define xda_be(i) ( \
	(i & 0x80 ? 0x4380 : 0) ^ (i & 0x40 ? 0x21c0 : 0) ^ \
	(i & 0x20 ? 0x10e0 : 0) ^ (i & 0x10 ? 0x0870 : 0) ^ \
	(i & 0x08 ? 0x0438 : 0) ^ (i & 0x04 ? 0x021c : 0) ^ \
	(i & 0x02 ? 0x010e : 0) ^ (i & 0x01 ? 0x0087 : 0) \
)

#define xda_le(i) ( \
	(i & 0x80 ? 0xe100 : 0) ^ (i & 0x40 ? 0x7080 : 0) ^ \
	(i & 0x20 ? 0x3840 : 0) ^ (i & 0x10 ? 0x1c20 : 0) ^ \
	(i & 0x08 ? 0x0e10 : 0) ^ (i & 0x04 ? 0x0708 : 0) ^ \
	(i & 0x02 ? 0x0384 : 0) ^ (i & 0x01 ? 0x01c2 : 0) \
)

static const u16 gf2_128_mul_table_le[256] = gf128mul_dat(xda_le);

/* Multiply a GF(2^8) element by the polynomial 'x' */
static inline u8 gf2_8_mul_x(u8 b)
{
	return (b << 1) ^ ((b & 0x80) ? 0x1B : 0);
}

/* Multiply four packed GF(2^8) elements by the polynomial 'x' */
static inline u32 gf2_8_mul_x_4way(u32 w)
{
	return ((w & 0x7F7F7F7F) << 1) ^ (((w & 0x80808080) >> 7) * 0x1B);
}

/* Element of GF(2^128) */
typedef struct {
	__le64 lo;
	__le64 hi;
} ble128;

typedef struct {
	__le64 hi;
	__le64 lo;
} be128;

/* Multiply a GF(2^128) element by the polynomial 'x' */
static inline void gf2_128_mul_x_ble(ble128 *t)
{
	u64 lo = le64_to_cpu(t->lo);
	u64 hi = le64_to_cpu(t->hi);

	t->hi = cpu_to_le64((hi << 1) | (lo >> 63));
	t->lo = cpu_to_le64((lo << 1) ^ ((hi & (1ULL << 63)) ? 0x87 : 0));
}

static inline void gf2_128_mul_x_lle(be128 *t)
{
	u64 hi = be64_to_cpu(t->hi);
	u64 lo = be64_to_cpu(t->lo);

	u64 _tt = (lo & (1ULL)) ? ((u64)0xe1 << 56) : 0;

	t->lo = cpu_to_be64((lo >> 1) | (hi << 63));
	t->hi = cpu_to_be64((hi >> 1) ^ _tt);
}

static inline void gf2_128_mul_x8_lle(be128 *t)
{
	u64 hi = be64_to_cpu(t->hi);
	u64 lo = be64_to_cpu(t->lo);
	u64 _tt = gf2_128_mul_table_le[lo & 0xff];

	t->lo = cpu_to_be64((lo >> 8) | (hi << 56));
	t->hi = cpu_to_be64((hi >> 8) ^ (_tt << 48));
}

void gf2_128_mul_lle(be128 *r, const be128 *b)
{
	be128 p[8];
	int i;

	p[0] = *r;
	for (i = 0; i < 7; ++i) {
		memcpy(&p[i + 1], &p[i], sizeof(be128));
		gf2_128_mul_x_lle(&p[i + 1]);
	}

	memset(r, 0, sizeof(*r));
	for (i = 0;;) {
		u8 ch = ((u8 *)b)[15-i];

		if (ch & 0x80)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[0], sizeof(*r));
		if (ch & 0x40)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[1], sizeof(*r));
		if (ch & 0x20)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[2], sizeof(*r));
		if (ch & 0x10)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[3], sizeof(*r));
		if (ch & 0x08)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[4], sizeof(*r));
		if (ch & 0x04)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[5], sizeof(*r));
		if (ch & 0x02)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[6], sizeof(*r));
		if (ch & 0x01)
			xor((u8 *)r, (u8 *)r, (u8 *)&p[7], sizeof(*r));

		if (++i >= 16)
			break;

		gf2_128_mul_x8_lle(r);
	}
}

/*----------------------------------------------------------------------------*
 *                             Group arithmetic                               *
 *----------------------------------------------------------------------------*/

/* Element of Z/(2^{128}Z)  (a.k.a. the integers modulo 2^128) */
typedef struct {
	__le64 lo;
	__le64 hi;
} le128;

static inline void le128_add(le128 *res, const le128 *a, const le128 *b)
{
	u64 a_lo = le64_to_cpu(a->lo);
	u64 b_lo = le64_to_cpu(b->lo);

	res->lo = cpu_to_le64(a_lo + b_lo);
	res->hi = cpu_to_le64(le64_to_cpu(a->hi) + le64_to_cpu(b->hi) +
			      (a_lo + b_lo < a_lo));
}

static inline void le128_sub(le128 *res, const le128 *a, const le128 *b)
{
	u64 a_lo = le64_to_cpu(a->lo);
	u64 b_lo = le64_to_cpu(b->lo);

	res->lo = cpu_to_le64(a_lo - b_lo);
	res->hi = cpu_to_le64(le64_to_cpu(a->hi) - le64_to_cpu(b->hi) -
			      (a_lo - b_lo > a_lo));
}

/*----------------------------------------------------------------------------*
 *                              AES block cipher                              *
 *----------------------------------------------------------------------------*/

/*
 * Reference: "FIPS 197, Advanced Encryption Standard"
 *	https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.197.pdf
 */

#define AES_BLOCK_SIZE		16
#define AES_128_KEY_SIZE	16
#define AES_192_KEY_SIZE	24
#define AES_256_KEY_SIZE	32

static inline void AddRoundKey(u32 state[4], const u32 *rk)
{
	int i;

	for (i = 0; i < 4; i++)
		state[i] ^= rk[i];
}

static const u8 aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
	0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
	0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
	0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
	0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
	0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
	0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
	0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
	0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
	0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
	0xb0, 0x54, 0xbb, 0x16,
};

static u8 aes_inverse_sbox[256];

static void aes_init(void)
{
	int i;

	for (i = 0; i < 256; i++)
		aes_inverse_sbox[aes_sbox[i]] = i;
}

static inline u32 DoSubWord(u32 w, const u8 sbox[256])
{
	return ((u32)sbox[(u8)(w >> 24)] << 24) |
	       ((u32)sbox[(u8)(w >> 16)] << 16) |
	       ((u32)sbox[(u8)(w >>  8)] <<  8) |
	       ((u32)sbox[(u8)(w >>  0)] <<  0);
}

static inline u32 SubWord(u32 w)
{
	return DoSubWord(w, aes_sbox);
}

static inline u32 InvSubWord(u32 w)
{
	return DoSubWord(w, aes_inverse_sbox);
}

static inline void SubBytes(u32 state[4])
{
	int i;

	for (i = 0; i < 4; i++)
		state[i] = SubWord(state[i]);
}

static inline void InvSubBytes(u32 state[4])
{
	int i;

	for (i = 0; i < 4; i++)
		state[i] = InvSubWord(state[i]);
}

static inline void DoShiftRows(u32 state[4], int direction)
{
	u32 newstate[4];
	int i;

	for (i = 0; i < 4; i++)
		newstate[i] = (state[(i + direction*0) & 3] & 0xff) |
			      (state[(i + direction*1) & 3] & 0xff00) |
			      (state[(i + direction*2) & 3] & 0xff0000) |
			      (state[(i + direction*3) & 3] & 0xff000000);
	memcpy(state, newstate, 16);
}

static inline void ShiftRows(u32 state[4])
{
	DoShiftRows(state, 1);
}

static inline void InvShiftRows(u32 state[4])
{
	DoShiftRows(state, -1);
}

/*
 * Mix one column by doing the following matrix multiplication in GF(2^8):
 *
 *     | 2 3 1 1 |   | w[0] |
 *     | 1 2 3 1 |   | w[1] |
 *     | 1 1 2 3 | x | w[2] |
 *     | 3 1 1 2 |   | w[3] |
 *
 * a.k.a. w[i] = 2*w[i] + 3*w[(i+1)%4] + w[(i+2)%4] + w[(i+3)%4]
 */
static inline u32 MixColumn(u32 w)
{
	u32 _2w0_w2 = gf2_8_mul_x_4way(w) ^ ror32(w, 16);
	u32 _3w1_w3 = ror32(_2w0_w2 ^ w, 8);

	return _2w0_w2 ^ _3w1_w3;
}

/*
 *           ( | 5 0 4 0 |   | w[0] | )
 *          (  | 0 5 0 4 |   | w[1] |  )
 * MixColumn(  | 4 0 5 0 | x | w[2] |  )
 *           ( | 0 4 0 5 |   | w[3] | )
 */
static inline u32 InvMixColumn(u32 w)
{
	u32 _4w = gf2_8_mul_x_4way(gf2_8_mul_x_4way(w));

	return MixColumn(_4w ^ w ^ ror32(_4w, 16));
}

static inline void MixColumns(u32 state[4])
{
	int i;

	for (i = 0; i < 4; i++)
		state[i] = MixColumn(state[i]);
}

static inline void InvMixColumns(u32 state[4])
{
	int i;

	for (i = 0; i < 4; i++)
		state[i] = InvMixColumn(state[i]);
}

struct aes_key {
	u32 round_keys[15 * 4];
	int nrounds;
};

/* Expand an AES key */
static void aes_setkey(struct aes_key *k, const u8 *key, int keysize)
{
	const int N = keysize / 4;
	u32 * const rk = k->round_keys;
	u8 rcon = 1;
	int i;

	ASSERT(keysize == 16 || keysize == 24 || keysize == 32);
	k->nrounds = 6 + N;
	for (i = 0; i < 4 * (k->nrounds + 1); i++) {
		if (i < N) {
			rk[i] = get_unaligned_le32(&key[i * sizeof(__le32)]);
		} else if (i % N == 0) {
			rk[i] = rk[i - N] ^ SubWord(ror32(rk[i - 1], 8)) ^ rcon;
			rcon = gf2_8_mul_x(rcon);
		} else if (N > 6 && i % N == 4) {
			rk[i] = rk[i - N] ^ SubWord(rk[i - 1]);
		} else {
			rk[i] = rk[i - N] ^ rk[i - 1];
		}
	}
}

/* Encrypt one 16-byte block with AES */
static void aes_encrypt(const struct aes_key *k, const u8 src[AES_BLOCK_SIZE],
			u8 dst[AES_BLOCK_SIZE])
{
	u32 state[4];
	int i;

	for (i = 0; i < 4; i++)
		state[i] = get_unaligned_le32(&src[i * sizeof(__le32)]);

	AddRoundKey(state, k->round_keys);
	for (i = 1; i < k->nrounds; i++) {
		SubBytes(state);
		ShiftRows(state);
		MixColumns(state);
		AddRoundKey(state, &k->round_keys[4 * i]);
	}
	SubBytes(state);
	ShiftRows(state);
	AddRoundKey(state, &k->round_keys[4 * i]);

	for (i = 0; i < 4; i++)
		put_unaligned_le32(state[i], &dst[i * sizeof(__le32)]);
}

/* Decrypt one 16-byte block with AES */
static void aes_decrypt(const struct aes_key *k, const u8 src[AES_BLOCK_SIZE],
			u8 dst[AES_BLOCK_SIZE])
{
	u32 state[4];
	int i;

	for (i = 0; i < 4; i++)
		state[i] = get_unaligned_le32(&src[i * sizeof(__le32)]);

	AddRoundKey(state, &k->round_keys[4 * k->nrounds]);
	InvShiftRows(state);
	InvSubBytes(state);
	for (i = k->nrounds - 1; i >= 1; i--) {
		AddRoundKey(state, &k->round_keys[4 * i]);
		InvMixColumns(state);
		InvShiftRows(state);
		InvSubBytes(state);
	}
	AddRoundKey(state, k->round_keys);

	for (i = 0; i < 4; i++)
		put_unaligned_le32(state[i], &dst[i * sizeof(__le32)]);
}

#ifdef ENABLE_ALG_TESTS
#include <openssl/aes.h>
static void test_aes_keysize(int keysize)
{
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;

	while (num_tests--) {
		struct aes_key k;
		AES_KEY ref_k;
		u8 key[AES_256_KEY_SIZE];
		u8 ptext[AES_BLOCK_SIZE];
		u8 ctext[AES_BLOCK_SIZE];
		u8 ref_ctext[AES_BLOCK_SIZE];
		u8 decrypted[AES_BLOCK_SIZE];

		rand_bytes(key, keysize);
		rand_bytes(ptext, AES_BLOCK_SIZE);

		aes_setkey(&k, key, keysize);
		aes_encrypt(&k, ptext, ctext);

		ASSERT(AES_set_encrypt_key(key, keysize*8, &ref_k) == 0);
		AES_encrypt(ptext, ref_ctext, &ref_k);

		ASSERT(memcmp(ctext, ref_ctext, AES_BLOCK_SIZE) == 0);

		aes_decrypt(&k, ctext, decrypted);
		ASSERT(memcmp(ptext, decrypted, AES_BLOCK_SIZE) == 0);
	}
}

static void test_aes(void)
{
	test_aes_keysize(AES_128_KEY_SIZE);
	test_aes_keysize(AES_192_KEY_SIZE);
	test_aes_keysize(AES_256_KEY_SIZE);
}
#endif /* ENABLE_ALG_TESTS */

/*----------------------------------------------------------------------------*
 *                            SHA-512 and SHA-256                             *
 *----------------------------------------------------------------------------*/

/*
 * Reference: "FIPS 180-2, Secure Hash Standard"
 *	https://csrc.nist.gov/csrc/media/publications/fips/180/2/archive/2002-08-01/documents/fips180-2withchangenotice.pdf
 */

#define SHA512_DIGEST_SIZE	64
#define SHA512_BLOCK_SIZE	128

#define SHA256_DIGEST_SIZE	32
#define SHA256_BLOCK_SIZE	64

#define Ch(x, y, z)	(((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z)	(((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define Sigma512_0(x)	(ror64((x), 28) ^ ror64((x), 34) ^ ror64((x), 39))
#define Sigma512_1(x)	(ror64((x), 14) ^ ror64((x), 18) ^ ror64((x), 41))
#define sigma512_0(x)	(ror64((x),  1) ^ ror64((x),  8) ^ ((x) >> 7))
#define sigma512_1(x)	(ror64((x), 19) ^ ror64((x), 61) ^ ((x) >> 6))

#define Sigma256_0(x)	(ror32((x),  2) ^ ror32((x), 13) ^ ror32((x), 22))
#define Sigma256_1(x)	(ror32((x),  6) ^ ror32((x), 11) ^ ror32((x), 25))
#define sigma256_0(x)	(ror32((x),  7) ^ ror32((x), 18) ^ ((x) >>  3))
#define sigma256_1(x)	(ror32((x), 17) ^ ror32((x), 19) ^ ((x) >> 10))

static const u64 sha512_iv[8] = {
	0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b,
	0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f,
	0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
};

static const u64 sha512_round_constants[80] = {
	0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f,
	0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
	0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242,
	0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
	0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
	0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
	0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275,
	0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
	0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f,
	0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
	0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc,
	0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
	0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6,
	0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
	0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
	0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
	0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99,
	0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
	0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc,
	0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
	0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915,
	0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
	0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba,
	0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
	0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
	0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
	0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

/* Compute the SHA-512 digest of the given buffer */
static void sha512(const u8 *in, size_t inlen, u8 out[SHA512_DIGEST_SIZE])
{
	const size_t msglen = ROUND_UP(inlen + 17, SHA512_BLOCK_SIZE);
	u8 * const msg = xmalloc(msglen);
	u64 H[8];
	int i;

	/* super naive way of handling the padding */
	memcpy(msg, in, inlen);
	memset(&msg[inlen], 0, msglen - inlen);
	msg[inlen] = 0x80;
	put_unaligned_be64((u64)inlen * 8, &msg[msglen - sizeof(__be64)]);
	in = msg;

	memcpy(H, sha512_iv, sizeof(H));
	do {
		u64 a = H[0], b = H[1], c = H[2], d = H[3],
		    e = H[4], f = H[5], g = H[6], h = H[7];
		u64 W[80];

		for (i = 0; i < 16; i++)
			W[i] = get_unaligned_be64(&in[i * sizeof(__be64)]);
		for (; i < ARRAY_SIZE(W); i++)
			W[i] = sigma512_1(W[i - 2]) + W[i - 7] +
			       sigma512_0(W[i - 15]) + W[i - 16];
		for (i = 0; i < ARRAY_SIZE(W); i++) {
			u64 T1 = h + Sigma512_1(e) + Ch(e, f, g) +
				 sha512_round_constants[i] + W[i];
			u64 T2 = Sigma512_0(a) + Maj(a, b, c);

			h = g; g = f; f = e; e = d + T1;
			d = c; c = b; b = a; a = T1 + T2;
		}
		H[0] += a; H[1] += b; H[2] += c; H[3] += d;
		H[4] += e; H[5] += f; H[6] += g; H[7] += h;
	} while ((in += SHA512_BLOCK_SIZE) != &msg[msglen]);

	for (i = 0; i < ARRAY_SIZE(H); i++)
		put_unaligned_be64(H[i], &out[i * sizeof(__be64)]);
	free(msg);
}

/* Compute the SHA-256 digest of the given buffer */
static void sha256(const u8 *in, size_t inlen, u8 out[SHA256_DIGEST_SIZE])
{
	const size_t msglen = ROUND_UP(inlen + 9, SHA256_BLOCK_SIZE);
	u8 * const msg = xmalloc(msglen);
	u32 H[8];
	int i;

	/* super naive way of handling the padding */
	memcpy(msg, in, inlen);
	memset(&msg[inlen], 0, msglen - inlen);
	msg[inlen] = 0x80;
	put_unaligned_be64((u64)inlen * 8, &msg[msglen - sizeof(__be64)]);
	in = msg;

	for (i = 0; i < ARRAY_SIZE(H); i++)
		H[i] = (u32)(sha512_iv[i] >> 32);
	do {
		u32 a = H[0], b = H[1], c = H[2], d = H[3],
		    e = H[4], f = H[5], g = H[6], h = H[7];
		u32 W[64];

		for (i = 0; i < 16; i++)
			W[i] = get_unaligned_be32(&in[i * sizeof(__be32)]);
		for (; i < ARRAY_SIZE(W); i++)
			W[i] = sigma256_1(W[i - 2]) + W[i - 7] +
			       sigma256_0(W[i - 15]) + W[i - 16];
		for (i = 0; i < ARRAY_SIZE(W); i++) {
			u32 T1 = h + Sigma256_1(e) + Ch(e, f, g) +
				 (u32)(sha512_round_constants[i] >> 32) + W[i];
			u32 T2 = Sigma256_0(a) + Maj(a, b, c);

			h = g; g = f; f = e; e = d + T1;
			d = c; c = b; b = a; a = T1 + T2;
		}
		H[0] += a; H[1] += b; H[2] += c; H[3] += d;
		H[4] += e; H[5] += f; H[6] += g; H[7] += h;
	} while ((in += SHA256_BLOCK_SIZE) != &msg[msglen]);

	for (i = 0; i < ARRAY_SIZE(H); i++)
		put_unaligned_be32(H[i], &out[i * sizeof(__be32)]);
	free(msg);
}

#ifdef ENABLE_ALG_TESTS
#include <openssl/sha.h>
static void test_sha2(void)
{
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;

	while (num_tests--) {
		u8 in[4096];
		u8 digest[SHA512_DIGEST_SIZE];
		u8 ref_digest[SHA512_DIGEST_SIZE];
		const size_t inlen = rand() % (1 + sizeof(in));

		rand_bytes(in, inlen);

		sha256(in, inlen, digest);
		SHA256(in, inlen, ref_digest);
		ASSERT(memcmp(digest, ref_digest, SHA256_DIGEST_SIZE) == 0);

		sha512(in, inlen, digest);
		SHA512(in, inlen, ref_digest);
		ASSERT(memcmp(digest, ref_digest, SHA512_DIGEST_SIZE) == 0);
	}
}
#endif /* ENABLE_ALG_TESTS */

/*----------------------------------------------------------------------------*
 *                            HKDF implementation                             *
 *----------------------------------------------------------------------------*/

static void hmac_sha512(const u8 *key, size_t keylen, const u8 *msg,
			size_t msglen, u8 mac[SHA512_DIGEST_SIZE])
{
	u8 *ibuf = xmalloc(SHA512_BLOCK_SIZE + msglen);
	u8 obuf[SHA512_BLOCK_SIZE + SHA512_DIGEST_SIZE];

	ASSERT(keylen <= SHA512_BLOCK_SIZE); /* keylen > bs not implemented */

	memset(ibuf, 0x36, SHA512_BLOCK_SIZE);
	xor(ibuf, ibuf, key, keylen);
	memcpy(&ibuf[SHA512_BLOCK_SIZE], msg, msglen);

	memset(obuf, 0x5c, SHA512_BLOCK_SIZE);
	xor(obuf, obuf, key, keylen);
	sha512(ibuf, SHA512_BLOCK_SIZE + msglen, &obuf[SHA512_BLOCK_SIZE]);
	sha512(obuf, sizeof(obuf), mac);

	free(ibuf);
}

static void hkdf_sha512(const u8 *ikm, size_t ikmlen,
			const u8 *salt, size_t saltlen,
			const u8 *info, size_t infolen,
			u8 *output, size_t outlen)
{
	static const u8 default_salt[SHA512_DIGEST_SIZE];
	u8 prk[SHA512_DIGEST_SIZE]; /* pseudorandom key */
	u8 *buf = xmalloc(1 + infolen + SHA512_DIGEST_SIZE);
	u8 counter = 1;
	size_t i;

	if (saltlen == 0) {
		salt = default_salt;
		saltlen = sizeof(default_salt);
	}

	/* HKDF-Extract */
	ASSERT(ikmlen > 0);
	hmac_sha512(salt, saltlen, ikm, ikmlen, prk);

	/* HKDF-Expand */
	for (i = 0; i < outlen; i += SHA512_DIGEST_SIZE) {
		u8 *p = buf;
		u8 tmp[SHA512_DIGEST_SIZE];

		ASSERT(counter != 0);
		if (i > 0) {
			memcpy(p, &output[i - SHA512_DIGEST_SIZE],
			       SHA512_DIGEST_SIZE);
			p += SHA512_DIGEST_SIZE;
		}
		memcpy(p, info, infolen);
		p += infolen;
		*p++ = counter++;
		hmac_sha512(prk, sizeof(prk), buf, p - buf, tmp);
		memcpy(&output[i], tmp, MIN(sizeof(tmp), outlen - i));
	}
	free(buf);
}

#ifdef ENABLE_ALG_TESTS
#include <openssl/evp.h>
#include <openssl/kdf.h>
static void openssl_hkdf_sha512(const u8 *ikm, size_t ikmlen,
				const u8 *salt, size_t saltlen,
				const u8 *info, size_t infolen,
				u8 *output, size_t outlen)
{
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	size_t actual_outlen = outlen;

	ASSERT(pctx != NULL);
	ASSERT(EVP_PKEY_derive_init(pctx) > 0);
	ASSERT(EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha512()) > 0);
	ASSERT(EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, ikmlen) > 0);
	ASSERT(EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, saltlen) > 0);
	ASSERT(EVP_PKEY_CTX_add1_hkdf_info(pctx, info, infolen) > 0);
	ASSERT(EVP_PKEY_derive(pctx, output, &actual_outlen) > 0);
	ASSERT(actual_outlen == outlen);
	EVP_PKEY_CTX_free(pctx);
}

static void test_hkdf_sha512(void)
{
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;

	while (num_tests--) {
		u8 ikm[SHA512_DIGEST_SIZE];
		u8 salt[SHA512_DIGEST_SIZE];
		u8 info[128];
		u8 actual_output[512];
		u8 expected_output[sizeof(actual_output)];
		size_t ikmlen = 1 + (rand() % sizeof(ikm));
		size_t saltlen = rand() % (1 + sizeof(salt));
		size_t infolen = rand() % (1 + sizeof(info));
		size_t outlen = rand() % (1 + sizeof(actual_output));

		rand_bytes(ikm, ikmlen);
		rand_bytes(salt, saltlen);
		rand_bytes(info, infolen);

		hkdf_sha512(ikm, ikmlen, salt, saltlen, info, infolen,
			    actual_output, outlen);
		openssl_hkdf_sha512(ikm, ikmlen, salt, saltlen, info, infolen,
				    expected_output, outlen);
		ASSERT(memcmp(actual_output, expected_output, outlen) == 0);
	}
}
#endif /* ENABLE_ALG_TESTS */

/*----------------------------------------------------------------------------*
 *                                 POLVAL                                     *
 *----------------------------------------------------------------------------*/

#define POLYVAL_KEY_SIZE	16
#define POLYVAL_BLOCK_SIZE	16

static void reverse_bytes(be128 *a)
{
	__le64 tmp = a->lo;

	a->lo = a->hi;
	a->hi = tmp;
	a->lo = __builtin_bswap64(a->lo);
	a->hi = __builtin_bswap64(a->hi);
}

static void polyval(const u8 key[POLYVAL_KEY_SIZE],
		    const u8 *msg, size_t msglen, be128 *out)
{
	be128 h;
	be128 tmp;
	int nblocks = msglen / POLYVAL_BLOCK_SIZE;
	int tail = msglen % POLYVAL_BLOCK_SIZE;

	reverse_bytes(out);
	memcpy(&h, key, sizeof(h));
	reverse_bytes(&h);
	gf2_128_mul_x_lle(&h);

	while (nblocks > 0) {
		memcpy(&tmp, msg, sizeof(tmp));
		reverse_bytes(&tmp);
		xor((u8 *)out, (u8 *)out, (u8 *)&tmp, sizeof(tmp));
		gf2_128_mul_lle(out, &h);
		msg += POLYVAL_BLOCK_SIZE;
		nblocks--;
	}
	if (tail) {
		memset(&tmp, 0, POLYVAL_BLOCK_SIZE);
		memcpy(&tmp, msg, tail);
		reverse_bytes(&tmp);
		xor((u8 *)out, (u8 *)out, (u8 *)&tmp, sizeof(tmp));
		gf2_128_mul_lle(out, &h);
	}
	reverse_bytes(out);
}

/*----------------------------------------------------------------------------*
 *                            AES encryption modes                            *
 *----------------------------------------------------------------------------*/

static void aes_256_xts_crypt(const u8 key[2 * AES_256_KEY_SIZE],
			      const u8 iv[AES_BLOCK_SIZE], const u8 *src,
			      u8 *dst, size_t nbytes, bool decrypting)
{
	struct aes_key tweak_key, cipher_key;
	ble128 t;
	size_t i;

	ASSERT(nbytes % AES_BLOCK_SIZE == 0);
	aes_setkey(&cipher_key, key, AES_256_KEY_SIZE);
	aes_setkey(&tweak_key, &key[AES_256_KEY_SIZE], AES_256_KEY_SIZE);
	aes_encrypt(&tweak_key, iv, (u8 *)&t);
	for (i = 0; i < nbytes; i += AES_BLOCK_SIZE) {
		xor(&dst[i], &src[i], (const u8 *)&t, AES_BLOCK_SIZE);
		if (decrypting)
			aes_decrypt(&cipher_key, &dst[i], &dst[i]);
		else
			aes_encrypt(&cipher_key, &dst[i], &dst[i]);
		xor(&dst[i], &dst[i], (const u8 *)&t, AES_BLOCK_SIZE);
		gf2_128_mul_x_ble(&t);
	}
}

static void aes_256_xts_encrypt(const u8 key[2 * AES_256_KEY_SIZE],
				const u8 iv[AES_BLOCK_SIZE], const u8 *src,
				u8 *dst, size_t nbytes)
{
	aes_256_xts_crypt(key, iv, src, dst, nbytes, false);
}

static void aes_256_xts_decrypt(const u8 key[2 * AES_256_KEY_SIZE],
				const u8 iv[AES_BLOCK_SIZE], const u8 *src,
				u8 *dst, size_t nbytes)
{
	aes_256_xts_crypt(key, iv, src, dst, nbytes, true);
}

#ifdef ENABLE_ALG_TESTS
#include <openssl/evp.h>
static void test_aes_256_xts(void)
{
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

	ASSERT(ctx != NULL);
	while (num_tests--) {
		u8 key[2 * AES_256_KEY_SIZE];
		u8 iv[AES_BLOCK_SIZE];
		u8 ptext[512];
		u8 ctext[sizeof(ptext)];
		u8 ref_ctext[sizeof(ptext)];
		u8 decrypted[sizeof(ptext)];
		const size_t datalen = ROUND_DOWN(rand() % (1 + sizeof(ptext)),
						  AES_BLOCK_SIZE);
		int outl, res;

		rand_bytes(key, sizeof(key));
		rand_bytes(iv, sizeof(iv));
		rand_bytes(ptext, datalen);

		aes_256_xts_encrypt(key, iv, ptext, ctext, datalen);
		res = EVP_EncryptInit_ex(ctx, EVP_aes_256_xts(), NULL, key, iv);
		ASSERT(res > 0);
		res = EVP_EncryptUpdate(ctx, ref_ctext, &outl, ptext, datalen);
		ASSERT(res > 0);
		ASSERT(outl == datalen);
		ASSERT(memcmp(ctext, ref_ctext, datalen) == 0);

		aes_256_xts_decrypt(key, iv, ctext, decrypted, datalen);
		ASSERT(memcmp(ptext, decrypted, datalen) == 0);
	}
	EVP_CIPHER_CTX_free(ctx);
}
#endif /* ENABLE_ALG_TESTS */

static void aes_cbc_encrypt(const struct aes_key *k,
			    const u8 iv[AES_BLOCK_SIZE],
			    const u8 *src, u8 *dst, size_t nbytes)
{
	size_t i;

	ASSERT(nbytes % AES_BLOCK_SIZE == 0);
	for (i = 0; i < nbytes; i += AES_BLOCK_SIZE) {
		xor(&dst[i], &src[i], (i == 0 ? iv : &dst[i - AES_BLOCK_SIZE]),
		    AES_BLOCK_SIZE);
		aes_encrypt(k, &dst[i], &dst[i]);
	}
}

static void aes_cbc_decrypt(const struct aes_key *k,
			    const u8 iv[AES_BLOCK_SIZE],
			    const u8 *src, u8 *dst, size_t nbytes)
{
	size_t i = nbytes;

	ASSERT(i % AES_BLOCK_SIZE == 0);
	while (i) {
		i -= AES_BLOCK_SIZE;
		aes_decrypt(k, &src[i], &dst[i]);
		xor(&dst[i], &dst[i], (i == 0 ? iv : &src[i - AES_BLOCK_SIZE]),
		    AES_BLOCK_SIZE);
	}
}

static void aes_cts_cbc_encrypt(const u8 *key, int keysize,
				const u8 iv[AES_BLOCK_SIZE],
				const u8 *src, u8 *dst, size_t nbytes)
{
	const size_t offset = ROUND_DOWN(nbytes - 1, AES_BLOCK_SIZE);
	const size_t final_bsize = nbytes - offset;
	struct aes_key k;
	u8 *pad;
	u8 buf[AES_BLOCK_SIZE];

	ASSERT(nbytes >= AES_BLOCK_SIZE);

	aes_setkey(&k, key, keysize);

	if (nbytes == AES_BLOCK_SIZE)
		return aes_cbc_encrypt(&k, iv, src, dst, nbytes);

	aes_cbc_encrypt(&k, iv, src, dst, offset);
	pad = &dst[offset - AES_BLOCK_SIZE];

	memcpy(buf, pad, AES_BLOCK_SIZE);
	xor(buf, buf, &src[offset], final_bsize);
	memcpy(&dst[offset], pad, final_bsize);
	aes_encrypt(&k, buf, pad);
}

static void aes_cts_cbc_decrypt(const u8 *key, int keysize,
				const u8 iv[AES_BLOCK_SIZE],
				const u8 *src, u8 *dst, size_t nbytes)
{
	const size_t offset = ROUND_DOWN(nbytes - 1, AES_BLOCK_SIZE);
	const size_t final_bsize = nbytes - offset;
	struct aes_key k;
	u8 *pad;

	ASSERT(nbytes >= AES_BLOCK_SIZE);

	aes_setkey(&k, key, keysize);

	if (nbytes == AES_BLOCK_SIZE)
		return aes_cbc_decrypt(&k, iv, src, dst, nbytes);

	pad = &dst[offset - AES_BLOCK_SIZE];
	aes_decrypt(&k, &src[offset - AES_BLOCK_SIZE], pad);
	xor(&dst[offset], &src[offset], pad, final_bsize);
	xor(pad, pad, &dst[offset], final_bsize);

	aes_cbc_decrypt(&k, (offset == AES_BLOCK_SIZE ?
			     iv : &src[offset - 2 * AES_BLOCK_SIZE]),
			pad, pad, AES_BLOCK_SIZE);
	aes_cbc_decrypt(&k, iv, src, dst, offset - AES_BLOCK_SIZE);
}

static void aes_256_cts_cbc_encrypt(const u8 key[AES_256_KEY_SIZE],
				    const u8 iv[AES_BLOCK_SIZE],
				    const u8 *src, u8 *dst, size_t nbytes)
{
	aes_cts_cbc_encrypt(key, AES_256_KEY_SIZE, iv, src, dst, nbytes);
}

static void aes_256_cts_cbc_decrypt(const u8 key[AES_256_KEY_SIZE],
				    const u8 iv[AES_BLOCK_SIZE],
				    const u8 *src, u8 *dst, size_t nbytes)
{
	aes_cts_cbc_decrypt(key, AES_256_KEY_SIZE, iv, src, dst, nbytes);
}

#ifdef ENABLE_ALG_TESTS
#include <openssl/modes.h>
static void aes_block128_f(const unsigned char in[16],
			   unsigned char out[16], const void *key)
{
	aes_encrypt(key, in, out);
}

static void test_aes_256_cts_cbc(void)
{
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;

	while (num_tests--) {
		u8 key[AES_256_KEY_SIZE];
		u8 iv[AES_BLOCK_SIZE];
		u8 iv_copy[AES_BLOCK_SIZE];
		u8 ptext[512];
		u8 ctext[sizeof(ptext)];
		u8 ref_ctext[sizeof(ptext)];
		u8 decrypted[sizeof(ptext)];
		const size_t datalen = 16 + (rand() % (sizeof(ptext) - 15));
		struct aes_key k;

		rand_bytes(key, sizeof(key));
		rand_bytes(iv, sizeof(iv));
		rand_bytes(ptext, datalen);

		aes_256_cts_cbc_encrypt(key, iv, ptext, ctext, datalen);

		/* OpenSSL doesn't allow datalen=AES_BLOCK_SIZE; Linux does */
		if (datalen != AES_BLOCK_SIZE) {
			aes_setkey(&k, key, sizeof(key));
			memcpy(iv_copy, iv, sizeof(iv));
			ASSERT(CRYPTO_cts128_encrypt_block(ptext, ref_ctext,
							   datalen, &k, iv_copy,
							   aes_block128_f)
			       == datalen);
			ASSERT(memcmp(ctext, ref_ctext, datalen) == 0);
		}
		aes_256_cts_cbc_decrypt(key, iv, ctext, decrypted, datalen);
		ASSERT(memcmp(ptext, decrypted, datalen) == 0);
	}
}
#endif /* ENABLE_ALG_TESTS */

static void essiv_generate_iv(const u8 orig_key[AES_128_KEY_SIZE],
			      const u8 orig_iv[AES_BLOCK_SIZE],
			      u8 real_iv[AES_BLOCK_SIZE])
{
	u8 essiv_key[SHA256_DIGEST_SIZE];
	struct aes_key essiv;

	/* AES encrypt the original IV using a hash of the original key */
	STATIC_ASSERT(SHA256_DIGEST_SIZE == AES_256_KEY_SIZE);
	sha256(orig_key, AES_128_KEY_SIZE, essiv_key);
	aes_setkey(&essiv, essiv_key, AES_256_KEY_SIZE);
	aes_encrypt(&essiv, orig_iv, real_iv);
}

static void aes_128_cbc_essiv_encrypt(const u8 key[AES_128_KEY_SIZE],
				      const u8 iv[AES_BLOCK_SIZE],
				      const u8 *src, u8 *dst, size_t nbytes)
{
	struct aes_key k;
	u8 real_iv[AES_BLOCK_SIZE];

	aes_setkey(&k, key, AES_128_KEY_SIZE);
	essiv_generate_iv(key, iv, real_iv);
	aes_cbc_encrypt(&k, real_iv, src, dst, nbytes);
}

static void aes_128_cbc_essiv_decrypt(const u8 key[AES_128_KEY_SIZE],
				      const u8 iv[AES_BLOCK_SIZE],
				      const u8 *src, u8 *dst, size_t nbytes)
{
	struct aes_key k;
	u8 real_iv[AES_BLOCK_SIZE];

	aes_setkey(&k, key, AES_128_KEY_SIZE);
	essiv_generate_iv(key, iv, real_iv);
	aes_cbc_decrypt(&k, real_iv, src, dst, nbytes);
}

static void aes_128_cts_cbc_encrypt(const u8 key[AES_128_KEY_SIZE],
				    const u8 iv[AES_BLOCK_SIZE],
				    const u8 *src, u8 *dst, size_t nbytes)
{
	aes_cts_cbc_encrypt(key, AES_128_KEY_SIZE, iv, src, dst, nbytes);
}

static void aes_128_cts_cbc_decrypt(const u8 key[AES_128_KEY_SIZE],
				    const u8 iv[AES_BLOCK_SIZE],
				    const u8 *src, u8 *dst, size_t nbytes)
{
	aes_cts_cbc_decrypt(key, AES_128_KEY_SIZE, iv, src, dst, nbytes);
}

static void aes_256_xctr_crypt(const u8 key[AES_256_KEY_SIZE],
			      const u8 iv[AES_BLOCK_SIZE], const u8 *src,
			      u8 *dst, size_t nbytes)
{
	struct aes_key k;
	int i;
	int nblocks;
	le128 ctr;
	u8 keystream[AES_BLOCK_SIZE];

	aes_setkey(&k, key, AES_256_KEY_SIZE);

	nblocks = nbytes / AES_BLOCK_SIZE;
	for (i = 0; i < nblocks; i++) {
		ctr.hi = 0;
		ctr.lo = cpu_to_le64(i + 1);
		xor((u8 *)&ctr, (u8 *)&ctr, iv, AES_BLOCK_SIZE);
		aes_encrypt(&k, (u8 *)&ctr, keystream);
		xor(dst, keystream, src, AES_BLOCK_SIZE);
		src += AES_BLOCK_SIZE;
		dst += AES_BLOCK_SIZE;
	}

	if (nbytes % AES_BLOCK_SIZE != 0) {
		ctr.hi = 0;
		ctr.lo = cpu_to_le64(nbytes / AES_BLOCK_SIZE + 1);
		xor((u8 *)&ctr, (u8 *)&ctr, iv, AES_BLOCK_SIZE);
		aes_encrypt(&k, (u8 *)&ctr, keystream);
		xor(dst, keystream, src, nbytes % AES_BLOCK_SIZE);
	}
}

#define HCTR2_IV_SIZE 32
static void aes_256_hctr2_crypt(const u8 key[AES_256_KEY_SIZE],
				const u8 iv[HCTR2_IV_SIZE], const u8 *src,
				u8 *dst, size_t nbytes, bool decrypting)
{
	be128 digest;
	u8 MM[AES_BLOCK_SIZE];
	u8 UU[AES_BLOCK_SIZE];
	u8 S[AES_BLOCK_SIZE];
	u8 hbar[AES_BLOCK_SIZE];
	u8 L[AES_BLOCK_SIZE];
	u8 padded_block[POLYVAL_BLOCK_SIZE];
	const u8 *M = src;
	const u8 *N = src + AES_BLOCK_SIZE;
	u8 *U = dst;
	u8 *V = dst + AES_BLOCK_SIZE;
	int bulk_bytes = nbytes - AES_BLOCK_SIZE;
	int remainder = bulk_bytes % AES_BLOCK_SIZE;
	struct aes_key k;
	le128 tweak_block;

	ASSERT(nbytes >= AES_BLOCK_SIZE);

	memset(hbar, 0, AES_BLOCK_SIZE);
	memset(L, 0, AES_BLOCK_SIZE);
	L[0] = 0x01;
	aes_setkey(&k, key, AES_256_KEY_SIZE);
	aes_encrypt(&k, hbar, hbar);
	aes_encrypt(&k, L, L);

	tweak_block.hi = 0;
	tweak_block.lo = cpu_to_le64(remainder ? (HCTR2_IV_SIZE * 8 * 2 + 3) :
		(HCTR2_IV_SIZE * 8 * 2 + 2));
	memset(&digest, 0, sizeof(digest));
	polyval(hbar, (u8 *)&tweak_block, POLYVAL_BLOCK_SIZE, &digest);
	polyval(hbar, iv, HCTR2_IV_SIZE, &digest);

	if (remainder == 0) {
		polyval(hbar, N, bulk_bytes, &digest);
	} else {
		polyval(hbar, N, bulk_bytes - remainder, &digest);
		memset(padded_block, 0, POLYVAL_BLOCK_SIZE);
		memcpy(padded_block, N + bulk_bytes - remainder, remainder);
		padded_block[remainder] = 0x01;
		polyval(hbar, padded_block, POLYVAL_BLOCK_SIZE, &digest);
	}

	xor(MM, M, (u8 *)&digest, AES_BLOCK_SIZE);

	if (decrypting)
		aes_decrypt(&k, MM, UU);
	else
		aes_encrypt(&k, MM, UU);

	xor(S, MM, UU, AES_BLOCK_SIZE);
	xor(S, L, S, AES_BLOCK_SIZE);

	aes_256_xctr_crypt(key, S, N, V, bulk_bytes);

	memset(&digest, 0, sizeof(digest));
	polyval(hbar, (u8 *)&tweak_block, POLYVAL_BLOCK_SIZE, &digest);
	polyval(hbar, iv, HCTR2_IV_SIZE, &digest);

	if (remainder == 0) {
		polyval(hbar, V, bulk_bytes, &digest);
	} else {
		polyval(hbar, V, bulk_bytes - remainder, &digest);
		memset(padded_block, 0, POLYVAL_BLOCK_SIZE);
		memcpy(padded_block, V + bulk_bytes - remainder, remainder);
		padded_block[remainder] = 0x01;
		polyval(hbar, padded_block, POLYVAL_BLOCK_SIZE, &digest);
	}

	xor(U, UU, (u8 *)&digest, AES_BLOCK_SIZE);
}

static void aes_256_hctr2_encrypt(const u8 key[AES_256_KEY_SIZE],
				const u8 iv[HCTR2_IV_SIZE], const u8 *src,
				u8 *dst, size_t nbytes)
{
	aes_256_hctr2_crypt(key, iv, src, dst, nbytes, false);
}

static void aes_256_hctr2_decrypt(const u8 key[AES_256_KEY_SIZE],
				const u8 iv[HCTR2_IV_SIZE], const u8 *src,
				u8 *dst, size_t nbytes)
{
	aes_256_hctr2_crypt(key, iv, src, dst, nbytes, true);
}

#ifdef ENABLE_ALG_TESTS
#include <linux/if_alg.h>
#include <sys/socket.h>
static void test_aes_256_hctr2(void)
{
	int algfd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	struct sockaddr_alg addr = {
		.salg_type = "skcipher",
		.salg_name = "hctr2(aes)",
	};
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;

	if (algfd < 0)
		die_errno("can't create AF_ALG socket");
	if (bind(algfd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
		die_errno("can't bind AF_ALG socket to HCTR2 algorithm");

	while (num_tests--) {
		u8 key[AES_256_KEY_SIZE];
		u8 iv[HCTR2_IV_SIZE];
		u8 ptext[4096];
		u8 ctext[sizeof(ptext)];
		u8 ref_ctext[sizeof(ptext)];
		u8 decrypted[sizeof(ptext)];
		const size_t datalen = 16;

		rand_bytes(key, sizeof(key));
		rand_bytes(iv, sizeof(iv));
		rand_bytes(ptext, datalen);
		memset(key, 0, sizeof(key));
		memset(iv, 0, sizeof(iv));
		memset(ptext, 0, datalen);

		aes_256_hctr2_encrypt(key, iv, ptext, ctext, datalen);
		af_alg_crypt(algfd, ALG_OP_ENCRYPT, key, sizeof(key),
			     iv, sizeof(iv), ptext, ref_ctext, datalen);
		ASSERT(memcmp(ctext, ref_ctext, datalen) == 0);

		aes_256_hctr2_decrypt(key, iv, ctext, decrypted, datalen);
		ASSERT(memcmp(ptext, decrypted, datalen) == 0);
	}
	close(algfd);
}
#endif /* ENABLE_ALG_TESTS */

/*----------------------------------------------------------------------------*
 *                           XChaCha12 stream cipher                          *
 *----------------------------------------------------------------------------*/

/*
 * References:
 *   - "XChaCha: eXtended-nonce ChaCha and AEAD_XChaCha20_Poly1305"
 *	https://tools.ietf.org/html/draft-arciszewski-xchacha-03
 *
 *   - "ChaCha, a variant of Salsa20"
 *	https://cr.yp.to/chacha/chacha-20080128.pdf
 *
 *   - "Extending the Salsa20 nonce"
 *	https://cr.yp.to/snuffle/xsalsa-20081128.pdf
 */

#define CHACHA_KEY_SIZE		32
#define XCHACHA_KEY_SIZE	CHACHA_KEY_SIZE
#define XCHACHA_NONCE_SIZE	24

static void chacha_init_state(u32 state[16], const u8 key[CHACHA_KEY_SIZE],
			      const u8 iv[16])
{
	static const u8 consts[16] = "expand 32-byte k";
	int i;

	for (i = 0; i < 4; i++)
		state[i] = get_unaligned_le32(&consts[i * sizeof(__le32)]);
	for (i = 0; i < 8; i++)
		state[4 + i] = get_unaligned_le32(&key[i * sizeof(__le32)]);
	for (i = 0; i < 4; i++)
		state[12 + i] = get_unaligned_le32(&iv[i * sizeof(__le32)]);
}

#define CHACHA_QUARTERROUND(a, b, c, d)		\
	do {					\
		a += b; d = rol32(d ^ a, 16);	\
		c += d; b = rol32(b ^ c, 12);	\
		a += b; d = rol32(d ^ a, 8);	\
		c += d; b = rol32(b ^ c, 7);	\
	} while (0)

static void chacha_permute(u32 x[16], int nrounds)
{
	do {
		/* column round */
		CHACHA_QUARTERROUND(x[0], x[4], x[8], x[12]);
		CHACHA_QUARTERROUND(x[1], x[5], x[9], x[13]);
		CHACHA_QUARTERROUND(x[2], x[6], x[10], x[14]);
		CHACHA_QUARTERROUND(x[3], x[7], x[11], x[15]);

		/* diagonal round */
		CHACHA_QUARTERROUND(x[0], x[5], x[10], x[15]);
		CHACHA_QUARTERROUND(x[1], x[6], x[11], x[12]);
		CHACHA_QUARTERROUND(x[2], x[7], x[8], x[13]);
		CHACHA_QUARTERROUND(x[3], x[4], x[9], x[14]);
	} while ((nrounds -= 2) != 0);
}

static void xchacha(const u8 key[XCHACHA_KEY_SIZE],
		    const u8 nonce[XCHACHA_NONCE_SIZE],
		    const u8 *src, u8 *dst, size_t nbytes, int nrounds)
{
	u32 state[16];
	u8 real_key[CHACHA_KEY_SIZE];
	u8 real_iv[16] = { 0 };
	size_t i, j;

	/* Compute real key using original key and first 128 nonce bits */
	chacha_init_state(state, key, nonce);
	chacha_permute(state, nrounds);
	for (i = 0; i < 8; i++) /* state words 0..3, 12..15 */
		put_unaligned_le32(state[(i < 4 ? 0 : 8) + i],
				   &real_key[i * sizeof(__le32)]);

	/* Now do regular ChaCha, using real key and remaining nonce bits */
	memcpy(&real_iv[8], nonce + 16, 8);
	chacha_init_state(state, real_key, real_iv);
	for (i = 0; i < nbytes; i += 64) {
		u32 x[16];
		__le32 keystream[16];

		memcpy(x, state, 64);
		chacha_permute(x, nrounds);
		for (j = 0; j < 16; j++)
			keystream[j] = cpu_to_le32(x[j] + state[j]);
		xor(&dst[i], &src[i], (u8 *)keystream, MIN(nbytes - i, 64));
		if (++state[12] == 0)
			state[13]++;
	}
}

static void xchacha12(const u8 key[XCHACHA_KEY_SIZE],
		      const u8 nonce[XCHACHA_NONCE_SIZE],
		      const u8 *src, u8 *dst, size_t nbytes)
{
	xchacha(key, nonce, src, dst, nbytes, 12);
}

/*----------------------------------------------------------------------------*
 *                                 Poly1305                                   *
 *----------------------------------------------------------------------------*/

/*
 * Note: this is only the Poly1305 ε-almost-∆-universal hash function, not the
 * full Poly1305 MAC.  I.e., it doesn't add anything at the end.
 */

#define POLY1305_KEY_SIZE	16
#define POLY1305_BLOCK_SIZE	16

static void poly1305(const u8 key[POLY1305_KEY_SIZE],
		     const u8 *msg, size_t msglen, le128 *out)
{
	const u32 limb_mask = 0x3ffffff;	/* limbs are base 2^26 */
	const u64 r0 = (get_unaligned_le32(key +  0) >> 0) & 0x3ffffff;
	const u64 r1 = (get_unaligned_le32(key +  3) >> 2) & 0x3ffff03;
	const u64 r2 = (get_unaligned_le32(key +  6) >> 4) & 0x3ffc0ff;
	const u64 r3 = (get_unaligned_le32(key +  9) >> 6) & 0x3f03fff;
	const u64 r4 = (get_unaligned_le32(key + 12) >> 8) & 0x00fffff;
	u32 h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
	u32 g0, g1, g2, g3, g4, ge_p_mask;

	/* Partial block support is not necessary for Adiantum */
	ASSERT(msglen % POLY1305_BLOCK_SIZE == 0);

	while (msglen) {
		u64 d0, d1, d2, d3, d4;

		/* h += *msg */
		h0 += (get_unaligned_le32(msg +  0) >> 0) & limb_mask;
		h1 += (get_unaligned_le32(msg +  3) >> 2) & limb_mask;
		h2 += (get_unaligned_le32(msg +  6) >> 4) & limb_mask;
		h3 += (get_unaligned_le32(msg +  9) >> 6) & limb_mask;
		h4 += (get_unaligned_le32(msg + 12) >> 8) | (1 << 24);

		/* h *= r */
		d0 = h0*r0 + h1*5*r4 + h2*5*r3 + h3*5*r2 + h4*5*r1;
		d1 = h0*r1 + h1*r0   + h2*5*r4 + h3*5*r3 + h4*5*r2;
		d2 = h0*r2 + h1*r1   + h2*r0   + h3*5*r4 + h4*5*r3;
		d3 = h0*r3 + h1*r2   + h2*r1   + h3*r0   + h4*5*r4;
		d4 = h0*r4 + h1*r3   + h2*r2   + h3*r1   + h4*r0;

		/* (partial) h %= 2^130 - 5 */
		d1 += d0 >> 26;		h0 = d0 & limb_mask;
		d2 += d1 >> 26;		h1 = d1 & limb_mask;
		d3 += d2 >> 26;		h2 = d2 & limb_mask;
		d4 += d3 >> 26;		h3 = d3 & limb_mask;
		h0 += (d4 >> 26) * 5;	h4 = d4 & limb_mask;
		h1 += h0 >> 26;		h0 &= limb_mask;

		msg += POLY1305_BLOCK_SIZE;
		msglen -= POLY1305_BLOCK_SIZE;
	}

	/* fully carry h */
	h2 += (h1 >> 26);	h1 &= limb_mask;
	h3 += (h2 >> 26);	h2 &= limb_mask;
	h4 += (h3 >> 26);	h3 &= limb_mask;
	h0 += (h4 >> 26) * 5;	h4 &= limb_mask;
	h1 += (h0 >> 26);	h0 &= limb_mask;

	/* if (h >= 2^130 - 5) h -= 2^130 - 5; */
	g0 = h0 + 5;
	g1 = h1 + (g0 >> 26);	g0 &= limb_mask;
	g2 = h2 + (g1 >> 26);	g1 &= limb_mask;
	g3 = h3 + (g2 >> 26);	g2 &= limb_mask;
	g4 = h4 + (g3 >> 26);	g3 &= limb_mask;
	ge_p_mask = ~((g4 >> 26) - 1); /* all 1's if h >= 2^130 - 5, else 0 */
	h0 = (h0 & ~ge_p_mask) | (g0 & ge_p_mask);
	h1 = (h1 & ~ge_p_mask) | (g1 & ge_p_mask);
	h2 = (h2 & ~ge_p_mask) | (g2 & ge_p_mask);
	h3 = (h3 & ~ge_p_mask) | (g3 & ge_p_mask);
	h4 = (h4 & ~ge_p_mask) | (g4 & ge_p_mask & limb_mask);

	/* h %= 2^128 */
	out->lo = cpu_to_le64(((u64)h2 << 52) | ((u64)h1 << 26) | h0);
	out->hi = cpu_to_le64(((u64)h4 << 40) | ((u64)h3 << 14) | (h2 >> 12));
}

/*----------------------------------------------------------------------------*
 *                          Adiantum encryption mode                          *
 *----------------------------------------------------------------------------*/

/*
 * Reference: "Adiantum: length-preserving encryption for entry-level processors"
 *	https://tosc.iacr.org/index.php/ToSC/article/view/7360
 */

#define ADIANTUM_KEY_SIZE	32
#define ADIANTUM_IV_SIZE	32
#define ADIANTUM_HASH_KEY_SIZE	((2 * POLY1305_KEY_SIZE) + NH_KEY_SIZE)

#define NH_KEY_SIZE		1072
#define NH_KEY_WORDS		(NH_KEY_SIZE / sizeof(u32))
#define NH_BLOCK_SIZE		1024
#define NH_HASH_SIZE		32
#define NH_MESSAGE_UNIT		16

static u64 nh_pass(const u32 *key, const u8 *msg, size_t msglen)
{
	u64 sum = 0;

	ASSERT(msglen % NH_MESSAGE_UNIT == 0);
	while (msglen) {
		sum += (u64)(u32)(get_unaligned_le32(msg +  0) + key[0]) *
			    (u32)(get_unaligned_le32(msg +  8) + key[2]);
		sum += (u64)(u32)(get_unaligned_le32(msg +  4) + key[1]) *
			    (u32)(get_unaligned_le32(msg + 12) + key[3]);
		key += NH_MESSAGE_UNIT / sizeof(key[0]);
		msg += NH_MESSAGE_UNIT;
		msglen -= NH_MESSAGE_UNIT;
	}
	return sum;
}

/* NH ε-almost-universal hash function */
static void nh(const u32 *key, const u8 *msg, size_t msglen,
	       u8 result[NH_HASH_SIZE])
{
	size_t i;

	for (i = 0; i < NH_HASH_SIZE; i += sizeof(__le64)) {
		put_unaligned_le64(nh_pass(key, msg, msglen), &result[i]);
		key += NH_MESSAGE_UNIT / sizeof(key[0]);
	}
}

/* Adiantum's ε-almost-∆-universal hash function */
static void adiantum_hash(const u8 key[ADIANTUM_HASH_KEY_SIZE],
			  const u8 iv[ADIANTUM_IV_SIZE],
			  const u8 *msg, size_t msglen, le128 *result)
{
	const u8 *header_poly_key = key;
	const u8 *msg_poly_key = header_poly_key + POLY1305_KEY_SIZE;
	const u8 *nh_key = msg_poly_key + POLY1305_KEY_SIZE;
	u32 nh_key_words[NH_KEY_WORDS];
	u8 header[POLY1305_BLOCK_SIZE + ADIANTUM_IV_SIZE];
	const size_t num_nh_blocks = DIV_ROUND_UP(msglen, NH_BLOCK_SIZE);
	u8 *nh_hashes = xmalloc(num_nh_blocks * NH_HASH_SIZE);
	const size_t padded_msglen = ROUND_UP(msglen, NH_MESSAGE_UNIT);
	u8 *padded_msg = xmalloc(padded_msglen);
	le128 hash1, hash2;
	size_t i;

	for (i = 0; i < NH_KEY_WORDS; i++)
		nh_key_words[i] = get_unaligned_le32(&nh_key[i * sizeof(u32)]);

	/* Hash tweak and message length with first Poly1305 key */
	put_unaligned_le64((u64)msglen * 8, header);
	put_unaligned_le64(0, &header[sizeof(__le64)]);
	memcpy(&header[POLY1305_BLOCK_SIZE], iv, ADIANTUM_IV_SIZE);
	poly1305(header_poly_key, header, sizeof(header), &hash1);

	/* Hash NH hashes of message blocks using second Poly1305 key */
	/* (using a super naive way of handling the padding) */
	memcpy(padded_msg, msg, msglen);
	memset(&padded_msg[msglen], 0, padded_msglen - msglen);
	for (i = 0; i < num_nh_blocks; i++) {
		nh(nh_key_words, &padded_msg[i * NH_BLOCK_SIZE],
		   MIN(NH_BLOCK_SIZE, padded_msglen - (i * NH_BLOCK_SIZE)),
		   &nh_hashes[i * NH_HASH_SIZE]);
	}
	poly1305(msg_poly_key, nh_hashes, num_nh_blocks * NH_HASH_SIZE, &hash2);

	/* Add the two hashes together to get the final hash */
	le128_add(result, &hash1, &hash2);

	free(nh_hashes);
	free(padded_msg);
}

static void adiantum_crypt(const u8 key[ADIANTUM_KEY_SIZE],
			   const u8 iv[ADIANTUM_IV_SIZE], const u8 *src,
			   u8 *dst, size_t nbytes, bool decrypting)
{
	u8 subkeys[AES_256_KEY_SIZE + ADIANTUM_HASH_KEY_SIZE] = { 0 };
	struct aes_key aes_key;
	union {
		u8 nonce[XCHACHA_NONCE_SIZE];
		le128 block;
	} u = { .nonce = { 1 } };
	const size_t bulk_len = nbytes - sizeof(u.block);
	le128 hash;

	ASSERT(nbytes >= sizeof(u.block));

	/* Derive subkeys */
	xchacha12(key, u.nonce, subkeys, subkeys, sizeof(subkeys));
	aes_setkey(&aes_key, subkeys, AES_256_KEY_SIZE);

	/* Hash left part and add to right part */
	adiantum_hash(&subkeys[AES_256_KEY_SIZE], iv, src, bulk_len, &hash);
	memcpy(&u.block, &src[bulk_len], sizeof(u.block));
	le128_add(&u.block, &u.block, &hash);

	if (!decrypting) /* Encrypt right part with block cipher */
		aes_encrypt(&aes_key, u.nonce, u.nonce);

	/* Encrypt left part with stream cipher, using the computed nonce */
	u.nonce[sizeof(u.block)] = 1;
	xchacha12(key, u.nonce, src, dst, bulk_len);

	if (decrypting) /* Decrypt right part with block cipher */
		aes_decrypt(&aes_key, u.nonce, u.nonce);

	/* Finalize right part by subtracting hash of left part */
	adiantum_hash(&subkeys[AES_256_KEY_SIZE], iv, dst, bulk_len, &hash);
	le128_sub(&u.block, &u.block, &hash);
	memcpy(&dst[bulk_len], &u.block, sizeof(u.block));
}

static void adiantum_encrypt(const u8 key[ADIANTUM_KEY_SIZE],
			     const u8 iv[ADIANTUM_IV_SIZE],
			     const u8 *src, u8 *dst, size_t nbytes)
{
	adiantum_crypt(key, iv, src, dst, nbytes, false);
}

static void adiantum_decrypt(const u8 key[ADIANTUM_KEY_SIZE],
			     const u8 iv[ADIANTUM_IV_SIZE],
			     const u8 *src, u8 *dst, size_t nbytes)
{
	adiantum_crypt(key, iv, src, dst, nbytes, true);
}

#ifdef ENABLE_ALG_TESTS
static void test_adiantum(void)
{
	int algfd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	struct sockaddr_alg addr = {
		.salg_type = "skcipher",
		.salg_name = "adiantum(xchacha12,aes)",
	};
	unsigned long num_tests = NUM_ALG_TEST_ITERATIONS;

	if (algfd < 0)
		die_errno("can't create AF_ALG socket");
	if (bind(algfd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
		die_errno("can't bind AF_ALG socket to Adiantum algorithm");

	while (num_tests--) {
		u8 key[ADIANTUM_KEY_SIZE];
		u8 iv[ADIANTUM_IV_SIZE];
		u8 ptext[4096];
		u8 ctext[sizeof(ptext)];
		u8 ref_ctext[sizeof(ptext)];
		u8 decrypted[sizeof(ptext)];
		const size_t datalen = 16 + (rand() % (sizeof(ptext) - 15));

		rand_bytes(key, sizeof(key));
		rand_bytes(iv, sizeof(iv));
		rand_bytes(ptext, datalen);

		adiantum_encrypt(key, iv, ptext, ctext, datalen);
		af_alg_crypt(algfd, ALG_OP_ENCRYPT, key, sizeof(key),
			     iv, sizeof(iv), ptext, ref_ctext, datalen);
		ASSERT(memcmp(ctext, ref_ctext, datalen) == 0);

		adiantum_decrypt(key, iv, ctext, decrypted, datalen);
		ASSERT(memcmp(ptext, decrypted, datalen) == 0);
	}
	close(algfd);
}
#endif /* ENABLE_ALG_TESTS */

/*----------------------------------------------------------------------------*
 *                               SipHash-2-4                                  *
 *----------------------------------------------------------------------------*/

/*
 * Reference: "SipHash: a fast short-input PRF"
 *	https://cr.yp.to/siphash/siphash-20120918.pdf
 */

#define SIPROUND						\
	do {							\
		v0 += v1;	    v2 += v3;			\
		v1 = rol64(v1, 13); v3 = rol64(v3, 16);		\
		v1 ^= v0;	    v3 ^= v2;			\
		v0 = rol64(v0, 32);				\
		v2 += v1;	    v0 += v3;			\
		v1 = rol64(v1, 17); v3 = rol64(v3, 21);		\
		v1 ^= v2;	    v3 ^= v0;			\
		v2 = rol64(v2, 32);				\
	} while (0)

/* Compute the SipHash-2-4 of a 64-bit number when formatted as little endian */
static u64 siphash_1u64(const u64 key[2], u64 data)
{
	u64 v0 = key[0] ^ 0x736f6d6570736575ULL;
	u64 v1 = key[1] ^ 0x646f72616e646f6dULL;
	u64 v2 = key[0] ^ 0x6c7967656e657261ULL;
	u64 v3 = key[1] ^ 0x7465646279746573ULL;
	u64 m[2] = {data, (u64)sizeof(data) << 56};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(m); i++) {
		v3 ^= m[i];
		SIPROUND;
		SIPROUND;
		v0 ^= m[i];
	}

	v2 ^= 0xff;
	for (i = 0; i < 4; i++)
		SIPROUND;
	return v0 ^ v1 ^ v2 ^ v3;
}

/*----------------------------------------------------------------------------*
 *                               Main program                                 *
 *----------------------------------------------------------------------------*/

#define FILE_NONCE_SIZE		16
#define UUID_SIZE		16
#define MAX_KEY_SIZE		64
#define MAX_IV_SIZE		ADIANTUM_IV_SIZE

static const struct fscrypt_cipher {
	const char *name;
	void (*encrypt)(const u8 *key, const u8 *iv, const u8 *src,
			u8 *dst, size_t nbytes);
	void (*decrypt)(const u8 *key, const u8 *iv, const u8 *src,
			u8 *dst, size_t nbytes);
	int keysize;
	int min_input_size;
} fscrypt_ciphers[] = {
	{
		.name = "AES-256-XTS",
		.encrypt = aes_256_xts_encrypt,
		.decrypt = aes_256_xts_decrypt,
		.keysize = 2 * AES_256_KEY_SIZE,
	}, {
		.name = "AES-256-CTS-CBC",
		.encrypt = aes_256_cts_cbc_encrypt,
		.decrypt = aes_256_cts_cbc_decrypt,
		.keysize = AES_256_KEY_SIZE,
		.min_input_size = AES_BLOCK_SIZE,
	}, {
		.name = "AES-128-CBC-ESSIV",
		.encrypt = aes_128_cbc_essiv_encrypt,
		.decrypt = aes_128_cbc_essiv_decrypt,
		.keysize = AES_128_KEY_SIZE,
	}, {
		.name = "AES-128-CTS-CBC",
		.encrypt = aes_128_cts_cbc_encrypt,
		.decrypt = aes_128_cts_cbc_decrypt,
		.keysize = AES_128_KEY_SIZE,
		.min_input_size = AES_BLOCK_SIZE,
	}, {
		.name = "AES-256-HCTR2",
		.encrypt = aes_256_hctr2_encrypt,
		.decrypt = aes_256_hctr2_decrypt,
		.keysize = AES_256_KEY_SIZE,
		.min_input_size = AES_BLOCK_SIZE,
	}, {
		.name = "Adiantum",
		.encrypt = adiantum_encrypt,
		.decrypt = adiantum_decrypt,
		.keysize = ADIANTUM_KEY_SIZE,
		.min_input_size = AES_BLOCK_SIZE,
	}
};

static const struct fscrypt_cipher *find_fscrypt_cipher(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fscrypt_ciphers); i++) {
		if (strcmp(fscrypt_ciphers[i].name, name) == 0)
			return &fscrypt_ciphers[i];
	}
	return NULL;
}

union fscrypt_iv {
	/* usual IV format */
	struct {
		/* logical block number within the file */
		__le64 block_number;

		/* per-file nonce; only set in DIRECT_KEY mode */
		u8 nonce[FILE_NONCE_SIZE];
	};
	/* IV format for IV_INO_LBLK_* modes */
	struct {
		/*
		 * IV_INO_LBLK_64: logical block number within the file
		 * IV_INO_LBLK_32: hashed inode number + logical block number
		 *		   within the file, mod 2^32
		 */
		__le32 block_number32;

		/* IV_INO_LBLK_64: inode number */
		__le32 inode_number;
	};
	/* Any extra bytes up to the algorithm's IV size must be zeroed */
	u8 bytes[MAX_IV_SIZE];
};

static void crypt_loop(const struct fscrypt_cipher *cipher, const u8 *key,
		       union fscrypt_iv *iv, bool decrypting,
		       size_t block_size, size_t padding, bool is_bnum_32bit)
{
	u8 *buf = xmalloc(block_size);
	size_t res;

	while ((res = xread(STDIN_FILENO, buf, block_size)) > 0) {
		size_t crypt_len = block_size;

		if (padding > 0) {
			crypt_len = MAX(res, cipher->min_input_size);
			crypt_len = ROUND_UP(crypt_len, padding);
			crypt_len = MIN(crypt_len, block_size);
		}
		ASSERT(crypt_len >= res);
		memset(&buf[res], 0, crypt_len - res);

		if (decrypting)
			cipher->decrypt(key, iv->bytes, buf, buf, crypt_len);
		else
			cipher->encrypt(key, iv->bytes, buf, buf, crypt_len);

		full_write(STDOUT_FILENO, buf, crypt_len);

		if (is_bnum_32bit)
			iv->block_number32 = cpu_to_le32(
					le32_to_cpu(iv->block_number32) + 1);
		else
			iv->block_number = cpu_to_le64(
					le64_to_cpu(iv->block_number) + 1);
	}
	free(buf);
}

/* The supported key derivation functions */
enum kdf_algorithm {
	KDF_NONE,
	KDF_AES_128_ECB,
	KDF_HKDF_SHA512,
};

static enum kdf_algorithm parse_kdf_algorithm(const char *arg)
{
	if (strcmp(arg, "none") == 0)
		return KDF_NONE;
	if (strcmp(arg, "AES-128-ECB") == 0)
		return KDF_AES_128_ECB;
	if (strcmp(arg, "HKDF-SHA512") == 0)
		return KDF_HKDF_SHA512;
	die("Unknown KDF: %s", arg);
}

static u8 parse_mode_number(const char *arg)
{
	char *tmp;
	long num = strtol(arg, &tmp, 10);

	if (num <= 0 || *tmp || (u8)num != num)
		die("Invalid mode number: %s", arg);
	return num;
}

struct key_and_iv_params {
	u8 master_key[MAX_KEY_SIZE];
	int master_key_size;
	enum kdf_algorithm kdf;
	u8 mode_num;
	u8 file_nonce[FILE_NONCE_SIZE];
	bool file_nonce_specified;
	bool iv_ino_lblk_64;
	bool iv_ino_lblk_32;
	u64 block_number;
	u64 inode_number;
	u8 fs_uuid[UUID_SIZE];
	bool fs_uuid_specified;
};

#define HKDF_CONTEXT_KEY_IDENTIFIER	1
#define HKDF_CONTEXT_PER_FILE_ENC_KEY	2
#define HKDF_CONTEXT_DIRECT_KEY		3
#define HKDF_CONTEXT_IV_INO_LBLK_64_KEY	4
#define HKDF_CONTEXT_DIRHASH_KEY	5
#define HKDF_CONTEXT_IV_INO_LBLK_32_KEY	6
#define HKDF_CONTEXT_INODE_HASH_KEY	7

/* Hash the file's inode number using SipHash keyed by a derived key */
static u32 hash_inode_number(const struct key_and_iv_params *params)
{
	u8 info[9] = "fscrypt";
	union {
		u64 words[2];
		u8 bytes[16];
	} hash_key;

	info[8] = HKDF_CONTEXT_INODE_HASH_KEY;
	hkdf_sha512(params->master_key, params->master_key_size,
		    NULL, 0, info, sizeof(info),
		    hash_key.bytes, sizeof(hash_key));

	hash_key.words[0] = get_unaligned_le64(&hash_key.bytes[0]);
	hash_key.words[1] = get_unaligned_le64(&hash_key.bytes[8]);

	return (u32)siphash_1u64(hash_key.words, params->inode_number);
}

/*
 * Get the key and starting IV with which the encryption will actually be done.
 * If a KDF was specified, a subkey is derived from the master key and the mode
 * number or file nonce.  Otherwise, the master key is used directly.
 */
static void get_key_and_iv(const struct key_and_iv_params *params,
			   u8 *real_key, size_t real_key_size,
			   union fscrypt_iv *iv)
{
	bool file_nonce_in_iv = false;
	struct aes_key aes_key;
	u8 info[8 + 1 + 1 + UUID_SIZE] = "fscrypt";
	size_t infolen = 8;
	size_t i;

	ASSERT(real_key_size <= params->master_key_size);

	memset(iv, 0, sizeof(*iv));

	/* Overridden later for iv_ino_lblk_{64,32} */
	iv->block_number = cpu_to_le64(params->block_number);

	if (params->iv_ino_lblk_64 || params->iv_ino_lblk_32) {
		const char *opt = params->iv_ino_lblk_64 ? "--iv-ino-lblk-64" :
							   "--iv-ino-lblk-32";
		if (params->iv_ino_lblk_64 && params->iv_ino_lblk_32)
			die("--iv-ino-lblk-64 and --iv-ino-lblk-32 are mutually exclusive");
		if (params->kdf != KDF_HKDF_SHA512)
			die("%s requires --kdf=HKDF-SHA512", opt);
		if (!params->fs_uuid_specified)
			die("%s requires --fs-uuid", opt);
		if (params->inode_number == 0)
			die("%s requires --inode-number", opt);
		if (params->mode_num == 0)
			die("%s requires --mode-num", opt);
		if (params->block_number > UINT32_MAX)
			die("%s can't use --block-number > UINT32_MAX", opt);
		if (params->inode_number > UINT32_MAX)
			die("%s can't use --inode-number > UINT32_MAX", opt);
	}

	switch (params->kdf) {
	case KDF_NONE:
		if (params->mode_num != 0)
			die("--mode-num isn't supported with --kdf=none");
		memcpy(real_key, params->master_key, real_key_size);
		file_nonce_in_iv = true;
		break;
	case KDF_AES_128_ECB:
		if (!params->file_nonce_specified)
			die("--file-nonce is required with --kdf=AES-128-ECB");
		if (params->mode_num != 0)
			die("--mode-num isn't supported with --kdf=AES-128-ECB");
		STATIC_ASSERT(FILE_NONCE_SIZE == AES_128_KEY_SIZE);
		ASSERT(real_key_size % AES_BLOCK_SIZE == 0);
		aes_setkey(&aes_key, params->file_nonce, AES_128_KEY_SIZE);
		for (i = 0; i < real_key_size; i += AES_BLOCK_SIZE)
			aes_encrypt(&aes_key, &params->master_key[i],
				    &real_key[i]);
		break;
	case KDF_HKDF_SHA512:
		if (params->iv_ino_lblk_64) {
			info[infolen++] = HKDF_CONTEXT_IV_INO_LBLK_64_KEY;
			info[infolen++] = params->mode_num;
			memcpy(&info[infolen], params->fs_uuid, UUID_SIZE);
			infolen += UUID_SIZE;
			iv->block_number32 = cpu_to_le32(params->block_number);
			iv->inode_number = cpu_to_le32(params->inode_number);
		} else if (params->iv_ino_lblk_32) {
			info[infolen++] = HKDF_CONTEXT_IV_INO_LBLK_32_KEY;
			info[infolen++] = params->mode_num;
			memcpy(&info[infolen], params->fs_uuid, UUID_SIZE);
			infolen += UUID_SIZE;
			iv->block_number32 =
				cpu_to_le32(hash_inode_number(params) +
					    params->block_number);
			iv->inode_number = 0;
		} else if (params->mode_num != 0) {
			info[infolen++] = HKDF_CONTEXT_DIRECT_KEY;
			info[infolen++] = params->mode_num;
			file_nonce_in_iv = true;
		} else if (params->file_nonce_specified) {
			info[infolen++] = HKDF_CONTEXT_PER_FILE_ENC_KEY;
			memcpy(&info[infolen], params->file_nonce,
			       FILE_NONCE_SIZE);
			infolen += FILE_NONCE_SIZE;
		} else {
			die("With --kdf=HKDF-SHA512, at least one of --file-nonce and --mode-num must be specified");
		}
		hkdf_sha512(params->master_key, params->master_key_size,
			    NULL, 0, info, infolen, real_key, real_key_size);
		break;
	default:
		ASSERT(0);
	}

	if (file_nonce_in_iv && params->file_nonce_specified)
		memcpy(iv->nonce, params->file_nonce, FILE_NONCE_SIZE);
}

enum {
	OPT_BLOCK_NUMBER,
	OPT_BLOCK_SIZE,
	OPT_DECRYPT,
	OPT_FILE_NONCE,
	OPT_FS_UUID,
	OPT_HELP,
	OPT_INODE_NUMBER,
	OPT_IV_INO_LBLK_32,
	OPT_IV_INO_LBLK_64,
	OPT_KDF,
	OPT_MODE_NUM,
	OPT_PADDING,
};

static const struct option longopts[] = {
	{ "block-number",    required_argument, NULL, OPT_BLOCK_NUMBER },
	{ "block-size",      required_argument, NULL, OPT_BLOCK_SIZE },
	{ "decrypt",         no_argument,       NULL, OPT_DECRYPT },
	{ "file-nonce",      required_argument, NULL, OPT_FILE_NONCE },
	{ "fs-uuid",         required_argument, NULL, OPT_FS_UUID },
	{ "help",            no_argument,       NULL, OPT_HELP },
	{ "inode-number",    required_argument, NULL, OPT_INODE_NUMBER },
	{ "iv-ino-lblk-32",  no_argument,       NULL, OPT_IV_INO_LBLK_32 },
	{ "iv-ino-lblk-64",  no_argument,       NULL, OPT_IV_INO_LBLK_64 },
	{ "kdf",             required_argument, NULL, OPT_KDF },
	{ "mode-num",        required_argument, NULL, OPT_MODE_NUM },
	{ "padding",         required_argument, NULL, OPT_PADDING },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	size_t block_size = 4096;
	bool decrypting = false;
	struct key_and_iv_params params;
	size_t padding = 0;
	const struct fscrypt_cipher *cipher;
	u8 real_key[MAX_KEY_SIZE];
	union fscrypt_iv iv;
	char *tmp;
	int c;

	memset(&params, 0, sizeof(params));

	aes_init();

#ifdef ENABLE_ALG_TESTS
	test_aes();
	test_sha2();
	test_hkdf_sha512();
	test_aes_256_xts();
	test_aes_256_cts_cbc();
	test_adiantum();
	test_aes_256_hctr2();
#endif

	while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (c) {
		case OPT_BLOCK_NUMBER:
			errno = 0;
			params.block_number = strtoull(optarg, &tmp, 10);
			if (*tmp || errno)
				die("Invalid block number: %s", optarg);
			break;
		case OPT_BLOCK_SIZE:
			errno = 0;
			block_size = strtoul(optarg, &tmp, 10);
			if (block_size <= 0 || *tmp || errno)
				die("Invalid block size: %s", optarg);
			break;
		case OPT_DECRYPT:
			decrypting = true;
			break;
		case OPT_FILE_NONCE:
			if (hex2bin(optarg, params.file_nonce, FILE_NONCE_SIZE)
			    != FILE_NONCE_SIZE)
				die("Invalid file nonce: %s", optarg);
			params.file_nonce_specified = true;
			break;
		case OPT_FS_UUID:
			if (hex2bin(optarg, params.fs_uuid, UUID_SIZE)
			    != UUID_SIZE)
				die("Invalid filesystem UUID: %s", optarg);
			params.fs_uuid_specified = true;
			break;
		case OPT_HELP:
			usage(stdout);
			return 0;
		case OPT_INODE_NUMBER:
			errno = 0;
			params.inode_number = strtoull(optarg, &tmp, 10);
			if (params.inode_number <= 0 || *tmp || errno)
				die("Invalid inode number: %s", optarg);
			break;
		case OPT_IV_INO_LBLK_32:
			params.iv_ino_lblk_32 = true;
			break;
		case OPT_IV_INO_LBLK_64:
			params.iv_ino_lblk_64 = true;
			break;
		case OPT_KDF:
			params.kdf = parse_kdf_algorithm(optarg);
			break;
		case OPT_MODE_NUM:
			params.mode_num = parse_mode_number(optarg);
			break;
		case OPT_PADDING:
			padding = strtoul(optarg, &tmp, 10);
			if (padding <= 0 || *tmp || !is_power_of_2(padding) ||
			    padding > INT_MAX)
				die("Invalid padding amount: %s", optarg);
			break;
		default:
			usage(stderr);
			return 2;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2) {
		usage(stderr);
		return 2;
	}

	cipher = find_fscrypt_cipher(argv[0]);
	if (cipher == NULL)
		die("Unknown cipher: %s", argv[0]);

	if (block_size < cipher->min_input_size)
		die("Block size of %zu bytes is too small for cipher %s",
		    block_size, cipher->name);

	params.master_key_size = hex2bin(argv[1], params.master_key,
					 MAX_KEY_SIZE);
	if (params.master_key_size < 0)
		die("Invalid master_key: %s", argv[1]);
	if (params.master_key_size < cipher->keysize)
		die("Master key is too short for cipher %s", cipher->name);

	get_key_and_iv(&params, real_key, cipher->keysize, &iv);

	crypt_loop(cipher, real_key, &iv, decrypting, block_size, padding,
		   params.iv_ino_lblk_64 || params.iv_ino_lblk_32);
	return 0;
}

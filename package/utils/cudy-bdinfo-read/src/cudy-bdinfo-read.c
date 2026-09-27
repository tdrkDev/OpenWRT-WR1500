// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Read the board data of Cudy devices
 *
 * The "bdinfo" flash partition holds "key = value" records written in the
 * factory: MAC address, country, the default Wi-Fi password ("pin"), serial
 * number and more. Its layout:
 *
 *   0x0000  4      version, big endian
 *   0x0004  56700  the records, DES-CBC encrypted (zero IV), ending with a
 *                  "BDINFO_END" line and a NUL byte
 *   0xdd80  128    RSA signature of the bytes before it
 *   0xde00  6      MAC address (binary)
 *
 * Only the encrypted records are read here; the signature is not checked.
 * The options follow cudy-bdinfo of the packages feed, which needs OpenSSL.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BDINFO_TEXT_OFS		4
#define BDINFO_TEXT_LEN		56700
#define BDINFO_END		"BDINFO_END"

/* DES_string_to_key("88T3j05dtFu8=") of the vendor library */
static const uint8_t bdinfo_key[8] = {
	0x97, 0x5e, 0x07, 0x19, 0xc4, 0xd5, 0x92, 0x79
};

/* DES tables (FIPS 46-3), bit 1 = most significant bit */
static const uint8_t des_ip[64] = {
	58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
	62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
	57, 49, 41, 33, 25, 17,  9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};

static const uint8_t des_fp[64] = {
	40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
	38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
	36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
	34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41,  9, 49, 17, 57, 25
};

static const uint8_t des_e[48] = {
	32,  1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,
	 8,  9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
	16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
	24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32,  1
};

static const uint8_t des_p[32] = {
	16,  7, 20, 21, 29, 12, 28, 17,  1, 15, 23, 26,  5, 18, 31, 10,
	 2,  8, 24, 14, 32, 27,  3,  9, 19, 13, 30,  6, 22, 11,  4, 25
};

static const uint8_t des_pc1[56] = {
	57, 49, 41, 33, 25, 17,  9,  1, 58, 50, 42, 34, 26, 18,
	10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
	63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
	14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4
};

static const uint8_t des_pc2[48] = {
	14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
	23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
	41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};

static const uint8_t des_shifts[16] = {
	1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

static const uint8_t des_sbox[8][64] = {
	{
		14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,
		 0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,
		 4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0,
		15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13
	}, {
		15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,
		 3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,
		 0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15,
		13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9
	}, {
		10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,
		13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,
		13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
		 1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12
	}, {
		 7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,
		13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,
		10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
		 3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14
	}, {
		 2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9,
		14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6,
		 4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14,
		11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3
	}, {
		12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
		10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
		 9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
		 4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13
	}, {
		 4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
		13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
		 1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
		 6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12
	}, {
		13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
		 1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
		 7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
		 2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11
	}
};

static char *progname;

static uint64_t des_permute(uint64_t in, const uint8_t *table, int n,
			    int in_bits)
{
	uint64_t out = 0;
	int i;

	for (i = 0; i < n; i++)
		out = (out << 1) | ((in >> (in_bits - table[i])) & 1);

	return out;
}

static uint64_t load_be64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];

	return v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 7; i >= 0; i--) {
		p[i] = v & 0xff;
		v >>= 8;
	}
}

static void des_set_key(const uint8_t *key, uint64_t *subkeys)
{
	uint64_t cd = des_permute(load_be64(key), des_pc1, 56, 64);
	uint32_t c = cd >> 28, d = cd & 0xfffffff;
	int i, s;

	for (i = 0; i < 16; i++) {
		s = des_shifts[i];
		c = ((c << s) | (c >> (28 - s))) & 0xfffffff;
		d = ((d << s) | (d >> (28 - s))) & 0xfffffff;
		subkeys[i] = des_permute(((uint64_t)c << 28) | d, des_pc2, 48, 56);
	}
}

static uint32_t des_f(uint32_t r, uint64_t subkey)
{
	uint64_t x = des_permute(r, des_e, 48, 32) ^ subkey;
	uint32_t out = 0;
	int i, b;

	for (i = 0; i < 8; i++) {
		b = (x >> (42 - 6 * i)) & 0x3f;
		/* row: outer bits, column: inner four bits */
		out = (out << 4) |
		      des_sbox[i][(((b >> 4) & 2) | (b & 1)) * 16 + ((b >> 1) & 0xf)];
	}

	return des_permute(out, des_p, 32, 32);
}

static uint64_t des_decrypt(uint64_t block, const uint64_t *subkeys)
{
	uint64_t x = des_permute(block, des_ip, 64, 64);
	uint32_t l = x >> 32, r = x, t;
	int i;

	for (i = 15; i >= 0; i--) {
		t = r;
		r = l ^ des_f(r, subkeys[i]);
		l = t;
	}

	return des_permute(((uint64_t)r << 32) | l, des_fp, 64, 64);
}

/*
 * Decrypt the records up to the first NUL byte. Returns the NUL terminated
 * text or NULL if there is none.
 */
static char *bdinfo_decrypt(const uint8_t *data)
{
	uint64_t subkeys[16], iv = 0, c;
	static uint8_t text[BDINFO_TEXT_LEN + 1];
	int i;

	des_set_key(bdinfo_key, subkeys);

	for (i = 0; i + 8 <= BDINFO_TEXT_LEN; i += 8) {
		c = load_be64(data + i);
		store_be64(text + i, des_decrypt(c, subkeys) ^ iv);
		iv = c;

		if (memchr(text + i, '\0', 8))
			return (char *)text;
	}

	return NULL;
}

/*
 * Find the next "key = value" record. Returns false at the end of the
 * records.
 */
static bool bdinfo_next(char **pos, char **name, char **value)
{
	char *line, *end, *name_end, *p;

	while (**pos) {
		line = *pos;
		end = strchr(line, '\n');
		if (end) {
			*end = '\0';
			*pos = end + 1;
		} else {
			*pos = line + strlen(line);
		}

		if (!strcmp(line, BDINFO_END))
			return false;

		name_end = line + strcspn(line, " \t=");
		p = name_end + strspn(name_end, " \t");
		if (name_end == line || *p != '=')
			continue;

		*name_end = '\0';
		p++;
		p += strspn(p, " \t");
		if (!*p)
			continue;

		*name = line;
		*value = p;
		return true;
	}

	return false;
}

static bool bdinfo_complete(const char *text)
{
	const char *p = strstr(text, "\n" BDINFO_END);

	return p && (p[sizeof(BDINFO_END)] == '\0' || p[sizeof(BDINFO_END)] == '\n');
}

static void usage(int status)
{
	FILE *stream = (status != EXIT_SUCCESS) ? stderr : stdout;

	fprintf(stream, "Usage: %s -i <file> [-k <key>]\n", progname);
	fprintf(stream,
	"\n"
	"Options:\n"
	"  -h              show this screen\n"
	"  -i <file>       read the bdinfo partition/file <file>\n"
	"  -k <key>        display the value of <key> instead of all records\n"
	);

	exit(status);
}

int main(int argc, char *argv[])
{
	static uint8_t data[BDINFO_TEXT_OFS + BDINFO_TEXT_LEN];
	char *input_file = NULL, *key = NULL;
	char *text, *pos, *name, *value;
	bool found = false;
	size_t len;
	FILE *f;
	int c;

	progname = argv[0];

	while ((c = getopt(argc, argv, "hi:k:")) != -1) {
		switch (c) {
		case 'h':
			usage(EXIT_SUCCESS);
			break;
		case 'i':
			input_file = optarg;
			break;
		case 'k':
			key = optarg;
			break;
		default:
			usage(EXIT_FAILURE);
			break;
		}
	}

	if (!input_file) {
		fprintf(stderr, "ERROR: No input file (-i <file>) given!\n");
		return EXIT_FAILURE;
	}

	f = fopen(input_file, "r");
	if (!f) {
		fprintf(stderr, "ERROR: Failed to open %s: %s\n", input_file,
			strerror(errno));
		return EXIT_FAILURE;
	}

	len = fread(data, 1, sizeof(data), f);
	fclose(f);
	if (len != sizeof(data)) {
		fprintf(stderr, "ERROR: %s is too short\n", input_file);
		return EXIT_FAILURE;
	}

	text = bdinfo_decrypt(data + BDINFO_TEXT_OFS);
	if (!text || !bdinfo_complete(text)) {
		fprintf(stderr, "ERROR: No board data found in %s\n", input_file);
		return EXIT_FAILURE;
	}

	pos = text;
	while (bdinfo_next(&pos, &name, &value)) {
		if (!key) {
			printf("%s = %s\n", name, value);
			found = true;
		} else if (!strcmp(name, key)) {
			printf("%s\n", value);
			return EXIT_SUCCESS;
		}
	}

	if (!found) {
		if (key)
			fprintf(stderr, "ERROR: No value found for key %s!\n", key);
		else
			fprintf(stderr, "ERROR: No records found!\n");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

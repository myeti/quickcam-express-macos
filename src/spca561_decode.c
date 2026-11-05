/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Spca561decoder (C) 2005 Andrzej Szombierski [qq@kuku.eu.org]
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * Note this code was originally licensed under the GNU GPL instead of the
 * GNU LGPL, its license has been changed with permission (see the permission
 * mail at the end of lib/libv4lconvert/spca561-decompress.c in v4l-utils).
 *
 * Taken from v4l-utils' libv4lconvert and modified in 2026 by Magalon:
 * standalone header, spca561_decode() returns an error code instead of
 * silently keeping the previous output, clamp/diff tables computed at run
 * time (with a wider clamp range), abs_clamp15 completed to 39 entries,
 * out-of-range codes on corrupt input rejected instead of read past tables.
 */

/*
 *	Decoder for compressed spca561 images
 *	It was developed for "Labtec WebCam Elch 2(SPCA561A)" (046d:0929)
 *	but it might work with other spca561 cameras
 */
#include <string.h>
#include "spca561_decode.h"

/*fixme: not reentrant */
static unsigned int bit_bucket;
static const unsigned char *input_ptr, *input_end;

static inline void refill(int *bitfill)
{
	if (*bitfill < 8) {
		bit_bucket = (bit_bucket << 8) | *(input_ptr++);
		*bitfill += 8;
	}
}

static inline int nbits(int *bitfill, int n)
{
	bit_bucket = (bit_bucket << 8) | *(input_ptr++);
	*bitfill -= n;
	return (bit_bucket >> (*bitfill & 0xff)) & ((1 << n) - 1);
}

static inline int _nbits(int *bitfill, int n)
{
	*bitfill -= n;
	return (bit_bucket >> (*bitfill & 0xff)) & ((1 << n) - 1);
}

static int fun_A(int *bitfill)
{
	int ret;
	static int tab[] = {
		12, 13, 14, 15, 16, 17, 18, 19, -12, -13, -14, -15,
		-16, -17, -18, -19, -19
	};

	ret = tab[nbits(bitfill, 4)];

	refill(bitfill);
	return ret;
}

static int fun_B(int *bitfill)
{
	static int tab1[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 31, 31,
		31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
		16, 17,
		18,
		19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
	};
	static int tab[] = {
		4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, -5,
		-6, -7, -8, -9, -10, -11, -12, -13, -14, -15, -16, -17,
		-18, -19
	};
	unsigned int tmp;

	tmp = nbits(bitfill, 7) - 68;
	refill(bitfill);
	if (tmp > 47 || tab1[tmp] >= (int)(sizeof(tab) / sizeof(tab[0])))
		return 0xff;
	return tab[tab1[tmp]];
}

static int fun_C(int *bitfill, int gkw)
{
	static int tab1[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 23, 23, 23, 23, 23, 23,
		23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
		12, 13,
		14,
		15, 16, 17, 18, 19, 20, 21, 22
	};
	static int tab[] = {
		8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, -9, -10, -11,
		-12, -13, -14, -15, -16, -17, -18, -19
	};
	unsigned int tmp;

	if (gkw == 0xfe) {
		if (nbits(bitfill, 1) == 0)
			return 7;

		return -8;
	}

	if (gkw != 0xff)
		return 0xff;

	tmp = nbits(bitfill, 7) - 72;
	if (tmp > 43 || tab1[tmp] >= (int)(sizeof(tab) / sizeof(tab[0])))
		return 0xff;

	refill(bitfill);
	return tab[tab1[tmp]];
}

static int fun_D(int *bitfill, int gkw)
{
	if (gkw == 0xfd) {
		if (nbits(bitfill, 1) == 0)
			return 12;
		return -13;
	}

	if (gkw == 0xfc) {
		if (nbits(bitfill, 1) == 0)
			return 13;
		return -14;
	}

	if (gkw == 0xfe) {
		switch (nbits(bitfill, 2)) {
		case 0:
			return 14;
		case 1:
			return -15;
		case 2:
			return 15;
		case 3:
			return -16;
		}
	}

	if (gkw == 0xff) {
		switch (nbits(bitfill, 3)) {
		case 4:
			return 16;
		case 5:
			return -17;
		case 6:
			return 17;
		case 7:
			return -18;
		case 2:
			return _nbits(bitfill, 1) ? -19 : 18;
		case 3:
			(*bitfill)--;
			return 18;
		}
		return 0xff;
	}
	return gkw;
}

static int fun_E(int cur_byte, int *bitfill)
{
	static int tab0[] = { 0, -1, 1, -2, 2, -3, 3, -4 };
	static int tab1[] = { 4, -5, 5, -6, 6, -7, 7, -8 };
	static int tab2[] = { 8, -9, 9, -10, 10, -11, 11, -12 };
	static int tab3[] = { 12, -13, 13, -14, 14, -15, 15, -16 };
	static int tab4[] = { 16, -17, 17, -18, 18, -19, 19, -19 };

	if ((cur_byte & 0xf0) >= 0x80) {
		*bitfill -= 4;
		return tab0[(cur_byte >> 4) & 7];
	}
	if ((cur_byte & 0xc0) == 0x40) {
		*bitfill -= 5;
		return tab1[(cur_byte >> 3) & 7];

	}
	if ((cur_byte & 0xe0) == 0x20) {
		*bitfill -= 6;
		return tab2[(cur_byte >> 2) & 7];

	}
	if ((cur_byte & 0xf0) == 0x10) {
		*bitfill -= 7;
		return tab3[(cur_byte >> 1) & 7];

	}
	if ((cur_byte & 0xf8) == 8) {
		*bitfill -= 8;
		return tab4[cur_byte & 7];
	}
	return 0xff;
}

static int fun_F(int cur_byte, int *bitfill)
{
	*bitfill -= 5;
	switch (cur_byte & 0xf8) {
	case 0x80:
		return 0;
	case 0x88:
		return -1;
	case 0x90:
		return 1;
	case 0x98:
		return -2;
	case 0xa0:
		return 2;
	case 0xa8:
		return -3;
	case 0xb0:
		return 3;
	case 0xb8:
		return -4;
	case 0xc0:
		return 4;
	case 0xc8:
		return -5;
	case 0xd0:
		return 5;
	case 0xd8:
		return -6;
	case 0xe0:
		return 6;
	case 0xe8:
		return -7;
	case 0xf0:
		return 7;
	case 0xf8:
		return -8;
	}

	*bitfill -= 1;
	switch (cur_byte & 0xfc) {
	case 0x40:
		return 8;
	case 0x44:
		return -9;
	case 0x48:
		return 9;
	case 0x4c:
		return -10;
	case 0x50:
		return 10;
	case 0x54:
		return -11;
	case 0x58:
		return 11;
	case 0x5c:
		return -12;
	case 0x60:
		return 12;
	case 0x64:
		return -13;
	case 0x68:
		return 13;
	case 0x6c:
		return -14;
	case 0x70:
		return 14;
	case 0x74:
		return -15;
	case 0x78:
		return 15;
	case 0x7c:
		return -16;
	}

	*bitfill -= 1;
	switch (cur_byte & 0xfe) {
	case 0x20:
		return 16;
	case 0x22:
		return -17;
	case 0x24:
		return 17;
	case 0x26:
		return -18;
	case 0x28:
		return 18;
	case 0x2a:
		return -19;
	case 0x2c:
		return 19;
	}

	*bitfill += 7;
	return 0xff;
}

static int internal_spca561_decode(int width, int height,
		const unsigned char *inbuf,
		unsigned char *outbuf)
{
	/* buffers */
	static int accum[8 * 8 * 8];
	static int i_hits[8 * 8 * 8];

	static const int nbits_A[] = {
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 7, 7,
		7, 7,
		7, 7, 7, 7, 7,
		7, 7, 7, 7, 7, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
		5, 5,
		5, 5, 5, 5, 5,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	};
	static const int tab_A[] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 11, -11, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
		10, 10,
		255, 254, -4,
		-4, -5, -5, -6, -6, -7, -7, -8, -8, -9, -9, -10, -10, -1,
		-1, -1,
		-1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, -1, -1, -1, -1, -1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3,
		3, 3, 3,
		3, 3, 3,
		-2, -2, -2, -2, -2, -2, -2, -2, -3, -3, -3, -3, -3, -3, -3,
		-3, 1,
		1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1,
		1
	};

	static const int nbits_B[] = {
		0, 8, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	};
	static const int tab_B[] = {
		0xff, -4, 3, 3, -3, -3, -3, -3, 2, 2, 2, 2, 2, 2, 2, 2, -2,
		-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
		1, 1,
		1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0,
	};

	static const int nbits_C[] = {
		0, 0, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
		4, 4,
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	};
	static const int tab_C[] = {
		0xff, 0xfe, 6, -7, 5, 5, -6, -6, 4, 4, 4, 4, -5, -5, -5, -5,
		3, 3, 3, 3, 3, 3, 3, 3, -4, -4, -4, -4, -4, -4, -4, -4, 2,
		2, 2, 2,
		2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, -3, -3, -3, -3, -3, -3, -3, -3,
		-3, -3,
		-3, -3, -3,
		-3, -3, -3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1,
		1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -2, -2, -2, -2, -2, -2, -2,
		-2, -2,
		-2, -2, -2,
		-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
		-2, -2,
		-2, -2,
		-2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1,
	};

	static const int nbits_D[] = {
		0, 0, 0, 0, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6,
		6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5,
		5, 5,
		5, 5, 5, 5, 5,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4,
		4, 4,
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
		4, 4,
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
		4, 4,
		4, 4, 4, 4, 4,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3,
		3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
	};
	static const int tab_D[] = {
		0xff, 0xfe, 0xfd, 0xfc, 10, -11, 11, -12, 8, 8, -9, -9, 9, 9,
		-10, -10, 6, 6, 6, 6, -7, -7, -7, -7, 7, 7, 7, 7, -8, -8,
		-8, -8,
		4, 4, 4, 4,
		4, 4, 4, 4, -5, -5, -5, -5, -5, -5, -5, -5, 5, 5, 5, 5, 5,
		5, 5, 5,
		-6, -6,
		-6, -6, -6, -6, -6, -6, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2,
		2, 2, -3,
		-3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3,
		3, 3,
		3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, -4, -4, -4, -4, -4, -4, -4,
		-4, -4,
		-4, -4, -4,
		-4, -4, -4, -4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1,
		-1, -1,
		-1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1,
		1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, -2, -2, -2, -2, -2, -2, -2, -2,
		-2, -2,
		-2, -2, -2,
		-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
		-2, -2,
		-2, -2
	};

	/* a_curve[19 + i] = ... [-19..19] => [-160..160] */
	static const int a_curve[] = {
		-160, -144, -128, -112, -98, -88, -80, -72, -64, -56, -48,
		-40, -32, -24, -18, -12, -8, -5, -2, 0, 2, 5, 8, 12, 18,
		24, 32,
		40, 48, 56, 64,
		72, 80, 88, 98, 112, 128, 144, 160
	};
	/* clamp0_255[256 + i] = min(max(i,255),0); the index can reach
	 * [-511, 1278] on corrupt input, so the backing table is wider. */
	static unsigned char clamp_tab[2560];
	const unsigned char *clamp0_255 = clamp_tab + 1024 - 256;
	/* abs_clamp15[19 + i] = min(abs(i), 15). The copy this was taken from
	 * has 38 entries, so gkw == 19 read one past the end. */
	static const int abs_clamp15[] = {
		15, 15, 15, 15, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3,
		2, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
		15, 15, 15, 15
	};
	/* diff_encoding[256 + i] = ... */
	static int diff_encoding[512];

	int block;
	int bitfill = 0;
	int xwidth = width + 6;
	int off_up_right = 2 - 2 * xwidth;
	int off_up_left = -2 - 2 * xwidth;
	int pixel_U = 0, saved_pixel_UR = 0;
	int pixel_x = 0, pixel_y = 2;
	unsigned char *output_ptr = outbuf;

	/* These two tables are pure functions; the original source spells them
	 * out as literal arrays, here they are built once instead. */
	if (clamp_tab[sizeof(clamp_tab) - 1] == 0) {
		int i;
		for (i = 0; i < (int)sizeof(clamp_tab); i++) {
			int v = i - 1024;
			clamp_tab[i] = v < 0 ? 0 : v > 255 ? 255 : v;
		}
		for (i = 0; i < 512; i++) {
			int d = i - 256, e;
			if (d <= -21) e = 7;
			else if (d <= -7) e = 5;
			else if (d <= -3) e = 3;
			else if (d <= -1) e = 1;
			else if (d == 0) e = 0;
			else if (d <= 2) e = 2;
			else if (d <= 6) e = 4;
			else e = 6;
			diff_encoding[i] = e;
		}
	}

	memset(i_hits, 0, sizeof(i_hits));
	memset(accum, 0, sizeof(accum));

	memcpy(outbuf + xwidth * 2 + 3, inbuf + 0x14, width);
	memcpy(outbuf + xwidth * 3 + 3, inbuf + 0x14 + width, width);

	input_ptr = inbuf + 0x14 + width * 2;
	output_ptr = outbuf + (xwidth) * 4 + 3;

	bit_bucket = 0;

	for (block = 0; block < ((height - 2) * width) / 32; ++block) {
		int b_it, var_7 = 0;
		int cur_byte;

		/* Garbage can consume ~3 bytes per pixel; stop at the end of the
		 * data (one block reads well under SPCA561_DECODE_PAD bytes). */
		if (input_ptr > input_end)
			return -3;

		refill(&bitfill);

		cur_byte = (bit_bucket >> (bitfill & 7)) & 0xff;

		if ((cur_byte & 0x80) == 0) {
			var_7 = 0;
			bitfill--;
		} else if ((cur_byte & 0xC0) == 0x80) {
			var_7 = 1;
			bitfill -= 2;
		} else if ((cur_byte & 0xc0) == 0xc0) {
			var_7 = 2;
			bitfill -= 2;
		}

		for (b_it = 0; b_it < 32; b_it++) {
			int index;
			int pixel_L, pixel_UR, pixel_UL;
			int multiplier;
			int dL, dC, dR;
			int gkw;	/* God knows what */

			refill(&bitfill);
			cur_byte = bit_bucket >> (bitfill & 7) & 0xff;

			pixel_L = output_ptr[-2];
			pixel_UR = output_ptr[off_up_right];
			pixel_UL = output_ptr[off_up_left];

			dL = diff_encoding[0x100 + pixel_UL - pixel_L];
			dC = diff_encoding[0x100 + pixel_U - pixel_UL];
			dR = diff_encoding[0x100 + pixel_UR - pixel_U];

			if (pixel_x < 2) {
				pixel_L = pixel_UL = pixel_U =
					output_ptr[-xwidth * 2];
				pixel_UR = output_ptr[off_up_right];
				dL = dC = 0;
				dR = diff_encoding[0x100 + pixel_UR -
					pixel_U];
			} else if (pixel_x > width - 3)
				dR = 0;

			multiplier = 4;
			index = dR + dC * 8 + dL * 64;

			if (pixel_L + pixel_U * 2 <= 144
					&& (pixel_y & 1) == 0
					&& (b_it & 3) == 0 && (dR < 5) && (dC < 5)
					&& (dL < 5)) {
				multiplier = 1;
			} else if (pixel_L <= 48
					&& dL <= 4 && dC <= 4 && dL >= 1
					&& dC >= 1) {
				multiplier = 2;
			} else if (var_7 == 1) {
				multiplier = 2;
			} else if (dC + dL >= 11 || var_7 == 2) {
				multiplier = 8;
			}

			if (i_hits[index] < 7) {
				bitfill -= nbits_A[cur_byte];
				gkw = tab_A[cur_byte];
				if (gkw == 0xfe)
					gkw = fun_A(&bitfill);
			} else if (i_hits[index] >= accum[index]) {
				bitfill -= nbits_B[cur_byte];
				gkw = tab_B[cur_byte];
				if (cur_byte == 0)
					gkw = fun_B(&bitfill);
			} else if (i_hits[index] * 2 >= accum[index]) {
				bitfill -= nbits_C[cur_byte];
				gkw = tab_C[cur_byte];
				if (cur_byte < 2)
					gkw = fun_C(&bitfill, gkw);
			} else if (i_hits[index] * 4 >= accum[index]) {
				bitfill -= nbits_D[cur_byte];
				gkw = tab_D[cur_byte];
				if (cur_byte < 4)
					gkw = fun_D(&bitfill, gkw);
			} else if (i_hits[index] * 8 >= accum[index]) {
				gkw = fun_E(cur_byte, &bitfill);
			} else {
				gkw = fun_F(cur_byte, &bitfill);
			}

			if (gkw < -19 || gkw > 19)	/* 0xff or garbage: corrupt */
				return -3;

			{
				int tmp1, tmp2;

				tmp1 =
					(pixel_U + pixel_L) * 3 - pixel_UL * 2;
				tmp1 += (tmp1 < 0) ? 3 : 0;
				tmp2 = a_curve[19 + gkw] * multiplier;
				tmp2 += (tmp2 < 0) ? 1 : 0;

				*(output_ptr++) =
					clamp0_255[0x100 + (tmp1 >> 2) -
					(tmp2 >> 1)];
			}
			pixel_U = saved_pixel_UR;
			saved_pixel_UR = pixel_UR;

			if (++pixel_x == width) {
				output_ptr += 6;
				pixel_x = 0;
				pixel_y++;
			}

			accum[index] += abs_clamp15[19 + gkw];

			if (i_hits[index]++ == 15) {
				i_hits[index] = 8;
				accum[index] /= 2;
			}
		}
	}
	return 0;
}

/* FIXME, change internal_spca561_decode not to need the extra border
   around its dest buffer */
int spca561_decode(const unsigned char *inbuf, size_t inlen,
		unsigned char *outbuf, int width, int height)
{
	int i;
	static unsigned char tmpbuf[650 * 490];

	if (width > 644 || height > 488 || inlen < 0x14 + 2 * (size_t)width)
		return -1;
	input_end = inbuf + inlen;
	if (internal_spca561_decode(width, height, inbuf, tmpbuf) != 0)
		return -1;
	for (i = 0; i < height; i++)
		memcpy(outbuf + i * width,
				tmpbuf + (i + 2) * (width + 6) + 3, width);
	return 0;
}

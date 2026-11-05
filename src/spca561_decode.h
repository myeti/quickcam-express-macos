// SPDX-License-Identifier: LGPL-2.1-or-later
// SPCA561 compressed-Bayer decoder (LGPL-2.1+, from v4l-utils' libv4lconvert).
#pragma once

#include <stddef.h>

// The bit reader runs a little past the data it is given: the input buffer
// must have at least this many readable bytes after inlen.
#define SPCA561_DECODE_PAD 1024

// Decode one compressed frame of inlen bytes, starting at its 20-byte header,
// into an 8-bit GBRG Bayer image of width*height. Not reentrant.
// Returns 0 on success, -1 on a corrupt or truncated stream.
int spca561_decode(const unsigned char *inbuf, size_t inlen, unsigned char *outbuf, int width,
                   int height);

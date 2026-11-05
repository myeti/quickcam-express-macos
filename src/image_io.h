// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
#pragma once
#include <stddef.h>
#include <stdint.h>

// Write an RGBX (4 bytes/pixel) image as PNG using ImageIO. Returns 0 on success.
int qc_write_png(const char *path, const uint8_t *rgbx, int width, int height);

// Encode an RGBX image as JPEG (quality 0-1). Returns a malloc'd buffer, or NULL.
uint8_t *qc_encode_jpeg(const uint8_t *rgbx, int width, int height, float quality, size_t *len);

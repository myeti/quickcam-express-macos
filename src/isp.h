// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// Tiny image pipeline for 8-bit GBRG Bayer frames: statistics, white balance,
// bilinear demosaic and gamma.
#pragma once
#include <stdint.h>

// White balance statistics: average black-subtracted value of each channel
// (out[0..2] = R, G, B) over 2x2 cells that look grey under the current gains
// wb[], skipping clipped cells (any sample >= limit[]) and very dark ones.
// Strongly coloured surfaces (skin, wood) would otherwise bias a plain grey
// world. Falls back to all unclipped cells if too few grey ones are found.
void isp_wb_stats(const uint8_t *bayer, int w, int h, const int limit[3], const float black[3],
                  const float wb[3], double out[3]);

// Value below which the given fraction of green samples lie.
int isp_green_percentile(const uint8_t *bayer, int w, int h, double fraction);

// Fill a 1024-entry table mapping linear 10-bit values to gamma-encoded 8-bit.
void isp_gamma_lut(uint8_t lut[1024], float gamma);

// Demosaic into RGBX (4 bytes/pixel). Each raw sample has black[R/G/B]
// subtracted and is multiplied by gain[R/G/B] into a 10-bit linear value
// (clipped at 1023) before interpolation; scratch must hold w*h uint16_t.
void isp_render(const uint8_t *bayer, int w, int h, const float black[3], const float gain[3],
                const uint8_t lut[1024], uint8_t *rgbx, uint16_t *scratch);

// Smooth the colour (not the brightness) of an RGBX image to remove chroma
// speckle, and scale saturation. scratch must hold 3*w*h int16_t.
void isp_color(uint8_t *rgbx, int w, int h, float saturation, int16_t *scratch);

// Mean luma (0-255) of an RGBX image, centre-weighted.
int isp_mean_luma(const uint8_t *rgbx, int w, int h);

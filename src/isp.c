// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
#include "isp.h"

#include <math.h>

enum { R, G, B };

// GBRG: even rows are G B G B ..., odd rows are R G R G ...
static inline int color_at(int x, int y) {
    if (y & 1) return (x & 1) ? G : R;
    return (x & 1) ? B : G;
}

void isp_wb_stats(const uint8_t *bayer, int w, int h, const int limit[3], const float black[3],
                  const float wb[3], double out[3]) {
    double grey[3] = {0, 0, 0}, all[3] = {0, 0, 0};
    uint32_t ngrey = 0, nall = 0;
    for (int y = 0; y + 1 < h; y += 4) {
        const uint8_t *row0 = bayer + y * w, *row1 = row0 + w;
        for (int x = 0; x + 1 < w; x += 4) {
            int g0 = row0[x], bb = row0[x + 1], rr = row1[x], g1 = row1[x + 1];
            if (g0 >= limit[G] || g1 >= limit[G] || bb >= limit[B] || rr >= limit[R]) continue;
            double v[3] = {rr - black[R], (g0 + g1) / 2.0 - black[G], bb - black[B]};
            if (v[R] < 1 || v[G] < 1 || v[B] < 1) continue;
            for (int c = 0; c < 3; c++) all[c] += v[c];
            nall++;
            // Grey under the current gains: no channel more than 35% off another.
            double r = v[R] * wb[R], g = v[G], b = v[B] * wb[B];
            double lo = fmin(r, fmin(g, b)), hi = fmax(r, fmax(g, b));
            if (v[G] >= 6 && hi < 1.35 * lo) {
                for (int c = 0; c < 3; c++) grey[c] += v[c];
                ngrey++;
            }
        }
    }
    double *src = ngrey * 20 >= nall && ngrey ? grey : all;
    uint32_t n = src == grey ? ngrey : nall;
    for (int c = 0; c < 3; c++) out[c] = n ? src[c] / n : 0;
}

int isp_green_percentile(const uint8_t *bayer, int w, int h, double fraction) {
    uint32_t hist[256] = {0}, n = 0;
    for (int y = 0; y < h; y += 2)  // G samples sit at even x on even rows
        for (int x = 0; x < w; x += 4) {
            hist[bayer[y * w + x]]++;
            n++;
        }
    uint32_t want = (uint32_t)(fraction * n), acc = 0;
    for (int v = 0; v < 256; v++)
        if ((acc += hist[v]) > want) return v;
    return 255;
}

void isp_gamma_lut(uint8_t lut[1024], float gamma) {
    for (int i = 0; i < 1024; i++) lut[i] = (uint8_t)lrintf(255.0f * powf(i / 1023.0f, 1.0f / gamma));
}

static inline int reflect(int v, int n) { return v < 0 ? -v : v >= n ? 2 * n - 2 - v : v; }

void isp_render(const uint8_t *bayer, int w, int h, const float black[3], const float gain[3],
                const uint8_t lut[1024], uint8_t *rgbx, uint16_t *lin) {
    // Linear 10-bit plane with black point, white balance and white point
    // applied per sample, through a per-channel table.
    uint16_t map[3][256];
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) {
            float f = (v - black[c]) * gain[c];
            map[c][v] = f <= 0 ? 0 : f >= 1023 ? 1023 : (uint16_t)f;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) lin[y * w + x] = map[color_at(x, y)][bayer[y * w + x]];

    // Bilinear demosaic: each missing colour is the mean of the same-colour
    // samples in the 3x3 neighbourhood. Reflection at the borders keeps the
    // Bayer phase intact.
    for (int y = 0; y < h; y++) {
        int ys[3] = {reflect(y - 1, h), y, reflect(y + 1, h)};
        for (int x = 0; x < w; x++) {
            int xs[3] = {reflect(x - 1, w), x, reflect(x + 1, w)};
            int own = color_at(x, y);
            uint32_t sum[3] = {0, 0, 0}, cnt[3] = {0, 0, 0};
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++) {
                    int c = color_at(x + i - 1, y + j - 1);
                    if (c == own) continue;
                    sum[c] += lin[ys[j] * w + xs[i]];
                    cnt[c]++;
                }
            sum[own] = lin[y * w + x];
            cnt[own] = 1;
            uint8_t *o = rgbx + 4 * (y * w + x);
            o[0] = lut[sum[R] / cnt[R]];
            o[1] = lut[sum[G] / cnt[G]];
            o[2] = lut[sum[B] / cnt[B]];
            o[3] = 255;
        }
    }
}

int isp_mean_luma(const uint8_t *rgbx, int w, int h) {
    uint64_t s = 0, n = 0;
    for (int y = 2; y < h; y += 4)
        for (int x = 2; x < w; x += 4) {
            const uint8_t *p = rgbx + 4 * (y * w + x);
            int wgt = (x > w / 4 && x < 3 * w / 4 && y > h / 4 && y < 3 * h / 4) ? 2 : 1;
            s += (uint64_t)wgt * ((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            n += wgt;
        }
    return n ? (int)(s / n) : 0;
}

// 5-tap box blur of a plane, horizontally then vertically (via tmp).
static void box5(int16_t *p, int16_t *tmp, int w, int h) {
    for (int y = 0; y < h; y++) {
        const int16_t *row = p + y * w;
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = -2; k <= 2; k++) s += row[reflect(x + k, w)];
            tmp[y * w + x] = (int16_t)(s / 5);
        }
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = -2; k <= 2; k++) s += tmp[reflect(y + k, h) * w + x];
            p[y * w + x] = (int16_t)(s / 5);
        }
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

void isp_color(uint8_t *rgbx, int w, int h, float saturation, int16_t *scratch) {
    int n = w * h;
    int16_t *cb = scratch, *cr = scratch + n, *tmp = scratch + 2 * n;
    // Colour differences against luma; the eye is far less sensitive to their resolution.
    for (int i = 0; i < n; i++) {
        const uint8_t *p = rgbx + 4 * i;
        int y = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
        cr[i] = (int16_t)(p[0] - y);
        cb[i] = (int16_t)(p[2] - y);
    }
    box5(cb, tmp, w, h);
    box5(cr, tmp, w, h);
    int s = (int)(saturation * 256);
    for (int i = 0; i < n; i++) {
        uint8_t *p = rgbx + 4 * i;
        int y = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
        int r = (cr[i] * s) >> 8, b = (cb[i] * s) >> 8;
        p[0] = clamp8(y + r);
        p[1] = clamp8(y - (77 * r + 29 * b) / 150);
        p[2] = clamp8(y + b);
    }
}

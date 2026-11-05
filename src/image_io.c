// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
#include "image_io.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdlib.h>
#include <string.h>

static CGImageRef make_image(const uint8_t *rgbx, int width, int height) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, rgbx, (size_t)width * height * 4, NULL);
    CGImageRef img = CGImageCreate(width, height, 8, 32, (size_t)width * 4, cs,
                                   kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return img;
}

int qc_write_png(const char *path, const uint8_t *rgbx, int width, int height) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CFRelease(url);
    if (!dst) return -1;
    CGImageRef img = make_image(rgbx, width, height);
    CGImageDestinationAddImage(dst, img, NULL);
    bool ok = CGImageDestinationFinalize(dst);
    CGImageRelease(img);
    CFRelease(dst);
    return ok ? 0 : -1;
}

uint8_t *qc_encode_jpeg(const uint8_t *rgbx, int width, int height, float quality, size_t *len) {
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, NULL);
    uint8_t *out = NULL;
    if (dst) {
        CFNumberRef q = CFNumberCreate(NULL, kCFNumberFloatType, &quality);
        const void *keys[] = {kCGImageDestinationLossyCompressionQuality};
        const void *vals[] = {q};
        CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
        CGImageRef img = make_image(rgbx, width, height);
        CGImageDestinationAddImage(dst, img, opts);
        if (CGImageDestinationFinalize(dst)) {
            *len = (size_t)CFDataGetLength(data);
            out = malloc(*len);
            if (out) memcpy(out, CFDataGetBytePtr(data), *len);
        }
        CGImageRelease(img);
        CFRelease(opts);
        CFRelease(q);
        CFRelease(dst);
    }
    CFRelease(data);
    return out;
}

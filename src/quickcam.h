// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// Userspace driver for the Logitech QuickCam Express 046d:0928 (Sunplus SPCA561A rev 012a).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define QC_EXPOSURE_MAX (2047 + 325)
#define QC_GAIN_MAX 255

typedef struct qc_cam qc_cam;

typedef struct {
    int width, height;
    const uint8_t *rgbx;  // width*height*4 bytes (R,G,B,255); valid only during the callback
    const uint8_t *bayer; // the raw sensor frame, width*height bytes in GBRG order
    uint64_t sequence;
    int compressed;       // frame was sent in the SPCA561 compressed format
    int button;           // the camera's snapshot button was just pressed
} qc_frame;

// Called on the USB thread. f == NULL means the stream died (camera unplugged).
typedef void (*qc_frame_cb)(void *ctx, const qc_frame *f);

typedef struct {
    int exposure;            // 1 .. QC_EXPOSURE_MAX (higher values lower the frame rate)
    int gain;                // 0 .. QC_GAIN_MAX
    int auto_exposure;       // software AE drives exposure and gain
    int auto_white_balance;  // software grey-world white balance
    float gamma;             // display gamma applied to the linear sensor data
    float saturation;        // colour saturation, 1.0 = as measured
    int brightness;          // -128 .. 127, offset from the auto-calibrated black level
    // read-only status
    float gain_r, gain_b;    // current white balance gains
    int mean_luma;           // brightness of the last frame
    uint64_t frames, dropped;
} qc_controls;

// Open the camera and run the chip init sequence.
qc_cam *qc_open(char *err, size_t errlen);
void qc_close(qc_cam *c);

// Supported sizes: 352x288 and 320x240 (compressed), 176x144 and 160x120 (raw).
int qc_start(qc_cam *c, int width, int height, qc_frame_cb cb, void *ctx, char *err, size_t errlen);
void qc_stop(qc_cam *c);

void qc_get_controls(qc_cam *c, qc_controls *out);
void qc_set_exposure(qc_cam *c, int exposure);  // also disables auto exposure
void qc_set_gain(qc_cam *c, int gain);          // also disables auto exposure
void qc_set_auto_exposure(qc_cam *c, int on);
void qc_set_auto_white_balance(qc_cam *c, int on);
void qc_set_gamma(qc_cam *c, float gamma);
void qc_set_saturation(qc_cam *c, float saturation);  // 0 (grey) .. 3
void qc_set_brightness(qc_cam *c, int brightness);  // -128..127, relative to true black

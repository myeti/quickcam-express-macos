// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// Minimal userspace USB access on macOS (IOKit IOUSBLib): device open,
// control transfers and isochronous IN streaming on a dedicated thread.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct qc_usb qc_usb;

// Special `len` values passed to the packet callback.
#define QCUSB_PKT_ERROR    (-1)  // a packet was lost or corrupted
#define QCUSB_STREAM_ENDED (-2)  // streaming stopped unexpectedly (e.g. unplugged)

// Called on the USB thread for every isochronous packet received (len may be 0).
typedef void (*qc_packet_cb)(void *ctx, const uint8_t *data, int len);

// Open the first device matching vid/pid, select configuration 1 and claim interface 0.
qc_usb *qcusb_open(uint16_t vid, uint16_t pid, char *err, size_t errlen);
void qcusb_close(qc_usb *u);

// Control transfer on endpoint 0. Returns bytes transferred or -1.
int qcusb_ctrl(qc_usb *u, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index,
               void *data, uint16_t len);

// Select an alternate setting of interface 0 and look up its isochronous IN pipe.
int qcusb_set_alt(qc_usb *u, uint8_t alt);
int qcusb_max_packet(qc_usb *u);

// Max isochronous packet size of each alternate setting of interface 0 (sizes[alt]).
// Returns the number of alternate settings found, or -1.
int qcusb_alt_packet_sizes(qc_usb *u, int *sizes, int max);

// Start/stop isochronous streaming. Packets are delivered to cb on a private thread.
int qcusb_stream_start(qc_usb *u, qc_packet_cb cb, void *ctx);
void qcusb_stream_stop(qc_usb *u);

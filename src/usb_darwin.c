// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// Userspace USB transport for macOS built on IOKit's IOUSBLib plug-in interfaces.
// This is the same mechanism libusb uses on Darwin; no kernel extension is needed
// because nothing in the OS claims a vendor-class (0xff) device.
#include "usb_darwin.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NXFERS  8   // isochronous transfers kept in flight
#define NFRAMES 16  // 1 ms bus frames per transfer -> 128 ms of buffering

typedef IOUSBDeviceInterface650 **DevRef;
typedef IOUSBInterfaceInterface700 **IntfRef;

struct xfer {
    qc_usb *u;
    uint8_t *buf;
    IOUSBIsocFrame frames[NFRAMES];
};

struct qc_usb {
    DevRef dev;
    IntfRef intf;
    CFRunLoopSourceRef async_src;
    uint8_t pipe;
    uint16_t maxpkt;

    // Streaming state. Everything below except `stopping` is only touched on the stream thread.
    pthread_t thread;
    int have_thread;
    CFRunLoopRef runloop;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    volatile int stopping;
    int inflight;
    UInt64 next_frame;
    struct xfer xfers[NXFERS];
    qc_packet_cb cb;
    void *ctx;
};

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static void *get_plugin(io_service_t svc, CFUUIDRef type, CFUUIDRef iid) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    void *obj = NULL;
    if (IOCreatePlugInInterfaceForService(svc, type, kIOCFPlugInInterfaceID, &plug, &score) || !plug)
        return NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(iid), &obj);
    IODestroyPlugInInterface(plug);
    return obj;
}

qc_usb *qcusb_open(uint16_t vid, uint16_t pid, char *err, size_t errlen) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    SInt32 v = vid, p = pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), np);
    CFRelease(nv);
    CFRelease(np);
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, match);  // consumes match
    if (!svc) {
        seterr(err, errlen, "no USB device %04x:%04x found", vid, pid);
        return NULL;
    }

    qc_usb *u = calloc(1, sizeof *u);
    pthread_mutex_init(&u->lock, NULL);
    pthread_cond_init(&u->ready, NULL);
    u->dev = get_plugin(svc, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID650);
    IOObjectRelease(svc);
    if (!u->dev) {
        seterr(err, errlen, "cannot create IOUSBLib device interface");
        goto fail;
    }

    IOReturn kr = (*u->dev)->USBDeviceOpen(u->dev);
    if (kr == kIOReturnExclusiveAccess) kr = (*u->dev)->USBDeviceOpenSeize(u->dev);
    if (kr) {
        seterr(err, errlen, "cannot open device (0x%08x); is another program using the camera?", kr);
        (*u->dev)->Release(u->dev);
        u->dev = NULL;
        goto fail;
    }

    UInt8 cfg = 0;
    (*u->dev)->GetConfiguration(u->dev, &cfg);
    if (cfg != 1 && (kr = (*u->dev)->SetConfiguration(u->dev, 1))) {
        seterr(err, errlen, "SetConfiguration(1) failed (0x%08x)", kr);
        goto fail;
    }

    // The interface nub is published asynchronously after SetConfiguration.
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare};
    for (int tries = 0; tries < 40 && !u->intf; tries++) {
        io_iterator_t it = 0;
        if ((*u->dev)->CreateInterfaceIterator(u->dev, &req, &it) == kIOReturnSuccess) {
            io_service_t is = IOIteratorNext(it);
            IOObjectRelease(it);
            if (is) {
                u->intf = get_plugin(is, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID700);
                IOObjectRelease(is);
                break;
            }
        }
        usleep(50000);
    }
    if (!u->intf) {
        seterr(err, errlen, "cannot find USB interface 0");
        goto fail;
    }
    kr = (*u->intf)->USBInterfaceOpen(u->intf);
    if (kr == kIOReturnExclusiveAccess) kr = (*u->intf)->USBInterfaceOpenSeize(u->intf);
    if (kr) {
        seterr(err, errlen, "cannot open USB interface (0x%08x)", kr);
        (*u->intf)->Release(u->intf);
        u->intf = NULL;
        goto fail;
    }
    if ((kr = (*u->intf)->CreateInterfaceAsyncEventSource(u->intf, &u->async_src))) {
        seterr(err, errlen, "cannot create async event source (0x%08x)", kr);
        goto fail;
    }
    return u;

fail:
    qcusb_close(u);
    return NULL;
}

void qcusb_close(qc_usb *u) {
    if (!u) return;
    qcusb_stream_stop(u);
    if (u->async_src) CFRelease(u->async_src);
    if (u->intf) {
        (*u->intf)->SetAlternateInterface(u->intf, 0);
        (*u->intf)->USBInterfaceClose(u->intf);
        (*u->intf)->Release(u->intf);
    }
    if (u->dev) {
        (*u->dev)->USBDeviceClose(u->dev);
        (*u->dev)->Release(u->dev);
    }
    pthread_mutex_destroy(&u->lock);
    pthread_cond_destroy(&u->ready);
    free(u);
}

int qcusb_ctrl(qc_usb *u, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index,
               void *data, uint16_t len) {
    IOUSBDevRequestTO r = {
        .bmRequestType = reqtype,
        .bRequest = req,
        .wValue = value,
        .wIndex = index,
        .wLength = len,
        .pData = data,
        .noDataTimeout = 500,
        .completionTimeout = 500,
    };
    IOReturn kr = (*u->dev)->DeviceRequestTO(u->dev, &r);
    return kr ? -1 : (int)r.wLenDone;
}

int qcusb_set_alt(qc_usb *u, uint8_t alt) {
    if ((*u->intf)->SetAlternateInterface(u->intf, alt)) return -1;
    UInt8 n = 0;
    (*u->intf)->GetNumEndpoints(u->intf, &n);
    for (UInt8 i = 1; i <= n; i++) {
        UInt8 dir, num, type, interval;
        UInt16 mps;
        if ((*u->intf)->GetPipeProperties(u->intf, i, &dir, &num, &type, &mps, &interval)) continue;
        if (dir == kUSBIn && type == kUSBIsoc) {
            u->pipe = i;
            u->maxpkt = mps;
            return 0;
        }
    }
    u->pipe = 0;
    u->maxpkt = 0;
    return alt == 0 ? 0 : -1;
}

int qcusb_max_packet(qc_usb *u) { return u->maxpkt; }

int qcusb_alt_packet_sizes(qc_usb *u, int *sizes, int max) {
    IOUSBConfigurationDescriptorPtr cd;
    if ((*u->dev)->GetConfigurationDescriptorPtr(u->dev, 0, &cd)) return -1;
    const uint8_t *p = (const uint8_t *)cd, *end = p + USBToHostWord(cd->wTotalLength);
    int alt = -1, n = 0;
    for (; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
        if (p[1] == kUSBInterfaceDesc && p[0] >= 9) {
            alt = p[2] == 0 && p[3] < max ? p[3] : -1;
            if (alt >= 0) {
                sizes[alt] = 0;
                if (alt + 1 > n) n = alt + 1;
            }
        } else if (p[1] == kUSBEndpointDesc && p[0] >= 7 && alt >= 0 && (p[3] & 3) == kUSBIsoc &&
                   (p[2] & 0x80)) {
            sizes[alt] = (p[4] | (p[5] << 8)) & 0x7ff;
        }
    }
    return n;
}

static void resync(qc_usb *u) {
    UInt64 frame = 0;
    AbsoluteTime t;
    (*u->intf)->GetBusFrameNumber(u->intf, &frame, &t);
    u->next_frame = frame + 10;
}

static void iso_done(void *refcon, IOReturn result, void *arg0);

static void submit(struct xfer *x) {
    qc_usb *u = x->u;
    for (int attempt = 0; attempt < 3; attempt++) {
        for (int i = 0; i < NFRAMES; i++) {
            x->frames[i].frReqCount = u->maxpkt;
            x->frames[i].frActCount = 0;
            x->frames[i].frStatus = 0;
        }
        IOReturn kr = (*u->intf)->ReadIsochPipeAsync(u->intf, u->pipe, x->buf, u->next_frame, NFRAMES,
                                                     x->frames, iso_done, x);
        if (kr == kIOReturnSuccess) {
            u->next_frame += NFRAMES;
            u->inflight++;
            return;
        }
        if (kr != kIOReturnIsoTooOld && kr != kIOReturnIsoTooNew) {
            fprintf(stderr, "qcusb: isochronous submit failed (0x%08x)\n", kr);
            return;
        }
        resync(u);
    }
}

static void iso_done(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    struct xfer *x = refcon;
    qc_usb *u = x->u;
    u->inflight--;

    if (result == kIOReturnSuccess || result == kIOReturnUnderrun || result == kIOReturnOverrun) {
        for (int i = 0; i < NFRAMES; i++) {
            IOUSBIsocFrame *f = &x->frames[i];
            if (f->frStatus && f->frStatus != kIOReturnUnderrun)
                u->cb(u->ctx, NULL, QCUSB_PKT_ERROR);
            else
                u->cb(u->ctx, x->buf + (size_t)i * u->maxpkt, f->frActCount);
        }
        if (!u->stopping) submit(x);
    } else if (result != kIOReturnAborted && !u->stopping) {
        // Transient error (missed schedule etc.): drop this chunk and keep going.
        u->cb(u->ctx, NULL, QCUSB_PKT_ERROR);
        if (result == kIOReturnIsoTooOld) resync(u);
        if (result != kIOReturnNotResponding && result != kIOReturnNoDevice) submit(x);
    }
    if (u->inflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
}

static void *stream_thread(void *arg) {
    qc_usb *u = arg;
    pthread_setname_np("quickcam.usb");
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    CFRunLoopAddSource(rl, u->async_src, kCFRunLoopDefaultMode);

    pthread_mutex_lock(&u->lock);
    u->runloop = (CFRunLoopRef)CFRetain(rl);
    pthread_cond_signal(&u->ready);
    pthread_mutex_unlock(&u->lock);

    resync(u);
    for (int i = 0; i < NXFERS; i++) submit(&u->xfers[i]);
    if (u->inflight) CFRunLoopRun();

    if (!u->stopping) u->cb(u->ctx, NULL, QCUSB_STREAM_ENDED);
    CFRunLoopRemoveSource(rl, u->async_src, kCFRunLoopDefaultMode);
    return NULL;
}

int qcusb_stream_start(qc_usb *u, qc_packet_cb cb, void *ctx) {
    if (u->have_thread || !u->pipe || !u->maxpkt) return -1;
    u->cb = cb;
    u->ctx = ctx;
    u->stopping = 0;
    u->inflight = 0;
    u->runloop = NULL;
    int ok = 1;
    for (int i = 0; i < NXFERS; i++) {
        u->xfers[i].u = u;
        ok &= (u->xfers[i].buf = calloc(NFRAMES, u->maxpkt)) != NULL;
    }
    if (!ok || pthread_create(&u->thread, NULL, stream_thread, u)) {
        for (int i = 0; i < NXFERS; i++) {
            free(u->xfers[i].buf);
            u->xfers[i].buf = NULL;
        }
        return -1;
    }
    u->have_thread = 1;
    pthread_mutex_lock(&u->lock);
    while (!u->runloop) pthread_cond_wait(&u->ready, &u->lock);
    pthread_mutex_unlock(&u->lock);
    return 0;
}

void qcusb_stream_stop(qc_usb *u) {
    if (!u->have_thread) return;
    u->stopping = 1;
    IntfRef intf = u->intf;
    uint8_t pipe = u->pipe;
    CFRunLoopPerformBlock(u->runloop, kCFRunLoopDefaultMode, ^{ (*intf)->AbortPipe(intf, pipe); });
    CFRunLoopWakeUp(u->runloop);
    pthread_join(u->thread, NULL);
    u->have_thread = 0;
    CFRelease(u->runloop);
    u->runloop = NULL;
    for (int i = 0; i < NXFERS; i++) {
        free(u->xfers[i].buf);
        u->xfers[i].buf = NULL;
    }
}

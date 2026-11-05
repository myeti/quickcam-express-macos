// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// Logitech QuickCam Express (046d:0928): Sunplus SPCA561A bridge, chip revision 012a.
//
// The register sequences, control encodings and packet format are ported from
// the Linux gspca spca561 subdriver (drivers/media/usb/gspca/spca561.c,
// GPL-2.0-or-later, Copyright (C) 2004 Michel Xhaard, V4L2 by Jean-Francois Moine).
// Auto exposure and white balance are done in software here, as libv4l does on Linux.
#include "quickcam.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isp.h"
#include "spca561_decode.h"
#include "usb_darwin.h"

#define QC_VID 0x046d
#define QC_PID 0x0928

#define FBUF_SIZE (352 * 288 + 64 * 1024)  // largest raw frame plus slack
#define FBUF_PAD SPCA561_DECODE_PAD         // zeroed tail for the decoder's bit reader
#define HDR_LEN 20                         // frame header length (rev 012a)

static const struct {
    int w, h, priv;  // priv: value for register 0x8500 (|0x10 enables compression)
} modes[] = {
    {160, 120, 3},
    {176, 144, 2},
    {320, 240, 1},
    {352, 288, 0},
};

struct qc_cam {
    qc_usb *usb;
    pthread_mutex_t lock;  // guards register I/O and the controls

    // Streaming
    int width, height, priv, streaming;
    qc_frame_cb cb;
    void *cb_ctx;

    // Frame assembly and processing (USB thread only)
    uint8_t *fbuf;
    size_t fill;
    int active, discard, compressed, button_down, button_edge;
    uint8_t *bayer, *rgbx;
    uint16_t *scratch;
    uint8_t lut[1024];
    float lut_gamma;
    float wb_r, wb_b, sw_black;
    int black;  // pedestal currently removed in hardware (raw levels)
    int ae_wait;
    double ae_total;

    qc_controls ctl;
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- register access (spca561.c: reg_w_val, reg_r, reg_w_buf) ---- */

static int reg_w_val(qc_cam *c, uint16_t index, uint8_t value) {
    return qcusb_ctrl(c->usb, 0x40, 0, value, index, NULL, 0) < 0 ? -1 : 0;
}

static int reg_r(qc_cam *c, uint16_t index, uint8_t *buf, uint16_t len) {
    return qcusb_ctrl(c->usb, 0xc0, 0, 0, index, buf, len) == len ? 0 : -1;
}

static int reg_w_buf(qc_cam *c, uint16_t index, const uint8_t *data, uint16_t len) {
    uint8_t buf[8];
    memcpy(buf, data, len);
    return qcusb_ctrl(c->usb, 0x40, 0, 0, index, buf, len) < 0 ? -1 : 0;
}

/* ---- rev 012a init tables (spca561.c, "QC Express etch2 stuff") ---- */

static const uint16_t Pb100_1map8300[][2] = {
    /* reg, value */
    {0x8320, 0x3304},

    {0x8303, 0x0125}, /* image area */
    {0x8304, 0x0169},
    {0x8328, 0x000b},
    {0x833c, 0x0001}, /*fixme: win:07*/

    {0x832f, 0x1904}, /*fixme: was 0419*/
    {0x8307, 0x00aa},
    {0x8301, 0x0003},
    {0x8302, 0x000e},
    {0, 0},
};
static const uint16_t Pb100_2map8300[][2] = {
    /* reg, value */
    {0x8339, 0x0000},
    {0x8307, 0x00aa},
    {0, 0},
};

static const uint16_t spca561_161rev12A_data1[][2] = {
    /* value, reg */
    {0x29, 0x8118}, /* Control register (various enable bits) */
    {0x08, 0x8114}, /* GPIO: Led off */
    {0x0e, 0x8112}, /* 0x0e stream off 0x3e stream on */
    {0x00, 0x8102}, /* white balance - new */
    {0x92, 0x8804},
    {0x04, 0x8802}, /* windows uses 08 */
    {0, 0},
};
static const uint16_t spca561_161rev12A_data2[][2] = {
    /* value, reg */
    {0x21, 0x8118},
    {0x10, 0x8500},
    {0x07, 0x8601},
    {0x07, 0x8602},
    {0x04, 0x8501},

    {0x07, 0x8201}, /* windows uses 02 */
    {0x08, 0x8200},
    {0x01, 0x8200},

    {0x90, 0x8604},
    {0x00, 0x8605},
    {0xb0, 0x8603},

    /* sensor gains */
    {0x07, 0x8601}, /* white balance - new */
    {0x07, 0x8602}, /* white balance - new */
    {0x00, 0x8610}, /* *red */
    {0x00, 0x8611}, /* 3f   *green */
    {0x00, 0x8612}, /* green *blue */
    {0x00, 0x8613}, /* blue *green */
    {0x43, 0x8614}, /* green *red - white balance - was 0x35 */
    {0x40, 0x8615}, /* 40   *green - white balance - was 0x35 */
    {0x71, 0x8616}, /* 7a   *blue - white balance - was 0x35 */
    {0x40, 0x8617}, /* 40   *green - white balance - was 0x35 */

    {0x0c, 0x8620}, /* 0c */
    {0xc8, 0x8631}, /* c8 */
    {0xc8, 0x8634}, /* c8 */
    {0x23, 0x8635}, /* 23 */
    {0x1f, 0x8636}, /* 1f */
    {0xdd, 0x8637}, /* dd */
    {0xe1, 0x8638}, /* e1 */
    {0x1d, 0x8639}, /* 1d */
    {0x21, 0x863a}, /* 21 */
    {0xe3, 0x863b}, /* e3 */
    {0xdf, 0x863c}, /* df */
    {0xf0, 0x8505},
    {0x32, 0x850a},
    {0x29, 0x8118},
    {0, 0},
};

static void write_vector(qc_cam *c, const uint16_t (*data)[2]) {
    for (; (*data)[1]; data++) reg_w_val(c, (*data)[1], (uint8_t)(*data)[0]);
}

static void sensor_mapwrite(qc_cam *c, const uint16_t (*map)[2]) {
    for (; (*map)[0]; map++) {
        uint8_t buf[2] = {(uint8_t)(*map)[1], (uint8_t)((*map)[1] >> 8)};
        reg_w_buf(c, (*map)[0], buf, 2);
    }
}

static void init_161rev12A(qc_cam *c) {
    write_vector(c, spca561_161rev12A_data1);
    sensor_mapwrite(c, Pb100_1map8300);
    write_vector(c, spca561_161rev12A_data2);
    sensor_mapwrite(c, Pb100_2map8300);
}

static void sd_start_12a(qc_cam *c) {
    static const uint8_t Reg8391[8] = {0x92, 0x30, 0x20, 0x00, 0x0c, 0x00, 0x00, 0x00};

    // Compression on 320x240 and above; raw Bayer below (enough bandwidth at ~20 fps).
    reg_w_val(c, 0x8500, c->priv <= 1 ? 0x10 | c->priv : c->priv);
    reg_w_buf(c, 0x8307, (const uint8_t[]){0xaa, 0x00}, 2);
    reg_w_val(c, 0x8700, 0x8a);  // clock - lower 0x8X values lead to fps > 30
    reg_w_val(c, 0x8112, 0x1e | 0x20);
    reg_w_val(c, 0x850b, 0x03);
    reg_w_buf(c, 0x8391, Reg8391, 8);
    reg_w_buf(c, 0x8390, Reg8391, 8);
    reg_w_val(c, 0x8114, 0x00);  // LED on
}

static void sd_stopN(qc_cam *c) {
    reg_w_val(c, 0x8112, 0x0e);
    reg_w_val(c, 0x8114, 0x08);  // LED off
}

/* ---- controls (spca561.c: setwhite, setbrightness, setexposure, setgain) ---- */

// Hardware white balance, applied after the black level offset. Green's gain
// (0x8615/0x8617) is 0x40, i.e. 1.0. Linux's default "hue" of 0x40 gives
// red 0x38 and blue 0x68.
#define HW_WHITE 0x40
#define HW_GAIN_R ((0x20 + HW_WHITE * 3 / 8) / 64.0)
#define HW_GAIN_B ((0x90 - HW_WHITE * 5 / 8) / 64.0)

static void setwhite(qc_cam *c, int white) {
    reg_w_val(c, 0x8614, 0x20 + white * 3 / 8);  // red
    reg_w_val(c, 0x8616, 0x90 - white * 5 / 8);  // blue
}

static void setbrightness(qc_cam *c, int val) {
    for (int i = 0; i < 4; i++) reg_w_val(c, 0x8610 + i, (uint8_t)val);  // R, Gr, B, Gb
}

// The sensor output sits on a pedestal that is amplified by the analog gain
// (measured: ~0.58 * gain, from 11 at gain 16 to 143 at gain 255). Registers
// 0x8610-0x8613 add a signed offset after the analog gain but before the white
// balance gains, so one value cancels it for all channels. A few levels are left
// so that shadow noise is not clipped. The user brightness is relative to that.
static void update_black_level(qc_cam *c) {
    int pedestal = (int)lround(0.58 * c->ctl.gain) - 3;
    int offset = clampi(c->ctl.brightness - (pedestal > 0 ? pedestal : 0), -128, 127);
    setbrightness(c, offset);
    c->black = offset < 0 ? -offset : 0;
}

// Register 0x8309: bits 0-10 exposure, bits 11-12 a frame-rate divider
// (fps = 60 / (divider + 2)). The divider is raised as soon as possible because
// long exposures without it, combined with high gain, stall the camera.
static void setexposure(qc_cam *c, int val) {
    static const int table[] = {0, 450, 550, 625, QC_EXPOSURE_MAX};
    int expo = 0;
    for (int i = 0; i < 4; i++) {
        if (val <= table[i + 1]) {
            expo = val - table[i];
            if (i) expo += 300;
            expo |= i << 11;
            break;
        }
    }
    reg_w_buf(c, 0x8309, (const uint8_t[]){(uint8_t)expo, (uint8_t)(expo >> 8)}, 2);
}

// Gain register: low 6 bits 0-63, bits 6 and 7 each double the sensitivity.
static void setgain(qc_cam *c, int val) {
    uint8_t g = val < 64 ? val : val < 128 ? (val / 2) | 0x40 : (val / 4) | 0xc0;
    reg_w_buf(c, 0x8335, (const uint8_t[]){g, 0}, 2);
}

/* ---- software auto exposure ----
 * A single "total exposure" figure (exposure * gain / 63) is spread over
 * exposure time and gain along a fixed path that favours frame rate: exposure
 * up to 450 (the longest that keeps 30 fps), gain up to 2x, exposure down to
 * 15 fps, full gain, and only then the slow, very long exposures. */

#define AE_BASE_GAIN 63

static const int ae_path[][2] = {
    /* exposure, gain */
    {1, 16}, {1, AE_BASE_GAIN}, {450, AE_BASE_GAIN}, {450, 127},
    {625, 127}, {625, QC_GAIN_MAX}, {QC_EXPOSURE_MAX, QC_GAIN_MAX},
};
#define AE_PATH_LEN ((int)(sizeof ae_path / sizeof ae_path[0]))

static double ae_total_from(int expo, int gain) { return expo * (gain / (double)AE_BASE_GAIN); }

static void ae_split(double total, int *expo, int *gain) {
    double e = ae_path[0][0], g = ae_path[0][1];
    for (int i = 0; i + 1 < AE_PATH_LEN; i++) {
        if (total > ae_total_from(ae_path[i + 1][0], ae_path[i + 1][1]) && i + 2 < AE_PATH_LEN) continue;
        if (ae_path[i][0] == ae_path[i + 1][0]) {  // exposure fixed, solve for gain
            e = ae_path[i][0];
            g = total * AE_BASE_GAIN / e;
        } else {  // gain fixed, solve for exposure
            g = ae_path[i][1];
            e = total * AE_BASE_GAIN / g;
        }
        break;
    }
    *expo = clampi((int)lround(e), 1, QC_EXPOSURE_MAX);
    *gain = clampi((int)lround(g), 0, QC_GAIN_MAX);
}

static void auto_exposure(qc_cam *c, int luma) {
    const int target = 118, deadband = 12;
    if (c->ae_wait > 0) {  // let the previous change reach the sensor
        c->ae_wait--;
        return;
    }
    if (abs(luma - target) <= deadband) return;
    double f = (double)target / (luma < 8 ? 8 : luma);
    f = pow(f < 0.5 ? 0.5 : f > 2.0 ? 2.0 : f, 0.7);
    double lo = ae_total_from(ae_path[0][0], ae_path[0][1]);
    double hi = ae_total_from(ae_path[AE_PATH_LEN - 1][0], ae_path[AE_PATH_LEN - 1][1]);

    pthread_mutex_lock(&c->lock);  // ae_total is also reset by qc_set_auto_exposure
    c->ae_total *= f;
    c->ae_total = c->ae_total < lo ? lo : c->ae_total > hi ? hi : c->ae_total;
    int expo, gain;
    ae_split(c->ae_total, &expo, &gain);
    if (c->ctl.auto_exposure) {
        if (expo != c->ctl.exposure) setexposure(c, c->ctl.exposure = expo);
        if (gain != c->ctl.gain) {
            setgain(c, c->ctl.gain = gain);
            update_black_level(c);
        }
    }
    pthread_mutex_unlock(&c->lock);
    c->ae_wait = 2;
}

/* ---- frame assembly (spca561.c: sd_pkt_scan) ---- */

static void count_drop(qc_cam *c) {
    pthread_mutex_lock(&c->lock);
    c->ctl.dropped++;
    pthread_mutex_unlock(&c->lock);
}

// Residual black level (raw green levels) removed in software: what the
// pedestal model misses, plus some auto-levels on scenes without real black.
#define SW_BLACK_MAX 20

// Bayer -> RGBX into c->rgbx with white balance, levels, gamma and colour.
// Returns the mean luma. USB thread only (and the offline tuning harness).
static int process_bayer(qc_cam *c, const uint8_t *bayer, int awb, float gamma, float saturation,
                         int black) {
    int w = c->width, h = c->height;
    const double hw[3] = {HW_GAIN_R, 1.0, HW_GAIN_B};

    // The darkest 1% of the picture should be black. The residual offset is
    // added before the hardware white balance, so it scales with it.
    int dark = isp_green_percentile(bayer, w, h, 0.01);
    c->sw_black += 0.2f * ((dark < SW_BLACK_MAX ? dark : SW_BLACK_MAX) - c->sw_black);

    // The sensor clips at 255 *before* the black level is subtracted, so each
    // channel's white point is (255 - black) times its hardware gain.
    double white[3];
    float blk[3];
    int limit[3];
    for (int i = 0; i < 3; i++) {
        white[i] = (255 - black) * hw[i];
        if (white[i] > 255) white[i] = 255;
        white[i] *= 0.96;  // so that noisy clipped areas still reach full scale
        limit[i] = (int)(white[i] * 0.92);
        blk[i] = (float)(c->sw_black * hw[i]);
    }

    if (awb) {  // grey world over near-grey surfaces, smoothed over a few frames
        double m[3];
        const float cur[3] = {c->wb_r, 1.0f, c->wb_b};
        isp_wb_stats(bayer, w, h, limit, blk, cur, m);
        if (m[0] > 2 && m[1] > 2 && m[2] > 2) {
            double tr = m[1] / m[0], tb = m[1] / m[2];
            tr = tr < 0.25 ? 0.25 : tr > 4 ? 4 : tr;
            tb = tb < 0.25 ? 0.25 : tb > 4 ? 4 : tb;
            c->wb_r += (float)(0.15 * (tr - c->wb_r));
            c->wb_b += (float)(0.15 * (tb - c->wb_b));
        }
    } else {
        c->wb_r = c->wb_b = 1.0f;
    }
    if (gamma != c->lut_gamma) {
        isp_gamma_lut(c->lut, gamma);
        c->lut_gamma = gamma;
    }
    // Scale so that the first channel to clip reaches full scale; past that
    // point colour is unknown, so clipped areas come out white instead of tinted.
    double wb[3] = {c->wb_r, 1.0, c->wb_b}, ceiling = 1e9;
    for (int i = 0; i < 3; i++)
        if ((white[i] - blk[i]) * wb[i] < ceiling) ceiling = (white[i] - blk[i]) * wb[i];
    float gains[3];
    for (int i = 0; i < 3; i++) gains[i] = (float)(wb[i] * 1023.0 / ceiling);
    isp_render(bayer, w, h, blk, gains, c->lut, c->rgbx, c->scratch);
    isp_color(c->rgbx, w, h, saturation, (int16_t *)c->scratch);
    return isp_mean_luma(c->rgbx, w, h);
}

static void deliver_frame(qc_cam *c) {
    int w = c->width, h = c->height;
    const uint8_t *bayer;

    if (c->compressed) {
        if (c->fill < HDR_LEN + 2 * (size_t)w) goto drop;
        memset(c->fbuf + c->fill, 0, FBUF_PAD);
        if (spca561_decode(c->fbuf, c->fill, c->bayer, w, h)) goto drop;
        bayer = c->bayer;
    } else {
        if (c->fill < (size_t)w * h) goto drop;
        bayer = c->fbuf;
    }

    pthread_mutex_lock(&c->lock);
    int awb = c->ctl.auto_white_balance, ae = c->ctl.auto_exposure;
    float gamma = c->ctl.gamma, saturation = c->ctl.saturation;
    int black = c->black;
    pthread_mutex_unlock(&c->lock);

    int luma = process_bayer(c, bayer, awb, gamma, saturation, black);
    if (ae) auto_exposure(c, luma);

    pthread_mutex_lock(&c->lock);
    c->ctl.gain_r = c->wb_r;
    c->ctl.gain_b = c->wb_b;
    c->ctl.mean_luma = luma;
    uint64_t seq = c->ctl.frames++;
    pthread_mutex_unlock(&c->lock);

    qc_frame f = {
        .width = w,
        .height = h,
        .rgbx = c->rgbx,
        .bayer = bayer,
        .sequence = seq,
        .compressed = c->compressed,
        .button = c->button_edge,
    };
    c->button_edge = 0;
    c->cb(c->cb_ctx, &f);
    return;

drop:
    count_drop(c);
}

static void append(qc_cam *c, const uint8_t *d, int len) {
    if (len <= 0) return;
    if (c->fill + len > FBUF_SIZE) {
        c->discard = 1;
        return;
    }
    memcpy(c->fbuf + c->fill, d, len);
    c->fill += len;
}

static void on_packet(void *ctx, const uint8_t *d, int len) {
    qc_cam *c = ctx;
    if (len == QCUSB_STREAM_ENDED) {
        c->cb(c->cb_ctx, NULL);
        return;
    }
    if (len == QCUSB_PKT_ERROR) {
        c->discard = 1;
        return;
    }
    if (len < 1) return;

    uint8_t seq = *d++;
    len--;
    if (seq == 0xff) return;  // empty packet

    if (seq == 0) {  // start of frame: the previous one is complete
        if (c->active && !c->discard) deliver_frame(c);
        else if (c->active) count_drop(c);
        c->fill = 0;
        c->discard = 0;
        c->active = 0;
        if (len < 2) return;

        int down = (d[0] & 0x20) != 0;  // snapshot button
        if (down && !c->button_down) c->button_edge = 1;
        c->button_down = down;

        c->compressed = (d[1] & 0x10) != 0;
        c->active = 1;
        if (!c->compressed) {  // raw Bayer: skip the header
            d += HDR_LEN;
            len -= HDR_LEN;
        }
        append(c, d, len);
        return;
    }
    if (c->active) append(c, d, len);
}

/* ---- public API ---- */

qc_cam *qc_open(char *err, size_t errlen) {
    qc_usb *u = qcusb_open(QC_VID, QC_PID, err, errlen);
    if (!u) return NULL;

    qc_cam *c = calloc(1, sizeof *c);
    c->usb = u;
    pthread_mutex_init(&c->lock, NULL);

    // Read the USB IDs back from the bridge's global registers, as Linux does,
    // to prove we are talking to an SPCA561.
    uint8_t id[4];
    for (int i = 0; i < 4; i++)
        if (reg_r(c, 0x8104 + i, &id[i], 1)) {
            snprintf(err, errlen, "camera does not answer register reads");
            qc_close(c);
            return NULL;
        }
    if ((id[0] | id[1] << 8) != QC_VID || (id[2] | id[3] << 8) != QC_PID) {
        snprintf(err, errlen, "unexpected bridge ID %02x%02x:%02x%02x", id[1], id[0], id[3], id[2]);
        qc_close(c);
        return NULL;
    }

    init_161rev12A(c);

    c->ctl.exposure = 700;  // Linux defaults
    c->ctl.gain = 63;
    c->ctl.auto_exposure = 1;
    c->ctl.auto_white_balance = 1;
    c->ctl.gamma = 1.6f;
    c->ctl.saturation = 1.3f;  // the sensor's colour separation is weak
    c->ctl.gain_r = c->ctl.gain_b = 1.0f;
    c->wb_r = c->wb_b = 1.0f;
    c->fbuf = malloc(FBUF_SIZE + FBUF_PAD);
    c->bayer = malloc(352 * 288);
    c->rgbx = malloc(352 * 288 * 4);
    c->scratch = malloc(3 * 352 * 288 * sizeof(uint16_t));
    if (!c->fbuf || !c->bayer || !c->rgbx || !c->scratch) {
        snprintf(err, errlen, "out of memory");
        qc_close(c);
        return NULL;
    }
    return c;
}

void qc_close(qc_cam *c) {
    if (!c) return;
    qc_stop(c);
    qcusb_close(c->usb);
    pthread_mutex_destroy(&c->lock);
    free(c->fbuf);
    free(c->bayer);
    free(c->rgbx);
    free(c->scratch);
    free(c);
}

int qc_start(qc_cam *c, int width, int height, qc_frame_cb cb, void *ctx, char *err, size_t errlen) {
    if (c->streaming) qc_stop(c);
    int m = -1;
    for (int i = 0; i < (int)(sizeof modes / sizeof modes[0]); i++)
        if (modes[i].w == width && modes[i].h == height) m = i;
    if (m < 0) {
        snprintf(err, errlen, "unsupported size %dx%d", width, height);
        return -1;
    }

    // Use the alternate setting with the most bandwidth (gspca: needs_full_bandwidth).
    int sizes[32] = {0};
    int n = qcusb_alt_packet_sizes(c->usb, sizes, 32);
    int ok = 0;
    for (int tries = 0; tries < n && !ok; tries++) {
        int best = 0;
        for (int a = 1; a < n; a++)
            if (sizes[a] > sizes[best]) best = a;
        if (!sizes[best]) break;
        ok = qcusb_set_alt(c->usb, (uint8_t)best) == 0;
        sizes[best] = 0;
    }
    if (!ok) {
        snprintf(err, errlen, "cannot select an isochronous alternate setting");
        return -1;
    }

    c->width = width;
    c->height = height;
    c->priv = modes[m].priv;
    c->cb = cb;
    c->cb_ctx = ctx;
    c->fill = c->active = c->discard = 0;
    c->ae_wait = 0;

    pthread_mutex_lock(&c->lock);
    sd_start_12a(c);
    // gspca applies every control right after start (v4l2_ctrl_handler_setup).
    setwhite(c, HW_WHITE);
    setexposure(c, c->ctl.exposure);
    setgain(c, c->ctl.gain);
    update_black_level(c);
    c->ae_total = ae_total_from(c->ctl.exposure, c->ctl.gain);
    pthread_mutex_unlock(&c->lock);

    if (qcusb_stream_start(c->usb, on_packet, c)) {
        snprintf(err, errlen, "cannot start isochronous streaming");
        sd_stopN(c);
        qcusb_set_alt(c->usb, 0);
        return -1;
    }
    c->streaming = 1;
    return 0;
}

void qc_stop(qc_cam *c) {
    if (!c->streaming) return;
    pthread_mutex_lock(&c->lock);
    sd_stopN(c);
    pthread_mutex_unlock(&c->lock);
    qcusb_stream_stop(c->usb);
    qcusb_set_alt(c->usb, 0);
    c->streaming = 0;
}

void qc_get_controls(qc_cam *c, qc_controls *out) {
    pthread_mutex_lock(&c->lock);
    *out = c->ctl;
    pthread_mutex_unlock(&c->lock);
}

void qc_set_exposure(qc_cam *c, int exposure) {
    pthread_mutex_lock(&c->lock);
    c->ctl.auto_exposure = 0;
    c->ctl.exposure = clampi(exposure, 1, QC_EXPOSURE_MAX);
    if (c->streaming) setexposure(c, c->ctl.exposure);
    pthread_mutex_unlock(&c->lock);
}

void qc_set_gain(qc_cam *c, int gain) {
    pthread_mutex_lock(&c->lock);
    c->ctl.auto_exposure = 0;
    c->ctl.gain = clampi(gain, 0, QC_GAIN_MAX);
    if (c->streaming) {
        setgain(c, c->ctl.gain);
        update_black_level(c);
    }
    pthread_mutex_unlock(&c->lock);
}

void qc_set_auto_exposure(qc_cam *c, int on) {
    pthread_mutex_lock(&c->lock);
    if (on && !c->ctl.auto_exposure) c->ae_total = ae_total_from(c->ctl.exposure, c->ctl.gain);
    c->ctl.auto_exposure = on;
    pthread_mutex_unlock(&c->lock);
}

void qc_set_auto_white_balance(qc_cam *c, int on) {
    pthread_mutex_lock(&c->lock);
    c->ctl.auto_white_balance = on;
    pthread_mutex_unlock(&c->lock);
}

void qc_set_brightness(qc_cam *c, int brightness) {
    pthread_mutex_lock(&c->lock);
    c->ctl.brightness = clampi(brightness, -128, 127);
    if (c->streaming) update_black_level(c);
    pthread_mutex_unlock(&c->lock);
}

void qc_set_saturation(qc_cam *c, float saturation) {
    pthread_mutex_lock(&c->lock);
    c->ctl.saturation = !(saturation >= 0) ? 0 : saturation > 3 ? 3 : saturation;  // NaN -> 0
    pthread_mutex_unlock(&c->lock);
}

void qc_set_gamma(qc_cam *c, float gamma) {
    pthread_mutex_lock(&c->lock);
    c->ctl.gamma = !(gamma >= 0.5f) ? 0.5f : gamma > 3.0f ? 3.0f : gamma;  // NaN -> 0.5
    pthread_mutex_unlock(&c->lock);
}

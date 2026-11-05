// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// qcsnap: grab a still from the QuickCam Express and save it as PNG.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "image_io.h"
#include "quickcam.h"

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int skip, count, saved, done, dead;
    const char *out;
    uint8_t *img, *raw;
    int w, h;
} st = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void on_frame(void *ctx, const qc_frame *f) {
    (void)ctx;
    pthread_mutex_lock(&st.lock);
    if (!f) {
        st.dead = 1;
    } else if (!st.done && (int)f->sequence >= st.skip) {
        if (!st.img) st.img = malloc((size_t)f->width * f->height * 4);
        memcpy(st.img, f->rgbx, (size_t)f->width * f->height * 4);
        if (!st.raw) st.raw = malloc((size_t)f->width * f->height);
        memcpy(st.raw, f->bayer, (size_t)f->width * f->height);
        st.w = f->width;
        st.h = f->height;
        st.saved++;
        st.done = st.saved >= st.count;
        pthread_cond_signal(&st.cond);
    }
    pthread_mutex_unlock(&st.lock);
}

static volatile sig_atomic_t g_interrupted;
static void on_signal(int sig) {
    (void)sig;
    g_interrupted = 1;  // the main loop notices within 250 ms and stops the camera
}

static void usage(void) {
    fprintf(stderr,
            "usage: qcsnap [-s WxH] [-n frames] [-c count] [-e exposure -g gain] [-o out.png] [-v]\n"
            "  -s  352x288 (default), 320x240, 176x144 or 160x120\n"
            "  -n  frames to discard first so auto exposure can settle (default 45)\n"
            "  -c  number of images to save; with c>1 the name gets a -N suffix\n"
            "  -e/-g  fixed exposure (1-%d) and gain (0-%d), disables auto exposure\n"
            "  -r  also save the raw GBRG Bayer frame as a PGM next to the PNG\n"
            "  -v  print exposure statistics while waiting\n",
            QC_EXPOSURE_MAX, QC_GAIN_MAX);
    exit(2);
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int w = 352, h = 288, expo = -1, gain = -1, verbose = 0, raw = 0, bright = 0, bright_set = 0, opt;
    st.skip = 45;
    st.count = 1;
    st.out = "quickcam.png";
    while ((opt = getopt(argc, argv, "s:n:c:e:g:B:o:rvh")) != -1) {
        switch (opt) {
        case 's':
            if (sscanf(optarg, "%dx%d", &w, &h) != 2) usage();
            break;
        case 'n': st.skip = atoi(optarg); break;
        case 'c': st.count = atoi(optarg) < 1 ? 1 : atoi(optarg); break;
        case 'e': expo = atoi(optarg); break;
        case 'g': gain = atoi(optarg); break;
        case 'B': bright = atoi(optarg); bright_set = 1; break;
        case 'o': st.out = optarg; break;
        case 'r': raw = 1; break;
        case 'v': verbose = 1; break;
        default: usage();
        }
    }

    char err[256];
    qc_cam *cam = qc_open(err, sizeof err);
    if (!cam) {
        fprintf(stderr, "qcsnap: %s\n", err);
        return 1;
    }
    if (expo >= 0) qc_set_exposure(cam, expo);
    if (gain >= 0) qc_set_gain(cam, gain);
    if (bright_set) qc_set_brightness(cam, bright);
    if (qc_start(cam, w, h, on_frame, NULL, err, sizeof err)) {
        fprintf(stderr, "qcsnap: %s\n", err);
        qc_close(cam);
        return 1;
    }

    double t0 = now(), last = t0;
    int written = 0, rc = 0;
    pthread_mutex_lock(&st.lock);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (written < st.count && !st.dead && !g_interrupted) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&st.cond, &st.lock, &ts);

        qc_controls c;
        qc_get_controls(cam, &c);
        if (verbose && now() - last >= 1.0) {
            last = now();
            fprintf(stderr, "frames %llu dropped %llu  %.1f fps  exposure %d gain %d  luma %d  wb r%.2f b%.2f\n",
                    c.frames, c.dropped, c.frames / (last - t0), c.exposure, c.gain, c.mean_luma,
                    c.gain_r, c.gain_b);
        }
        if (c.frames == 0 && now() - t0 > 5) {
            fprintf(stderr, "qcsnap: no frames after 5 s (%llu dropped)\n", c.dropped);
            rc = 1;
            break;
        }
        while (written < st.saved) {
            char path[1024];
            if (st.count == 1) snprintf(path, sizeof path, "%s", st.out);
            else {
                const char *dot = strrchr(st.out, '.');
                int stem = dot ? (int)(dot - st.out) : (int)strlen(st.out);
                snprintf(path, sizeof path, "%.*s-%d%s", stem, st.out, written + 1, dot ? dot : ".png");
            }
            if (qc_write_png(path, st.img, st.w, st.h)) {
                fprintf(stderr, "qcsnap: cannot write %s\n", path);
                rc = 1;
            } else {
                printf("%s (%dx%d)\n", path, st.w, st.h);
            }
            if (raw) {
                strlcat(path, ".pgm", sizeof path);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    fprintf(fp, "P5\n%d %d\n255\n", st.w, st.h);
                    fwrite(st.raw, 1, (size_t)st.w * st.h, fp);
                    fclose(fp);
                    printf("%s (raw GBRG)\n", path);
                }
            }
            written++;
        }
    }
    if (g_interrupted) {
        fprintf(stderr, "qcsnap: interrupted\n");
        rc = 130;
    }
    if (st.dead) {
        fprintf(stderr, "qcsnap: camera stopped streaming\n");
        rc = 1;
    }
    pthread_mutex_unlock(&st.lock);

    qc_controls c;
    qc_get_controls(cam, &c);
    fprintf(stderr, "%llu frames (%llu dropped) in %.1f s, exposure %d gain %d\n", c.frames, c.dropped,
            now() - t0, c.exposure, c.gain);
    qc_close(cam);
    return rc;
}

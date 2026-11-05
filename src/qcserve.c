// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// qcserve: publish the QuickCam as an MJPEG-over-HTTP feed (for OBS, VLC or a
// browser), with a web page to adjust the picture. The camera only runs while
// somebody is watching. Settings persist across restarts.
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "image_io.h"
#include "page_html.h"  // generated from src/page.html
#include "quickcam.h"

#define IDLE_STOP_SECONDS 5.0  // keep the camera on this long after the last viewer leaves
#define SETTLE_FRAMES 30       // frames for auto exposure to settle before a cold snapshot
#define MAX_CONNECTIONS 64
#define HEADER_DEADLINE 10.0  // seconds a client gets to send its request headers

static const char video_page[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>QuickCam</title><style>"
    "html,body{margin:0;height:100%;background:#000;overflow:hidden}"
    "img{width:100%;height:100%;object-fit:contain;display:block}</style></head><body>"
    "<img src=\"/stream.mjpg\" onerror=\"setTimeout(()=>this.src='/stream.mjpg?'+Date.now(),1000)\">"
    "</body></html>";

typedef struct {
    int width, height;
    int auto_exposure, exposure, gain;
    int auto_white_balance, brightness;
    float gamma, saturation, quality;
} settings;

static const settings defaults = {352, 288, 1, 450, 63, 1, 0, 1.6f, 1.3f, 0.85f};

static struct {
    pthread_mutex_t lock;  // guards everything below except cam
    pthread_cond_t changed;  // new frame, viewer count or camera state changed
    uint8_t *jpeg;
    size_t jpeg_len;
    uint64_t seq, frames_since_start;
    int viewers, running, dead;
    double fps, idle_since;
    char error[256];
    settings set;

    pthread_mutex_t cam_lock;  // guards cam (open/close/start vs. control changes)
    qc_cam *cam;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER,
       .cam_lock = PTHREAD_MUTEX_INITIALIZER};

static int g_port;
static int g_loopback;  // listening on a loopback address
static int g_connections;
static char g_conf[1024];

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logmsg(const char *fmt, ...) {
    char t[32];
    time_t tt = time(NULL);
    struct tm tm;
    strftime(t, sizeof t, "%H:%M:%S", localtime_r(&tt, &tm));
    fprintf(stderr, "%s ", t);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void wait_changed(double seconds) {  // S.lock held
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ns = ts.tv_nsec + (long)(seconds * 1e9);
    ts.tv_sec += ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;
    pthread_cond_timedwait(&S.changed, &S.lock, &ts);
}

/* ---- settings file ---- */

static int valid_size(int w, int h) {
    return (w == 352 && h == 288) || (w == 320 && h == 240) || (w == 176 && h == 144) || (w == 160 && h == 120);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }  // NaN -> lo

// Bring settings from the API or a hand-edited file back into range.
static void sanitize(settings *s) {
    if (!valid_size(s->width, s->height)) s->width = defaults.width, s->height = defaults.height;
    s->auto_exposure = s->auto_exposure != 0;
    s->auto_white_balance = s->auto_white_balance != 0;
    s->exposure = clampi(s->exposure, 1, QC_EXPOSURE_MAX);
    s->gain = clampi(s->gain, 0, QC_GAIN_MAX);
    s->brightness = clampi(s->brightness, -128, 127);
    s->gamma = clampf(s->gamma, 0.5f, 3);
    s->saturation = clampf(s->saturation, 0, 3);
    s->quality = clampf(s->quality, 0.3f, 1);
}

static void load_settings(void) {
    S.set = defaults;
    FILE *f = fopen(g_conf, "r");
    if (!f) return;
    char line[128];
    settings *s = &S.set;
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "size=%dx%d", &s->width, &s->height);
        sscanf(line, "auto_exposure=%d", &s->auto_exposure);
        sscanf(line, "exposure=%d", &s->exposure);
        sscanf(line, "gain=%d", &s->gain);
        sscanf(line, "auto_white_balance=%d", &s->auto_white_balance);
        sscanf(line, "brightness=%d", &s->brightness);
        sscanf(line, "gamma=%f", &s->gamma);
        sscanf(line, "saturation=%f", &s->saturation);
        sscanf(line, "quality=%f", &s->quality);
    }
    fclose(f);
    sanitize(s);
}

static void save_settings(const settings *s) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_conf);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "size=%dx%d\nauto_exposure=%d\nexposure=%d\ngain=%d\nauto_white_balance=%d\n"
            "brightness=%d\ngamma=%.2f\nsaturation=%.2f\nquality=%.2f\n",
            s->width, s->height, s->auto_exposure, s->exposure, s->gain, s->auto_white_balance,
            s->brightness, s->gamma, s->saturation, s->quality);
    fclose(f);
    rename(tmp, g_conf);
}

static void apply_settings(qc_cam *cam, const settings *s) {
    qc_set_gamma(cam, s->gamma);
    qc_set_saturation(cam, s->saturation);
    qc_set_brightness(cam, s->brightness);
    qc_set_auto_white_balance(cam, s->auto_white_balance);
    qc_set_exposure(cam, s->exposure);  // also the starting point for auto exposure
    qc_set_gain(cam, s->gain);
    qc_set_auto_exposure(cam, s->auto_exposure);
}

/* ---- camera ---- */

static void on_frame(void *ctx, const qc_frame *f) {
    (void)ctx;
    if (!f) {
        pthread_mutex_lock(&S.lock);
        S.dead = 1;
        pthread_cond_broadcast(&S.changed);
        pthread_mutex_unlock(&S.lock);
        return;
    }
    pthread_mutex_lock(&S.lock);
    float q = S.set.quality;
    pthread_mutex_unlock(&S.lock);

    size_t len;
    uint8_t *jpg = qc_encode_jpeg(f->rgbx, f->width, f->height, q, &len);
    if (!jpg) return;
    pthread_mutex_lock(&S.lock);
    free(S.jpeg);
    S.jpeg = jpg;
    S.jpeg_len = len;
    S.seq++;
    S.frames_since_start++;
    pthread_cond_broadcast(&S.changed);
    pthread_mutex_unlock(&S.lock);
}

// Opens the camera when somebody watches, closes it when nobody has for a while,
// and recovers from unplugging.
static void *supervisor(void *arg) {
    (void)arg;
    double retry_at = 0, fps_t = now();
    uint64_t fps_frames = 0;
    for (;;) {
        pthread_mutex_lock(&S.lock);
        wait_changed(0.25);
        int want = S.viewers > 0, dead = S.dead;
        double idle = want ? 0 : now() - S.idle_since;
        settings set = S.set;
        pthread_mutex_unlock(&S.lock);

        pthread_mutex_lock(&S.cam_lock);
        if (S.cam && (dead || (!want && idle > IDLE_STOP_SECONDS))) {
            qc_controls k;
            qc_get_controls(S.cam, &k);
            qc_close(S.cam);
            S.cam = NULL;
            pthread_mutex_lock(&S.lock);
            S.set.exposure = k.exposure;  // resume auto exposure from here next time
            S.set.gain = k.gain;
            S.running = S.dead = 0;
            S.fps = 0;
            if (dead) snprintf(S.error, sizeof S.error, "camera disconnected");
            pthread_cond_broadcast(&S.changed);
            pthread_mutex_unlock(&S.lock);
            logmsg(dead ? "camera disconnected" : "nobody watching, camera off");
        }
        if (!S.cam && want && now() >= retry_at) {
            char err[256] = "";
            qc_cam *c = qc_open(err, sizeof err);
            if (c) {
                apply_settings(c, &set);
                if (qc_start(c, set.width, set.height, on_frame, NULL, err, sizeof err)) {
                    qc_close(c);
                    c = NULL;
                }
            }
            pthread_mutex_lock(&S.lock);
            if (c) {
                S.running = 1;
                S.frames_since_start = 0;
                S.error[0] = 0;
                logmsg("camera on, %dx%d", set.width, set.height);
            } else if (strcmp(S.error, err)) {
                snprintf(S.error, sizeof S.error, "%s", err);
                logmsg("cannot start camera: %s", err);
            }
            pthread_cond_broadcast(&S.changed);
            pthread_mutex_unlock(&S.lock);
            S.cam = c;
            if (!c) retry_at = now() + 2;
            fps_t = now();
            fps_frames = 0;
        }
        if (S.cam && now() - fps_t >= 1.0) {
            qc_controls k;
            qc_get_controls(S.cam, &k);
            pthread_mutex_lock(&S.lock);
            S.fps = (k.frames - fps_frames) / (now() - fps_t);
            pthread_mutex_unlock(&S.lock);
            fps_frames = k.frames;
            fps_t = now();
        }
        pthread_mutex_unlock(&S.cam_lock);
    }
    return NULL;
}

static void viewer_enter(void) {
    pthread_mutex_lock(&S.lock);
    S.viewers++;
    pthread_cond_broadcast(&S.changed);
    pthread_mutex_unlock(&S.lock);
}

static void viewer_leave(void) {
    pthread_mutex_lock(&S.lock);
    if (--S.viewers == 0) S.idle_since = now();
    pthread_cond_broadcast(&S.changed);
    pthread_mutex_unlock(&S.lock);
}

/* ---- HTTP ---- */

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void respond(int fd, int code, const char *type, const void *body, size_t len) {
    const char *reason = code == 200 ? "OK" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                       : code == 405 ? "Method Not Allowed" : "Service Unavailable";
    // Pages may not be framed by other sites (clickjacking).
    const char *extra = strncmp(type, "text/html", 9) ? ""
                        : "Content-Security-Policy: frame-ancestors 'none'\r\nX-Frame-Options: DENY\r\n";
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, reason, type, len, extra);
    if (write_all(fd, hdr, (size_t)n) == 0) write_all(fd, body, len);
}

static void respond_text(int fd, int code, const char *text) {
    respond(fd, code, "text/plain; charset=utf-8", text, strlen(text));
}

static int peer_closed(int fd) {
    char c;
    ssize_t k = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    return k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

static void serve_stream(int fd) {
    static const char hdr[] =
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=qcframe\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (write_all(fd, hdr, sizeof hdr - 1)) return;
    viewer_enter();
    pthread_mutex_lock(&S.lock);
    uint64_t last = S.seq;  // start with the next fresh frame
    for (;;) {
        while (S.seq == last) {
            wait_changed(1.0);
            if (S.seq == last && peer_closed(fd)) goto out;
        }
        size_t n = S.jpeg_len;
        uint8_t *buf = malloc(n);
        if (!buf) break;
        memcpy(buf, S.jpeg, n);
        last = S.seq;
        pthread_mutex_unlock(&S.lock);

        char part[128];
        int k = snprintf(part, sizeof part, "--qcframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", n);
        int bad = write_all(fd, part, (size_t)k) || write_all(fd, buf, n) || write_all(fd, "\r\n", 2);
        free(buf);
        pthread_mutex_lock(&S.lock);
        if (bad) break;
    }
out:
    pthread_mutex_unlock(&S.lock);
    viewer_leave();
}

static void serve_snapshot(int fd) {
    viewer_enter();
    pthread_mutex_lock(&S.lock);
    double deadline = now() + 10;
    uint64_t start = S.seq;
    while (!(S.running && S.frames_since_start >= SETTLE_FRAMES && S.seq > start) && now() < deadline)
        wait_changed(0.5);
    uint8_t *buf = NULL;
    size_t n = 0;
    if (S.running && S.seq > start) {
        n = S.jpeg_len;
        if ((buf = malloc(n))) memcpy(buf, S.jpeg, n);
    }
    char err[256];
    snprintf(err, sizeof err, "no picture: %s\n", S.error[0] ? S.error : "camera did not start");
    pthread_mutex_unlock(&S.lock);
    viewer_leave();
    if (buf) respond(fd, 200, "image/jpeg", buf, n);
    else respond_text(fd, 503, err);
    free(buf);
}

static void serve_controls(int fd) {
    qc_controls k;
    int have = 0;
    pthread_mutex_lock(&S.cam_lock);
    if (S.cam) {
        qc_get_controls(S.cam, &k);
        have = 1;
    }
    pthread_mutex_unlock(&S.cam_lock);

    pthread_mutex_lock(&S.lock);
    settings s = S.set;
    int running = S.running, viewers = S.viewers;
    double fps = S.fps;
    char err[256];
    snprintf(err, sizeof err, "%s", S.error);
    pthread_mutex_unlock(&S.lock);
    for (char *p = err; *p; p++)
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = '\'';
    if (!have) {
        memset(&k, 0, sizeof k);
        k.exposure = s.exposure;
        k.gain = s.gain;
        k.gain_r = k.gain_b = 1;
    }

    char body[1024];
    int n = snprintf(body, sizeof body,
                     "{\"running\":%s,\"viewers\":%d,\"fps\":%.1f,\"width\":%d,\"height\":%d,"
                     "\"auto_exposure\":%d,\"exposure\":%d,\"gain\":%d,\"auto_white_balance\":%d,"
                     "\"brightness\":%d,\"gamma\":%.2f,\"saturation\":%.2f,\"quality\":%.2f,"
                     "\"wb_r\":%.3f,\"wb_b\":%.3f,\"luma\":%d,\"error\":\"%s\"}",
                     running ? "true" : "false", viewers, fps, s.width, s.height, s.auto_exposure,
                     k.exposure, k.gain, s.auto_white_balance, s.brightness, s.gamma, s.saturation,
                     s.quality, k.gain_r, k.gain_b, k.mean_luma, err);
    respond(fd, 200, "application/json", body, (size_t)n);
}

// POST /api/set?key=value&...  Applies to the running camera and persists.
static void serve_set(int fd, char *query) {
    pthread_mutex_lock(&S.cam_lock);
    pthread_mutex_lock(&S.lock);
    settings s = S.set, old = S.set;
    pthread_mutex_unlock(&S.lock);
    if (S.cam) {  // auto exposure owns these while it runs
        qc_controls k;
        qc_get_controls(S.cam, &k);
        s.exposure = k.exposure;
        s.gain = k.gain;
    }

    char *save = NULL;
    for (char *kv = strtok_r(query, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
        char *v = strchr(kv, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(kv, "reset")) s = defaults;
        else if (!strcmp(kv, "size")) {
            int w, h;
            if (sscanf(v, "%dx%d", &w, &h) == 2 && valid_size(w, h)) s.width = w, s.height = h;
        } else if (!strcmp(kv, "auto_exposure")) s.auto_exposure = atoi(v) != 0;
        else if (!strcmp(kv, "exposure")) s.exposure = atoi(v), s.auto_exposure = 0;
        else if (!strcmp(kv, "gain")) s.gain = atoi(v), s.auto_exposure = 0;
        else if (!strcmp(kv, "auto_white_balance")) s.auto_white_balance = atoi(v) != 0;
        else if (!strcmp(kv, "brightness")) s.brightness = atoi(v);
        else if (!strcmp(kv, "gamma")) s.gamma = (float)atof(v);
        else if (!strcmp(kv, "saturation")) s.saturation = (float)atof(v);
        else if (!strcmp(kv, "quality")) s.quality = (float)atof(v);
    }
    sanitize(&s);

    if (S.cam) {
        apply_settings(S.cam, &s);
        if (s.width != old.width || s.height != old.height) {
            char err[256];
            if (qc_start(S.cam, s.width, s.height, on_frame, NULL, err, sizeof err)) {
                // Streaming has stopped: let the supervisor reopen the camera.
                logmsg("cannot switch size: %s", err);
                pthread_mutex_lock(&S.lock);
                S.dead = 1;
                snprintf(S.error, sizeof S.error, "%s", err);
                pthread_mutex_unlock(&S.lock);
            } else {
                logmsg("switched to %dx%d", s.width, s.height);
            }
        }
    }
    pthread_mutex_lock(&S.lock);
    S.set = s;
    if (s.width != old.width || s.height != old.height) S.frames_since_start = 0;
    pthread_mutex_unlock(&S.lock);
    save_settings(&s);  // under cam_lock so concurrent saves can't interleave
    pthread_mutex_unlock(&S.cam_lock);
    serve_controls(fd);
}

static const char *header(const char *req, const char *name) {
    size_t n = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2] != '\r'; p = strstr(p + 2, "\r\n"))
        if (!strncasecmp(p + 2, name, n) && p[2 + n] == ':') {
            p += 3 + n;
            while (*p == ' ') p++;
            return p;
        }
    return NULL;
}

static int header_is(const char *req, const char *name, const char *value) {
    const char *h = header(req, name);
    size_t n = strlen(value);
    return h && !strncasecmp(h, value, n) && (h[n] == '\r' || h[n] == 0);
}

// Only answer requests addressed to an IP literal or "localhost" on our port.
// A DNS-rebinding page reaches us under its own domain name, which its Host
// header gives away, so other web sites can't read the feed this way.
static int host_allowed(const char *req) {
    const char *h = header(req, "Host");
    if (!h) return 0;
    size_t len = strcspn(h, "\r\n");
    char host[128];
    if (!len || len >= sizeof host) return 0;
    memcpy(host, h, len);
    host[len] = 0;
    long port = 80;
    char *colon = strrchr(host, ':'), *bracket = strrchr(host, ']');
    if (colon && (!bracket || colon > bracket)) {
        char *end;
        port = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end) return 0;
        *colon = 0;
    }
    struct in_addr a;
    return port == g_port &&
           (!strcasecmp(host, "localhost") || !strcmp(host, "[::1]") || inet_pton(AF_INET, host, &a) == 1);
}

// Cross-site form posts carry a foreign Origin; only accept our own.
static int origin_allowed(const char *req) {
    const char *o = header(req, "Origin");
    if (!o) return 1;
    const char *h = header(req, "Host");
    if (!h) return 0;
    size_t hl = strcspn(h, "\r\n");
    return !strncmp(o, "http://", 7) && !strncmp(o + 7, h, hl) && (o[7 + hl] == '\r' || o[7 + hl] == 0);
}

static void *connection(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);

    char req[8192];
    size_t n = 0;
    double deadline = now() + HEADER_DEADLINE;
    while (n < sizeof req - 1 && now() < deadline) {
        ssize_t k = read(fd, req + n, sizeof req - 1 - n);
        if (k <= 0) break;
        n += (size_t)k;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[n] = 0;

    char method[8], target[1024];
    if (sscanf(req, "%7s %1023s", method, target) != 2) goto done;
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    else query = target + strlen(target);

    if (!host_allowed(req) || header_is(req, "Sec-Fetch-Site", "cross-site")) {
        // Browsers mark requests made by other web sites (e.g. an <img> pointing
        // here, which would switch the camera on); OBS and VLC don't send it.
        respond_text(fd, 403, "forbidden\n");
    } else if (!strcmp(target, "/api/set")) {
        if (strcmp(method, "POST")) respond_text(fd, 405, "use POST\n");
        else if (!origin_allowed(req)) respond_text(fd, 403, "forbidden\n");
        else serve_set(fd, query);
    } else if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        respond_text(fd, 405, "method not allowed\n");
    } else if (!strcmp(target, "/")) {
        respond(fd, 200, "text/html; charset=utf-8", page_html, page_html_len);
    } else if (!strcmp(target, "/video")) {
        respond(fd, 200, "text/html; charset=utf-8", video_page, sizeof video_page - 1);
    } else if (!strcmp(target, "/stream.mjpg")) {
        serve_stream(fd);
    } else if (!strcmp(target, "/snapshot.jpg")) {
        serve_snapshot(fd);
    } else if (!strcmp(target, "/api/controls")) {
        serve_controls(fd);
    } else {
        respond_text(fd, 404, "not found\n");
    }
done:
    close(fd);
    __atomic_fetch_sub(&g_connections, 1, __ATOMIC_RELAXED);
    return NULL;
}

// SIGINT/SIGTERM: stop the camera (LED off) before exiting.
static void *signal_thread(void *arg) {
    sigset_t *set = arg;
    int sig;
    sigwait(set, &sig);
    pthread_mutex_lock(&S.cam_lock);
    qc_close(S.cam);
    S.cam = NULL;
    logmsg("stopped");
    exit(0);
}

static void usage(void) {
    fprintf(stderr,
            "usage: qcserve [-p port] [-a address]\n"
            "  -p  TCP port (default 8090)\n"
            "  -a  address to listen on (default 127.0.0.1; 0.0.0.0 exposes the camera to your network)\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *addr = "127.0.0.1";
    int opt;
    g_port = 8090;
    while ((opt = getopt(argc, argv, "p:a:h")) != -1) {
        switch (opt) {
        case 'p': g_port = atoi(optarg); break;
        case 'a': addr = optarg; break;
        default: usage();
        }
    }

    const char *home = getenv("HOME");
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/QuickCam", home ? home : ".");
    mkdir(dir, 0755);
    snprintf(g_conf, sizeof g_conf, "%s/qcserve.conf", dir);
    load_settings();

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_port)};
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) usage();
    g_loopback = (ntohl(sa.sin_addr.s_addr) >> 24) == 127;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (ls >= 0) setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 16)) {
        fprintf(stderr, "qcserve: cannot listen on %s:%d: %s\n", addr, g_port, strerror(errno));
        return 1;
    }

    static sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);  // inherited by every thread created below
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    pthread_create(&t, NULL, signal_thread, &sigs);
    pthread_create(&t, NULL, supervisor, NULL);

    const char *shown = g_loopback ? "127.0.0.1" : addr;
    logmsg("QuickCam feed ready. Control page: http://%s:%d/", shown, g_port);
    logmsg("  OBS Browser source: http://%s:%d/video   MJPEG stream: http://%s:%d/stream.mjpg", shown, g_port,
           shown, g_port);
    if (!g_loopback) logmsg("  warning: listening on %s, anyone on your network can watch", addr);
    logmsg("settings: %s", g_conf);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) usleep(100000);  // don't spin when out of fds
            continue;
        }
        if (__atomic_fetch_add(&g_connections, 1, __ATOMIC_RELAXED) >= MAX_CONNECTIONS) {
            __atomic_fetch_sub(&g_connections, 1, __ATOMIC_RELAXED);
            close(fd);
            continue;
        }
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, connection, (void *)(intptr_t)fd)) {
            close(fd);
            __atomic_fetch_sub(&g_connections, 1, __ATOMIC_RELAXED);
        }
        pthread_attr_destroy(&at);
    }
}

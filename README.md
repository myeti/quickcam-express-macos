# 📷 1999 QuickCam Express for MacOS

**A 1999 webcam, back from the drawer, running on Apple Silicon, because why not.**

![platform](https://img.shields.io/badge/platform-macOS%2011%2B-black?logo=apple)
![arch](https://img.shields.io/badge/arch-Apple%20Silicon%20%7C%20Intel-blue)
![kext](https://img.shields.io/badge/kernel%20extension-not%20needed-brightgreen)
![deps](https://img.shields.io/badge/dependencies-none-brightgreen)
![license](https://img.shields.io/badge/license-GPL--2.0--or--later-orange)

```
        .-"""-.
       /  .-.  \        Logitech QuickCam Express
      |  ( o )  |       USB 046d:0928 · Sunplus SPCA561A
       \  '-'  /        352×288 · USB 1.1 · 0.1 megapixels of pure nostalgia
        '-...-'
          | |           macOS dropped it decades ago.
       ___|_|___        This brings it back: live view, snapshots,
      '---------'       and a feed you can use in Teams via OBS.
```

A userspace driver for the Logitech QuickCam Express, written in plain C on top
of IOKit. No kernel extension, no DriverKit entitlement, no Homebrew, no
libusb: just the Xcode Command Line Tools and a camera you forgot you owned.

## ⚡ Quick start

You need **macOS 11 Big Sur or later**, on an Apple Silicon or Intel Mac, and the
Xcode Command Line Tools. More in [Compatibility](#compatibility).

```bash
xcode-select --install   # if you don't have the Command Line Tools yet
make
./build/qcview
```

A window opens and your face appears in glorious 352×288. Press `?` to see what
the keys do.

## 🧰 What's in the box

| Program | What it does |
|---------|--------------|
| 🎬 `qcview` | Live video window, snapshots, manual controls |
| 📡 `qcserve` | Local HTTP feed with a web control panel, for OBS (and so Teams/Zoom), browsers and VLC |
| 📸 `qcsnap` | Grab stills from the command line |

## 🎬 qcview

```bash
./build/qcview
```

| Key | Action |
|-----|--------|
| `space` / `s` or the camera's own button | Snapshot to `~/Pictures/QuickCam/` |
| `m` | Mirror the preview (on by default; snapshots are never mirrored) |
| `a` / `w` | Toggle auto exposure / auto white balance |
| `↑` `↓` | Exposure (switches auto exposure off) |
| `←` `→` | Gain (switches auto exposure off) |
| `-` `=` | Brightness |
| `[` `]` | Gamma |
| `,` `.` | Saturation |
| `1`–`4` | 160×120 · 176×144 · 320×240 · 352×288 |
| `i` | Info line on/off |
| `f` | Full screen |
| `?` | Help |

Yank the cable mid-stream and plug it into another port: the viewer waits and
picks the camera back up.

## 📡 qcserve: the camera as a feed

```bash
./build/qcserve
```

| Address | What you get |
|---------|--------------|
| <http://127.0.0.1:8090/> | Control panel: live picture, sliders for everything, snapshot button |
| `http://127.0.0.1:8090/video` | Bare full-window video, made for an OBS Browser source |
| `http://127.0.0.1:8090/stream.mjpg` | MJPEG stream for OBS Media source, VLC, browsers |
| `http://127.0.0.1:8090/snapshot.jpg` | A single still |
| `GET /api/controls` · `POST /api/set?key=value` | JSON status and settings |

- 💡 **The camera is only on while someone watches.** It switches on with the
  first viewer and off 5 seconds after the last one leaves, so the LED isn't on
  all day.
- 💾 **Settings stick** across restarts, in
  `~/Library/Application Support/QuickCam/qcserve.conf`.
- 🔒 **Local only.** It listens on 127.0.0.1 and refuses requests addressed to
  any other host name or coming from other web sites, so the page you have open
  in another tab can't peek. `-a 0.0.0.0` opens it to your network on purpose;
  `-p` changes the port.
- 🚀 `make install-agent` starts it at every login (logs in
  `~/Library/Logs/qcserve.log`); `make uninstall-agent` undoes it.

### 🤝 Using it in Teams (or Zoom, or Meet…) through OBS

macOS only lists cameras that come with a signed Camera Extension, and OBS's
virtual camera is one. So:

1. In OBS: **+ → Browser**, URL `http://127.0.0.1:8090/video`, width 704,
   height 576. Tick **Shutdown source when not visible** so the camera sleeps
   when OBS isn't showing it.
2. Stretch the source over the canvas and hit **Start Virtual Camera**.
3. In Teams: **Settings → Devices → Camera → OBS Virtual Camera**.

The feed isn't mirrored, so the other people in the call see you the right way
round (Teams only mirrors your own preview).

A lighter alternative for step 1 is a **Media Source** with *Local File* unticked
and `http://127.0.0.1:8090/stream.mjpg` as input. It's untested so far; reports
welcome.

## 📸 qcsnap

```bash
./build/qcsnap -o photo.png                # 352×288, once auto exposure has settled
./build/qcsnap -s 176x144 -c 5 -o shot.png # five frames: shot-1.png … shot-5.png
./build/qcsnap -e 450 -g 100 -v            # fixed exposure and gain, print statistics
./build/qcsnap -r                          # also dump the raw Bayer frame as PGM
```

## 🔬 How it works

```
 USB isochronous packets (up to 1023 bytes every millisecond)
        │
        ▼
 frame assembly ──────► SPCA561 decompressor   (352×288 and 320×240 arrive compressed)
        │                       │
        ▼                       ▼
 raw 8-bit GBRG Bayer ◄─────────┘               (176×144 and 160×120 arrive raw)
        │
        ▼
 black point → white balance → demosaic → gamma → colour denoise → saturation
        │
        ▼
 RGB frames ──► qcview window · qcserve MJPEG · qcsnap PNG
```

| File | Role |
|------|------|
| `src/usb_darwin.c` | IOKit (IOUSBLib) transport: open, control transfers, isochronous streaming on its own run-loop thread |
| `src/quickcam.c` | The chip: init/start/stop sequences, controls, frame assembly, auto exposure, black level. Public API in `quickcam.h` |
| `src/spca561_decode.c` | Decoder for the chip's compressed Bayer format |
| `src/isp.c` | The little image pipeline |
| `src/qcview.m` · `src/qcserve.c` · `src/qcsnap.c` | The three programs |

**Why no kernel extension?** The camera is a vendor-class USB device, so nothing
in macOS claims it, and any normal process may open it through IOKit. That's
exactly what libusb does under the hood, minus the dependency.

**Finding the camera.** It looks up the first device with ID `046d:0928`
anywhere on the USB tree. Any port, any hub, and it keeps retrying if you
unplug it.

### 🧪 Field notes

Things this 25-year-old sensor taught us along the way:

- **The black level isn't zero.** The sensor's output sits on a pedestal that
  grows with gain, about 0.58 × gain on our unit. Left alone, everything looks
  like it's behind a foggy window. The driver cancels it with the bridge's
  offset registers every time the gain changes.
- **Highlights clip before that offset is removed.** So each colour channel tops
  out at a different level, and a bright window came out lavender. Each channel
  is now normalised to its own white point, and clipped areas are plain white.
- **Bookshelves fool grey-world white balance.** A room full of warm wood and
  skin pushed everything blue. White balance now only listens to surfaces that
  already look roughly grey, like a T-shirt or the ceiling.
- **USB 1.1 is tiny.** At about 1 MB/s, 352×288 only fits because the chip
  compresses it. Expect 15–18 fps in decent light; auto exposure keeps 30 fps as
  long as it can before trading frame rate for brightness.
- **A 2005 decoder that trusted its input.** The decompressor we started from
  had a table one entry short, and a corrupt frame (one bad bit on the cable is
  enough) made it read past three others. Fixed, and fuzzed with
  AddressSanitizer: bad frames are now simply dropped.

<a id="compatibility"></a>

## 🖥️ Compatibility

| | |
|---|---|
| **macOS** | 11 Big Sur or later (that's the build target). Tested on macOS 26 |
| **Macs** | Apple Silicon (tested) and Intel (compiles, untested) |
| **Linux** | Not needed: the kernel's `gspca_spca561` driver already supports this camera |
| **Windows** | Not supported; this code is macOS-only (IOKit, AppKit, ImageIO) |

### Cameras

| Camera | USB ID | Status |
|--------|--------|--------|
| Logitech QuickCam Express | `046d:0928` | ✅ Works (tested on macOS 26, Apple Silicon) |
| Other SPCA561 "rev 012a" cameras | `046d:0929`–`092f`, `041e:403b` | 🤞 Same chip and protocol: change `QC_PID` in `src/quickcam.c`. Untested |
| Older QuickCam Express (STV0600) | `046d:0840`, `0850`, `0870` | ❌ Different chip, not supported |

## ⚠️ Limitations

- The camera doesn't show up in Teams, Zoom or FaceTime on its own; go through
  `qcserve` and OBS as described above. Showing up natively needs a CoreMediaIO
  Camera Extension, which means full Xcode and a signed host app. `quickcam.h`
  is ready to be called from one, if you're up for it.
- One program at a time: `qcview` can't open the camera while `qcserve` is
  feeding OBS.
- One camera at a time: with two plugged in, it takes whichever macOS lists
  first.

## 🙏 Credits and license

Made by **Magalon** © 2026, standing on the shoulders of the Linux webcam folks:

- The chip protocol (register tables, control encodings, packet format) is
  ported from the Linux gspca `spca561` subdriver by **Michel Xhaard** and
  **Jean-François Moine** (GPL-2.0-or-later).
- `src/spca561_decode.c` is **Andrzej Szombierski**'s decoder from v4l-utils'
  libv4lconvert (LGPL-2.1-or-later).

Because of the ported protocol code, this project is **GPL-2.0-or-later** as a
whole: see [`LICENSE`](LICENSE). The decoder keeps its own LGPL-2.1-or-later
license, whose text is in [`LICENSES/LGPL-2.1.txt`](LICENSES/LGPL-2.1.txt).
Apple's system frameworks (IOKit, AppKit, ImageIO…) add no conditions.

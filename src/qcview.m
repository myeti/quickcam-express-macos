// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Magalon
// qcview: live viewer for the Logitech QuickCam Express.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <signal.h>

#include "image_io.h"
#include "quickcam.h"

static const int kSizes[][2] = {{160, 120}, {176, 144}, {320, 240}, {352, 288}};

static NSString *const kHelp =
    @"space/s  snapshot to ~/Pictures/QuickCam     m  mirror\n"
    @"a  auto exposure     w  auto white balance     i  info\n"
    @"↑/↓  exposure     ←/→  gain     -/=  brightness     [/]  gamma     ,/.  saturation\n"
    @"1-4  160×120, 176×144, 320×240, 352×288     f  full screen";

@interface QCView : NSView
@property(nonatomic, copy) BOOL (^onKey)(NSEvent *);
@end

@implementation QCView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)keyDown:(NSEvent *)e {
    if (!self.onKey || !self.onKey(e)) [super keyDown:e];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
- (void)handleFrame:(const qc_frame *)f;
@end

static void frame_cb(void *ctx, const qc_frame *f) {
    [(__bridge AppDelegate *)ctx handleFrame:f];
}

@implementation AppDelegate {
    NSWindow *_window;
    QCView *_view;
    CALayer *_video, *_flash;
    NSTextField *_hud;
    NSTimer *_timer;
    qc_cam *_cam;
    int _size;
    BOOL _showInfo;
    volatile BOOL _mirror;

    pthread_mutex_t _lock;  // guards the last unmirrored frame, used for snapshots
    uint8_t *_last;
    int _lastW, _lastH;

    uint64_t _shown, _fpsFrames;
    CFTimeInterval _fpsTime;
    double _fps;
    NSString *_message;
    CFTimeInterval _messageUntil;
}

- (instancetype)init {
    if ((self = [super init])) {
        pthread_mutex_init(&_lock, NULL);
        _size = 3;
        _mirror = YES;
        _showInfo = YES;
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSRect frame = NSMakeRect(0, 0, 704, 576);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"QuickCam Express";
    _window.contentAspectRatio = NSMakeSize(352, 288);
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.releasedWhenClosed = NO;

    _view = [[QCView alloc] initWithFrame:frame];
    _view.wantsLayer = YES;
    _view.layer.backgroundColor = NSColor.blackColor.CGColor;
    __weak AppDelegate *weakSelf = self;
    _view.onKey = ^BOOL(NSEvent *e) {
        return [weakSelf handleKey:e];
    };

    _video = [CALayer layer];
    _video.frame = _view.bounds;
    _video.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    _video.contentsGravity = kCAGravityResizeAspect;
    _video.magnificationFilter = kCAFilterLinear;
    [_view.layer addSublayer:_video];

    _flash = [CALayer layer];
    _flash.frame = _view.bounds;
    _flash.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    _flash.backgroundColor = NSColor.whiteColor.CGColor;
    _flash.opacity = 0;
    [_view.layer addSublayer:_flash];

    _hud = [NSTextField wrappingLabelWithString:@""];
    _hud.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium];
    _hud.textColor = NSColor.whiteColor;
    _hud.drawsBackground = YES;
    _hud.backgroundColor = [NSColor colorWithWhite:0 alpha:0.55];
    _hud.translatesAutoresizingMaskIntoConstraints = NO;
    [_view addSubview:_hud];
    [NSLayoutConstraint activateConstraints:@[
        [_hud.leadingAnchor constraintEqualToAnchor:_view.leadingAnchor constant:8],
        [_hud.bottomAnchor constraintEqualToAnchor:_view.bottomAnchor constant:-8],
        [_hud.trailingAnchor constraintLessThanOrEqualToAnchor:_view.trailingAnchor constant:-8],
    ]];

    _window.contentView = _view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];

    [self openCamera];
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                             target:self
                                           selector:@selector(tick)
                                           userInfo:nil
                                            repeats:YES];
    [self tick];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)n {
    qc_close(_cam);  // stops streaming and turns the LED off
    _cam = NULL;
}

#pragma mark - camera

- (BOOL)openCamera {
    char err[256];
    _cam = qc_open(err, sizeof err);
    if (_cam && qc_start(_cam, kSizes[_size][0], kSizes[_size][1], frame_cb, (__bridge void *)self,
                         err, sizeof err)) {
        qc_close(_cam);
        _cam = NULL;
    }
    if (!_cam) {
        [self showMessage:[NSString stringWithFormat:@"Waiting for camera: %s", err] seconds:1e9];
        return NO;
    }
    _message = nil;
    _fpsFrames = _shown;
    _fpsTime = CACurrentMediaTime();
    return YES;
}

- (void)cameraLost {
    qc_close(_cam);
    _cam = NULL;
    [self showMessage:@"Camera disconnected, waiting for it to come back…" seconds:1e9];
}

- (void)setSize:(int)size {
    _size = size;
    _window.contentAspectRatio = NSMakeSize(kSizes[size][0], kSizes[size][1]);
    if (!_cam) return;
    char err[256];
    if (qc_start(_cam, kSizes[size][0], kSizes[size][1], frame_cb, (__bridge void *)self, err, sizeof err))
        [self showMessage:[NSString stringWithUTF8String:err] seconds:4];
    _fpsFrames = _shown;
    _fpsTime = CACurrentMediaTime();
}

// USB thread.
- (void)handleFrame:(const qc_frame *)f {
    if (!f) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self cameraLost];
        });
        return;
    }
    int w = f->width, h = f->height;
    size_t n = (size_t)w * h * 4;

    pthread_mutex_lock(&_lock);
    if (w != _lastW || h != _lastH) {
        free(_last);
        _last = malloc(n);
        _lastW = w;
        _lastH = h;
    }
    memcpy(_last, f->rgbx, n);
    pthread_mutex_unlock(&_lock);

    NSMutableData *buf = [NSMutableData dataWithLength:n];
    uint32_t *dst = buf.mutableBytes;
    const uint32_t *src = (const uint32_t *)f->rgbx;
    if (_mirror) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) dst[y * w + x] = src[y * w + (w - 1 - x)];
    } else {
        memcpy(dst, src, n);
    }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)buf);
    CGImageRef img = CGImageCreate(w, h, 8, 32, (size_t)w * 4, cs,
                                   kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);

    BOOL button = f->button;
    dispatch_async(dispatch_get_main_queue(), ^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self->_video.contents = (__bridge id)img;
        [CATransaction commit];
        CGImageRelease(img);
        self->_shown++;
        if (button) [self snapshot];
    });
}

#pragma mark - UI

- (void)snapshot {
    pthread_mutex_lock(&_lock);
    int w = _lastW, h = _lastH;
    NSData *pixels = _last ? [NSData dataWithBytes:_last length:(size_t)w * h * 4] : nil;
    pthread_mutex_unlock(&_lock);
    if (!pixels) return;

    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Pictures/QuickCam"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
    NSDateFormatter *fmt = [NSDateFormatter new];
    fmt.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *name = [NSString stringWithFormat:@"QuickCam %@.png", [fmt stringFromDate:[NSDate date]]];
    NSString *path = [dir stringByAppendingPathComponent:name];
    for (int i = 2; [NSFileManager.defaultManager fileExistsAtPath:path]; i++)
        path = [dir stringByAppendingPathComponent:
                        [NSString stringWithFormat:@"%@ %d.png", name.stringByDeletingPathExtension, i]];

    if (qc_write_png(path.fileSystemRepresentation, pixels.bytes, w, h)) {
        [self showMessage:@"Could not save the snapshot" seconds:3];
        return;
    }
    CABasicAnimation *a = [CABasicAnimation animationWithKeyPath:@"opacity"];
    a.fromValue = @0.8;
    a.toValue = @0;
    a.duration = 0.35;
    [_flash addAnimation:a forKey:@"flash"];
    [self showMessage:[@"Saved " stringByAppendingString:path.stringByAbbreviatingWithTildeInPath] seconds:3];
}

- (void)showMessage:(NSString *)msg seconds:(double)s {
    _message = msg;
    _messageUntil = CACurrentMediaTime() + s;
    [self updateHud];
}

- (void)tick {
    if (!_cam) [self openCamera];
    CFTimeInterval now = CACurrentMediaTime();
    if (now - _fpsTime >= 1.0) {
        _fps = (_shown - _fpsFrames) / (now - _fpsTime);
        _fpsFrames = _shown;
        _fpsTime = now;
    }
    [self updateHud];
}

- (void)updateHud {
    NSMutableArray *lines = [NSMutableArray array];
    if (_message && CACurrentMediaTime() < _messageUntil) [lines addObject:_message];
    if (_showInfo && _cam) {
        qc_controls k;
        qc_get_controls(_cam, &k);
        [lines addObject:[NSString stringWithFormat:@"%d×%d  %.1f fps  exposure %d  gain %d %@  "
                                                    @"WB %@ (r %.2f b %.2f)  γ %.1f  sat %.1f  brightness %d",
                                                    kSizes[_size][0], kSizes[_size][1], _fps, k.exposure,
                                                    k.gain, k.auto_exposure ? @"(auto)" : @"(manual)",
                                                    k.auto_white_balance ? @"auto" : @"off", k.gain_r,
                                                    k.gain_b, k.gamma, k.saturation, k.brightness]];
        if (_shown < 60 && !_message) [lines addObject:@"press ? for keyboard shortcuts"];
    }
    _hud.stringValue = [lines componentsJoinedByString:@"\n"];
    _hud.hidden = lines.count == 0;
}

- (BOOL)handleKey:(NSEvent *)e {
    if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return NO;
    NSString *chars = e.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    switch (c) {
    case ' ':
    case 's': [self snapshot]; return YES;
    case 'm': _mirror = !_mirror; return YES;
    case 'i': _showInfo = !_showInfo; [self updateHud]; return YES;
    case 'f': [_window toggleFullScreen:nil]; return YES;
    case '?':
    case 'h': [self showMessage:kHelp seconds:8]; return YES;
    case '1':
    case '2':
    case '3':
    case '4': [self setSize:c - '1']; return YES;
    }
    if (!_cam) return NO;
    qc_controls k;
    qc_get_controls(_cam, &k);
    switch (c) {
    case 'a': qc_set_auto_exposure(_cam, !k.auto_exposure); break;
    case 'w': qc_set_auto_white_balance(_cam, !k.auto_white_balance); break;
    case NSUpArrowFunctionKey: qc_set_exposure(_cam, (int)(k.exposure * 1.15) + 1); break;
    case NSDownArrowFunctionKey: qc_set_exposure(_cam, (int)(k.exposure / 1.15)); break;
    case NSRightArrowFunctionKey: qc_set_gain(_cam, k.gain + 8); break;
    case NSLeftArrowFunctionKey: qc_set_gain(_cam, k.gain - 8); break;
    case ']': qc_set_gamma(_cam, k.gamma + 0.1f); break;
    case '[': qc_set_gamma(_cam, k.gamma - 0.1f); break;
    case '=':
    case '+': qc_set_brightness(_cam, k.brightness + 4); break;
    case '-': qc_set_brightness(_cam, k.brightness - 4); break;
    case '.': qc_set_saturation(_cam, k.saturation + 0.1f); break;
    case ',': qc_set_saturation(_cam, k.saturation - 0.1f); break;
    default: return NO;
    }
    [self updateHud];
    return YES;
}

@end

static void build_menu(void) {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Hide qcview" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit qcview" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem *winItem = [NSMenuItem new];
    [bar addItem:winItem];
    NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [winMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    NSMenuItem *fs = [winMenu addItemWithTitle:@"Enter Full Screen"
                                        action:@selector(toggleFullScreen:)
                                 keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    winItem.submenu = winMenu;
    NSApp.mainMenu = bar;
}

int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        build_menu();

        // Ctrl-C in the terminal: quit cleanly so the camera is stopped.
        static dispatch_source_t sources[2];
        int sigs[2] = {SIGINT, SIGTERM};
        for (int i = 0; i < 2; i++) {
            signal(sigs[i], SIG_IGN);
            sources[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sigs[i], 0, dispatch_get_main_queue());
            dispatch_source_set_event_handler(sources[i], ^{
                [NSApp terminate:nil];
            });
            dispatch_resume(sources[i]);
        }
        [app run];
    }
    return 0;
}

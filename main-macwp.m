// Cocoa desktop-wallpaper front-end -> bin/goo-macwp. The macos analogue of
// main-wlwp.c: runs the sim as the desktop wallpaper via a borderless NSWindow
// parked at the desktop window level, behind every app window and joined to all
// Spaces, with mouse events passing straight through to the desktop. On
// SIGINT/SIGTERM it closes the windows cleanly, so the normal desktop picture
// reappears -- no save/restore needed.
//
// -m selects the monitor (-1 = clone the same sim onto every monitor, stretched to
// each; -2 = same clone but sized to the largest monitor and centre-cropped 1:1 per
// monitor instead of stretched, so mismatched-aspect monitors don't distort).
// Cloning is one sim + one GL context shared across N windows: sim_step runs once,
// then we re-point the single context at each window (setView) and present the same
// field. One context means the VAO / texture bindings stay valid -- N independent
// sims would need N processes (the sim core is a singleton), which we don't want here.
//
// Not RGFW: a desktop-level all-Spaces click-through window needs NSWindow knobs
// (level / collectionBehavior / ignoresMouseEvents) RGFW doesn't expose, so this
// owns its own window + GL context + loop, same split as the wayland front-end.

#include "sim.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <gl.h> // glad declarations (impl emitted by sim.c)

#import <Cocoa/Cocoa.h>

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static void die(const char *msg) {
    fprintf(stderr, "goo-macwp: %s\n", msg);
    exit(EXIT_FAILURE);
}

// Borderless desktop-level window covering one screen, no input. Same recipe for
// every monitor in clone mode; the view is the GL drawable the shared context
// presents into.
static NSWindow *make_wallpaper_window(NSScreen *screen, NSView **out_view) {
    NSRect frame = [screen frame];
    // screen:nil -- with a screen passed, contentRect is interpreted relative to that
    // screen's origin, double-offsetting a window whose frame is already global (the
    // secondary lands at 2x its origin). nil uses the global frame as-is.
    NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO
                                                   screen:nil];
    // Desktop level: sit just under the desktop icons so the wallpaper renders
    // behind everything. NOTE: kCGDesktopWindowLevel paints over the icons on some
    // os versions; switch to kCGDesktopIconWindowLevelKey-1 if you want them visible.
    [win setLevel:CGWindowLevelForKey(kCGDesktopWindowLevelKey)];
    // Show on every Space, don't move/cycle with the user, ignore mission-control cycle.
    [win setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorStationary |
                               NSWindowCollectionBehaviorIgnoresCycle];
    [win setIgnoresMouseEvents:YES]; // clicks fall through to the desktop, like wlwp's no-input surface
    [win setOpaque:YES];
    [win setHasShadow:NO];
    [win setReleasedWhenClosed:NO];

    // wantsBestResolutionOpenGLSurface + (later) setView are AppKit-deprecated in
    // favour of NSOpenGLView, but the whole NSOpenGL path is deprecated anyway
    // (GL_SILENCE_DEPRECATION mutes the rest) -- silence these rather than restructure.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSView *view = [[NSView alloc] initWithFrame:frame];
    [view setWantsBestResolutionOpenGLSurface:YES]; // retina: backing store in device px
#pragma clang diagnostic pop
    [win setContentView:view];
    *out_view = view;
    return win;
}

// Backing-store size (device px) of a screen -- the present target resolution.
// NOTE: read from the screen, not the view: a view's convertRectToBacking reports
// the wrong scale until its window is realized on the target display, so a 1x
// secondary would inherit the 2x main scale and present at double size.
static void screen_backing_px(NSScreen *s, int *w, int *h) {
    NSRect f = [s frame];
    double k = [s backingScaleFactor];
    *w = (int)(f.size.width * k);
    *h = (int)(f.size.height * k);
}

int main(int argc, char **argv) {
    parse_args(argc, argv, true, true); // wlwp=true (wallpaper), macwp=true (exposes -m)

    @autoreleasepool {
        [NSApplication sharedApplication];
        // Accessory: no dock icon, no menu bar -- we're a background renderer, not an app.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSArray<NSScreen *> *all = [NSScreen screens];
        if ([all count] == 0)
            die("no screens");

        // Pick the target screens: negative clones onto every monitor; otherwise a
        // single monitor by index (0 = main), clamped into range. -2 additionally
        // switches the per-monitor present from stretch to centre-crop (see below).
        bool crop_mode = (whichMonitor == -2);
        NSMutableArray<NSScreen *> *screens = [NSMutableArray array];
        if (whichMonitor < 0) {
            [screens addObjectsFromArray:all];
        } else {
            NSUInteger idx = (NSUInteger)whichMonitor < [all count] ? (NSUInteger)whichMonitor : 0;
            [screens addObject:[all objectAtIndex:idx]];
        }
        NSUInteger nwin = [screens count];

        // One window + view per target screen; a single shared GL context drives them all.
        NSWindow **windows = calloc(nwin, sizeof(NSWindow *));
        NSView **views = calloc(nwin, sizeof(NSView *));
        if (!windows || !views)
            die("out of memory");
        for (NSUInteger i = 0; i < nwin; i++)
            windows[i] = make_wallpaper_window([screens objectAtIndex:i], &views[i]);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSOpenGLPixelFormatAttribute attrs[] = {
            // 4.1 core: macos caps here, and it's the highest that still runs our
            // "#version 330 core" shaders (matches the RGFW front-end's request).
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFAAccelerated,
            0,
        };
        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf)
            die("no 4.1-core pixel format");
        NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
        if (!ctx)
            die("NSOpenGLContext failed");
        [ctx setView:views[0]]; // bootstrap on the first screen; present re-points per frame
#pragma clang diagnostic pop
        [ctx makeCurrentContext];
        GLint swap = vsync ? 1 : 0;
        [ctx setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];

        if (!gladLoaderLoadGL()) // dlopens OpenGL.framework; no NSOpenGL getProcAddress needed
            die("gladLoaderLoadGL failed");

        // Context-level GL state (same triplet as the other front-ends).
        glEnable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Sim is sized to the first target screen (the reference). In clone mode the
        // same field upscales to each monitor's own resolution. NOTE: monitors of a
        // different size/aspect get the field stretched, not re-simulated; and no
        // resize/display-reconfig handling -- restart goo-macwp if the layout changes.
        NSScreen *ref = [screens objectAtIndex:0];
        NSRect ref_frame = [ref frame];
        int logical_w = (int)ref_frame.size.width;
        int logical_h = (int)ref_frame.size.height;
        int fb_w, fb_h;
        screen_backing_px(ref, &fb_w, &fb_h);
        if (crop_mode) {
            // Crop mode: size the shared field to the componentwise-max monitor dims
            // (largest width and largest height found -- possibly from different
            // monitors), so every monitor fits inside it and shows a centred 1:1 crop
            // rather than a stretch. fb only seeds the upscale target here; the present
            // loop overwrites window_/screenBuffer size per monitor anyway.
            double max_w = 0, max_h = 0;
            for (NSScreen *s in screens) {
                NSRect f = [s frame];
                if (f.size.width > max_w) max_w = f.size.width;
                if (f.size.height > max_h) max_h = f.size.height;
            }
            logical_w = (int)max_w;
            logical_h = (int)max_h;
            double k = [ref backingScaleFactor];
            fb_w = (int)(max_w * k);
            fb_h = (int)(max_h * k);
        }
        sim_set_dims(logical_w, logical_h, fb_w, fb_h);

        sim_setup();

        // orderFront, not makeKeyAndOrderFront -- a wallpaper never takes key focus.
        for (NSUInteger i = 0; i < nwin; i++)
            [windows[i] orderFront:nil];
        [NSApp finishLaunching]; // we drive our own loop below instead of [NSApp run]

        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);

        // Mouse repel: ignoresMouseEvents means no pointer events, so poll the global
        // cursor each frame (no input steal) and feed its per-frame velocity, same as
        // the other front-ends. The cursor maps from whichever target screen it's over
        // into the shared sim coords, so repel works on every cloned monitor.
        float prev_mouse[2] = {-1e9f, -1e9f};
        NSUInteger prev_hit = (NSUInteger)-1; // which screen the cursor was on last frame

        int epoch_counter = 0;
        while (running) {
            if (max_iterations > 0 && epoch_counter >= max_iterations)
                break;

            @autoreleasepool {
                // Pump pending events non-blocking so the windows stay live.
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES]))
                    [NSApp sendEvent:ev];

                float mouse_position[2] = {-1e9f, -1e9f};
                float mouse_velocity[2] = {0.0f, 0.0f};
                if (!no_mouse) {
                    NSPoint m = [NSEvent mouseLocation]; // screen coords, bottom-left origin
                    // Find the target screen under the cursor. In clone mode every monitor
                    // shows the same field, so map the cursor as a fraction of whichever
                    // screen it's on into the shared sim coords -- not just the reference.
                    NSRect hit = ref_frame;
                    bool in = false;
                    NSUInteger hit_idx = (NSUInteger)-1;
                    for (NSUInteger i = 0; i < nwin; i++) {
                        NSRect f = [[screens objectAtIndex:i] frame];
                        if (m.x >= f.origin.x && m.x < f.origin.x + f.size.width &&
                            m.y >= f.origin.y && m.y < f.origin.y + f.size.height) {
                            hit = f;
                            in = true;
                            hit_idx = i;
                            break;
                        }
                    }
                    if (in) {
                        // Cursor -> shared logical coords, matching the visual mapping:
                        //  - stretch (-1): fraction within the hit screen scaled to the field.
                        //  - crop (-2): 1:1, offset by this monitor's centred crop origin so
                        //    the cursor lands on the same particle it visually hovers.
                        double lx, ly_bottom;
                        if (crop_mode) {
                            lx = (logical_w - hit.size.width) * 0.5 + (m.x - hit.origin.x);
                            ly_bottom = (logical_h - hit.size.height) * 0.5 + (m.y - hit.origin.y);
                        } else {
                            lx = (m.x - hit.origin.x) / hit.size.width * logical_w;
                            ly_bottom = (m.y - hit.origin.y) / hit.size.height * logical_h;
                        }
                        mouse_position[0] = (float)lx * mouse_scale;
                        // sim space is top-origin; cocoa screen y is bottom-up -- flip.
                        mouse_position[1] = (float)(logical_h - ly_bottom) * mouse_scale;
                        // velocity only if the previous frame was on the same screen -- else
                        // the delta is a teleport: either re-entry from the parked sentinel,
                        // or a screen-cross (every monitor clones the field, so the fraction
                        // snaps from one screen edge to the opposite edge of the shared sim).
                        if (prev_mouse[0] > -1e8f && hit_idx == prev_hit) {
                            mouse_velocity[0] = mouse_position[0] - prev_mouse[0];
                            mouse_velocity[1] = mouse_position[1] - prev_mouse[1];
                        }
                    }
                    prev_hit = hit_idx;
                }
                prev_mouse[0] = mouse_position[0];
                prev_mouse[1] = mouse_position[1];

                sim_step(epoch_counter, mouse_position, mouse_velocity);

                // Present the one field to every target screen: size the upscale to that
                // monitor's backing store and swap. Single window = the common case, where
                // the view is set once at bootstrap, so skip the per-frame setView/update.
                for (NSUInteger i = 0; i < nwin; i++) {
                    if (nwin > 1) { // re-point the shared context only when cloning
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                        [ctx setView:views[i]]; // setView deprecated (NSOpenGLView); see make_wallpaper_window
#pragma clang diagnostic pop
                        [ctx update]; // drawable changed; refresh the context's view binding
                    }
                    NSScreen *s = [screens objectAtIndex:i];
                    if (crop_mode) {
                        // Show only the centred fraction of the field that matches this
                        // monitor's own size, at 1:1 (fx,fy <= 1 since the field is sized
                        // to the max monitor). Identical points-per-pixel as the reference
                        // -> no stretch; smaller monitors just see less of the field.
                        NSRect f = [s frame];
                        double fx = f.size.width / (double)logical_w;
                        double fy = f.size.height / (double)logical_h;
                        if (fx > 1.0) fx = 1.0;
                        if (fy > 1.0) fy = 1.0;
                        present_uv_min[0] = (float)((1.0 - fx) * 0.5);
                        present_uv_min[1] = (float)((1.0 - fy) * 0.5);
                        present_uv_max[0] = (float)((1.0 + fx) * 0.5);
                        present_uv_max[1] = (float)((1.0 + fy) * 0.5);
                    }
                    int pw, ph;
                    screen_backing_px(s, &pw, &ph);
                    window_width = pw;
                    window_height = ph;
                    screenBuffer.width = pw;
                    screenBuffer.height = ph;
                    sim_present();
                    [ctx flushBuffer]; // swap; vsync (swap interval) paces the loop
                }
                epoch_counter++;
            }
        }

        // Clean teardown -> the desktop picture underneath repaints.
        glBindVertexArray(0);
        glDeleteTextures(9, textures);
        glDeleteFramebuffers(9, framebuffers);
        [NSOpenGLContext clearCurrentContext];
        for (NSUInteger i = 0; i < nwin; i++)
            [windows[i] close];
        free(windows);
        free(views);
    }
    return 0;
}

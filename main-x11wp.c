// RGFW front-end -> bin/goo-x11wp. x11 animated desktop wallpaper.
//
// A borderless x11 window marked _NET_WM_WINDOW_TYPE_DESKTOP: an EWMH wm pins it at the wallpaper
// layer, so the window IS the wallpaper -- no extension. Main target: gnome-wayland via XWayland.
//
// usage: goo-x11wp [--mouse] [goo options]   (--mouse: cursor repel; disables click-through)
//
// The simulation itself lives in sim.c (see sim.h).

#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl.h>

#define RGFW_OPENGL
#define RGFWDEF // implementation lives in rgfw_impl.c
#include <RGFW.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define WIN_TITLE "goo-x11wp"

static RGFW_window *window;
static i32 g_fbw, g_fbh;            // initial framebuffer size in device px, captured at map
static bool g_interactive = false; // --mouse: grab pointer for the repel; else click-through, no repel

//============================================================
// window
//============================================================

// Mark the window _NET_WM_WINDOW_TYPE_DESKTOP (+ skip taskbar/pager, sticky). Must run before map.
// No-op off x11.
static void set_desktop_window_type(void) {
    Display *dpy = (Display *)RGFW_getDisplay_X11();
    if (!dpy)
        return;
    Window win = (Window)RGFW_window_getWindow_X11(window);
    if (!win)
        return;

    Atom desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);

    Atom state[] = {
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
        XInternAtom(dpy, "_NET_WM_STATE_STICKY", False),
    };
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)state, 3);
    XFlush(dpy);
}

// glad + context GL state. Split out so gl_refresh can rebuild it after recreating the context.
static void gl_load_state(void) {
    if (!gladLoadGL((GLADloadfunc)RGFW_getProcAddress_OpenGL)) {
        fprintf(stderr, "goo-x11wp: failed to initialize GLAD\n");
        exit(EXIT_FAILURE);
    }
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void window_setup(void) {
    RGFW_init();

    // core 4.1 (matches main.c; runs the "#version 330 core" shaders).
    RGFW_glHints *hints = RGFW_getGlobalHints_OpenGL();
    hints->major = 4;
    hints->minor = 1;
    hints->profile = RGFW_glCore;

    // borderless, no focus grab, created hidden so we can set the desktop type before map.
    RGFW_windowFlags flags =
        RGFW_windowOpenGL | RGFW_windowNoBorder | RGFW_windowNoFocusOnCreate | RGFW_windowHide;

    // NOTE: monitor 0 only; multi-monitor = one window per output.
    size_t monitorCount;
    RGFW_monitor **monitors = RGFW_getMonitors(&monitorCount);
    if (monitorCount == 0) {
        fprintf(stderr, "goo-x11wp: no monitors found\n");
        exit(EXIT_FAILURE);
    }
    RGFW_monitor *monitor = monitors[0];
    width = monitor->mode.w;
    height = monitor->mode.h;

    window = RGFW_createWindow(WIN_TITLE, monitor->x, monitor->y, width, height, flags);
    if (!window) {
        fprintf(stderr, "goo-x11wp: failed to create window\n");
        exit(EXIT_FAILURE);
    }

    RGFW_window_swapInterval_OpenGL(window, 0); // free-run; --fps caps the spin

    set_desktop_window_type(); // type before map, then show
    RGFW_window_show(window);

    if (!g_interactive) {
        // click-through: clear the input region so desktop clicks pass through. --mouse skips it
        // (goo needs the pointer for the repel). NOTE: goo renders over the desktop icons.
        RGFW_window_setMousePassthrough(window, RGFW_TRUE);
    }

    // Initial drawable, device px (getSize; getSizeInPixels lies on fractional scale). The loop
    // re-reads it -- the wm may resize us a frame or two after map.
    i32 fbw, fbh;
    RGFW_window_getSize(window, &fbw, &fbh);
    g_fbw = fbw;
    g_fbh = fbh;
    printf("goo-x11wp: monitor 0 mode=%dx%d initial-drawable=%dx%d\n", monitor->mode.w, monitor->mode.h, fbw, fbh);
    sim_set_dims(fbw, fbh, fbw, fbh);

    gl_load_state();
}

// Recreate the GL context to bound the freedreno per-submit leak (see goo-wlwp --gl-refresh).
// Snapshot the sim, remake the context on the same window, rebuild. One frame's hitch.
static void gl_refresh(void) {
    SimSnapshot *snap = sim_snapshot();
    sim_teardown_cpu();
    RGFW_window_deleteContext_OpenGL(window, RGFW_window_getContext_OpenGL(window));
    RGFW_window_createContext_OpenGL(window, RGFW_getGlobalHints_OpenGL());
    RGFW_window_makeCurrentContext_OpenGL(window);
    RGFW_window_swapInterval_OpenGL(window, 0); // a fresh context defaults to vsync on
    gl_load_state();
    sim_restore(snap);
    sim_setup();
    sim_snapshot_free(snap);
}

// Global X pointer, device px relative to our drawable (caller scales to sim space). False when
// off our monitor.
static bool poll_mouse_px(float out[2]) {
    Display *dpy = (Display *)RGFW_getDisplay_X11();
    if (!dpy)
        return false;
    Window win = (Window)RGFW_window_getWindow_X11(window);
    Window root, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (!XQueryPointer(dpy, win, &root, &child, &rx, &ry, &wx, &wy, &mask))
        return false;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr) || attr.width <= 0 || attr.height <= 0)
        return false;
    if (wx < 0 || wy < 0 || wx >= attr.width || wy >= attr.height)
        return false;
    out[0] = (float)wx * ((float)window_width / attr.width);
    out[1] = (float)wy * ((float)window_height / attr.height);
    return true;
}

//============================================================
// main
//============================================================

int main(int argc, char **argv) {
    // These flags aren't in wlwp mode's option table; consume + strip before parse_args.
    // --mouse: interactive repel. *-debug: 1:1 check overlays.
    for (int i = 1; i < argc; i++) {
        bool strip = false;
        if (strcmp(argv[i], "--corners-debug") == 0)
            strip = corners_debug = true;
        else if (strcmp(argv[i], "--mouse-debug") == 0)
            strip = mouse_debug = true;
        else if (strcmp(argv[i], "--mouse") == 0)
            strip = g_interactive = true;
        if (strip) {
            for (int j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            i--;
        }
    }

    // wlwp=true reuses the wallpaper flag set. Side effect: --help says "goo-wlwp". cosmetic.
    parse_args(argc, argv, true, false);

    title = WIN_TITLE;
    if (fps_cap <= 0)
        fps_cap = 60; // soft cap so free-run doesn't spin the gpu

    window_setup();
    sim_setup();

    // Provisional present target (the loop re-reads getSize each frame and resizes on change).
    window_width = g_fbw;
    window_height = g_fbh;
    screenBuffer.width = g_fbw;
    screenBuffer.height = g_fbh;

    int epoch_counter = 0;
    double frame_prev = get_time();
    double frame_ms = 0.0;
    double fps_sleep_ms = 0.0;

    // --gl-refresh: bound the freedreno leak (default 30m, shared with goo-wlwp; 0 disables).
    double last_refresh = get_time();

    // Mouse repel: global cursor -> sim space + velocity. Parked when off-monitor / not --mouse.
    float prev_mouse[2] = {-1e9f, -1e9f};

    while (!RGFW_window_shouldClose(window)) {
        if (max_iterations > 0 && epoch_counter >= max_iterations)
            break; // -N: bounded run for testing/benchmarks

        if (gl_refresh_seconds > 0 && get_time() - last_refresh >= gl_refresh_seconds) {
            gl_refresh();
            last_refresh = get_time();
        }

        // the wm may resize us after map; resize the sim to the real drawable (fill, not crop).
        {
            i32 fbw, fbh;
            RGFW_window_getSize(window, &fbw, &fbh);
            if (fbw > 1 && fbh > 1 && (fbw != window_width || fbh != window_height)) {
                sim_resize(fbw, fbh, fbw, fbh);
                window_width = fbw;
                window_height = fbh;
                screenBuffer.width = fbw;
                screenBuffer.height = fbh;
            }
        }

        // poll only in --mouse mode: with passthrough on, XQueryPointer is stale (dead-spot bubble).
        float mouse_px[2] = {-1e9f, -1e9f};
        bool mouse_on = g_interactive && !no_mouse && poll_mouse_px(mouse_px);
        float mouse_position[2] = {-1e9f, -1e9f};
        float mouse_velocity[2] = {0.0f, 0.0f};
        if (mouse_on) {
            mouse_position[0] = mouse_px[0] * mouse_scale;
            mouse_position[1] = mouse_px[1] * mouse_scale;
            if (prev_mouse[0] > -1e8f) { // skip the re-entry teleport delta
                mouse_velocity[0] = mouse_position[0] - prev_mouse[0];
                mouse_velocity[1] = mouse_position[1] - prev_mouse[1];
            }
        }
        prev_mouse[0] = mouse_position[0];
        prev_mouse[1] = mouse_position[1];

        sim_step(epoch_counter, mouse_position, mouse_velocity);
        sim_present();

        // --corners-debug: green squares at the 4 corners -- a missing one = viewport overshoot.
        if (corners_debug) {
            const int s = 40; // square side, px
            const int W = window_width, H = window_height;
            const int corner[4][2] = {{0, 0}, {W - s, 0}, {0, H - s}, {W - s, H - s}};
            glEnable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
            for (int c = 0; c < 4; c++) {
                glScissor(corner[c][0], corner[c][1], s, s);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // restore so the next frame's clears stay black
        }

        // --mouse-debug cursor dot + motion line now render in the shared debug overlay pass
        // (sim_step -> debugShader), alongside the exclusion/edge fills.

        double frame_now = get_time();
        frame_ms = ema(frame_ms, (frame_now - frame_prev) * 1000.0, 0.1);
        frame_prev = frame_now;
        if (epoch_counter % 120 == 0)
            fprintf(stdout, "frame=%.2f ms (%.0f fps)\n", frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0);

        glFinish(); // freedreno: force tile resolve before commit (else the compositor samples stale tiles)
        RGFW_window_swapBuffers_OpenGL(window);

        RGFW_event event; // drain events (responsive; getSize picks up resizes)
        while (RGFW_window_checkEvent(window, &event)) {
        }

        if (fps_cap > 0) { // --fps soft cap (proportional pacer)
            double target_ms = 1000.0 / fps_cap;
            fps_sleep_ms += (target_ms - frame_ms) * 0.5;
            if (fps_sleep_ms < 0.0)
                fps_sleep_ms = 0.0;
            sleep_ms(fps_sleep_ms);
        }
        epoch_counter++;
    }

    glBindVertexArray(0);
    glDeleteTextures(9, textures);
    glDeleteFramebuffers(9, framebuffers);
    RGFW_window_close(window);
    RGFW_deinit();
    return 0;
}

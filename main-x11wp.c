// RGFW front-end -> bin/goo-x11wp. x11 animated desktop wallpaper.
//
// Runs as a single borderless x11 window (RGFW x11 backend, same as bin/goo) marked
// _NET_WM_WINDOW_TYPE_DESKTOP (+ skip taskbar/pager, sticky): any EWMH wm pins it at the wallpaper
// layer, sticky, ignored by autotilers. So the window IS the wallpaper -- no extension, no install.
// Primary target is gnome-wayland under XWayland (where layer-shell / goo-wlwp isn't available);
// also runs on native x11. Not wayland-native on purpose: mutter mis-scales a native surface.
//
// usage:
//   goo-x11wp [--mouse] [goo options]   # --mouse: cursor repel (grabs pointer; no click-through)
//
// The simulation itself lives in sim.c (see sim.h).

#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl.h>
#include "shader.h" // debugShader for --mouse-debug

#define RGFW_OPENGL
#define RGFWDEF // implementation lives in rgfw_impl.c
#include <RGFW.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define WIN_TITLE "goo-x11wp"

static RGFW_window *window;
static i32 g_fbw, g_fbh;            // initial framebuffer size in device px, captured at map
static bool g_interactive = false; // --mouse: grab pointer input for the repel; else click-through, no repel

//============================================================
// window
//============================================================

// Mark our window _NET_WM_WINDOW_TYPE_DESKTOP (+ skip taskbar/pager, sticky): an EWMH wm pins it at
// the wallpaper layer, sticky across workspaces, ignored by autotilers. Must run before map -- the
// wm reads the type then -- so the window is created hidden and shown after. No-op off x11.
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

// glad + context-level GL state. Split out so gl_refresh can rebuild it after recreating the context.
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

    // core 4.1 context (matches main.c; runs the "#version 330 core" shaders).
    RGFW_glHints *hints = RGFW_getGlobalHints_OpenGL();
    hints->major = 4;
    hints->minor = 1;
    hints->profile = RGFW_glCore;

    // borderless, no focus grab, created hidden so we can set the desktop type before map.
    RGFW_windowFlags flags =
        RGFW_windowOpenGL | RGFW_windowNoBorder | RGFW_windowNoFocusOnCreate | RGFW_windowHide;

    // NOTE: single-monitor (monitor 0). multi-monitor = one window per output; add when asked.
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

    RGFW_window_swapInterval_OpenGL(window, 0); // free-run (vsync off); --fps caps the spin

    // set the desktop type, THEN map -- goo is the wallpaper directly.
    set_desktop_window_type();
    RGFW_window_show(window);

    if (!g_interactive) {
        // click-through: clear the input region so desktop clicks fall through. --mouse skips it so
        // goo gets the pointer for the repel (an empty input region also blinds XQueryPointer).
        // NOTE: goo renders over the shell's desktop icons; a window can't sit below that layer.
        RGFW_window_setMousePassthrough(window, RGFW_TRUE);
    }

    // Initial drawable (device px). getSize is the true gl drawable here (getSizeInPixels inflates
    // on fractional XWayland). Provisional -- mutter resizes us a frame or two after map, so the
    // loop re-reads getSize and resizes the sim to match.
    i32 fbw, fbh;
    RGFW_window_getSize(window, &fbw, &fbh);
    g_fbw = fbw;
    g_fbh = fbh;
    printf("goo-x11wp: monitor 0 mode=%dx%d initial-drawable=%dx%d\n", monitor->mode.w, monitor->mode.h, fbw, fbh);
    sim_set_dims(fbw, fbh, fbw, fbh);

    gl_load_state();
}

// Recreate the GL context to reclaim the freedreno per-submit leak (see debug/, goo-wlwp's
// --gl-refresh). Snapshot the sim (it lives in GL objects), delete+remake the context on the same
// window (no remap/flash), rebuild the sim. Costs one frame's hitch.
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

// Global X pointer in device px relative to our drawable (caller applies mouse_scale for sim space,
// like main.c). False when off our monitor / another screen. window_width/attr.width maps X coords
// to device px whatever XWayland reports, so fractional scale needs no special case.
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
    // --corners-debug / --mouse-debug / --mouse aren't in wlwp mode's option table (dropt would
    // reject them). Consume + strip before parse_args. --mouse: interactive (repel, goo on top, no
    // click-through). *-debug: green corners / cursor dot to check the sim maps 1:1.
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

    // wlwp=true reuses the wallpaper flag set. Side effect: --help says "goo-wlwp" and lists
    // --gl-refresh. cosmetic; give x11wp its own parse_args mode if it grows real flags.
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

    // --gl-refresh: periodic GL-context recreate to bound the freedreno leak (default 30m, shared
    // with goo-wlwp via gl_refresh_seconds; 0 disables).
    double last_refresh = get_time();

    // Mouse repel: global cursor -> sim space + per-frame velocity, like goo-wlwp. Parked ({-1e9,..})
    // when off-monitor / --no-mouse / not --mouse; velocity only when the prev frame was also on.
    float prev_mouse[2] = {-1e9f, -1e9f};

    while (!RGFW_window_shouldClose(window)) {
        if (max_iterations > 0 && epoch_counter >= max_iterations)
            break; // -N: bounded run for testing/benchmarks

        if (gl_refresh_seconds > 0 && get_time() - last_refresh >= gl_refresh_seconds) {
            gl_refresh();
            last_refresh = get_time();
        }

        // Track the real drawable: fractional-scaled XWayland resizes us a frame or two after map.
        // getSize is the true gl drawable; resize the sim on change so we fill it, not a crop.
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

        // Global cursor -> sim space. Only in --mouse mode: with passthrough on, XQueryPointer is
        // stale and would pin a repel bubble at a dead spot.
        float mouse_px[2] = {-1e9f, -1e9f};
        bool mouse_on = g_interactive && !no_mouse && poll_mouse_px(mouse_px);
        float mouse_position[2] = {-1e9f, -1e9f};
        float mouse_velocity[2] = {0.0f, 0.0f};
        if (mouse_on) {
            mouse_position[0] = mouse_px[0] * mouse_scale;
            mouse_position[1] = mouse_px[1] * mouse_scale;
            if (prev_mouse[0] > -1e8f) { // skip the teleport delta on re-entry from the sentinel
                mouse_velocity[0] = mouse_position[0] - prev_mouse[0];
                mouse_velocity[1] = mouse_position[1] - prev_mouse[1];
            }
        }
        prev_mouse[0] = mouse_position[0];
        prev_mouse[1] = mouse_position[1];

        sim_step(epoch_counter, mouse_position, mouse_velocity);
        sim_present();

        // --corners-debug: green squares at the 4 framebuffer corners -- a corner you don't see
        // means the assumed size overshoots the real buffer there. scissor+clear over FB 0.
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

        // --mouse-debug: green dot (+ trail) at where the sim sees the cursor -- confirms 1:1.
        static float debug_prev[2] = {0.0f, 0.0f};
        if (mouse_debug && mouse_on) {
            float win_shape[2] = {(float)window_width, (float)window_height};
            float positions[4] = {debug_prev[0], debug_prev[1], mouse_px[0], mouse_px[1]};
            shader_use(&debugShader);
            shader_set_uniform_vec(&debugShader, "window_shape", 2, win_shape);
            glUniform2fv(glGetUniformLocation(debugShader.program, "positions"), 2, positions);
            glDrawArrays(GL_LINES, 0, 2);
            glDrawArrays(GL_POINTS, 1, 1);
            debug_prev[0] = mouse_px[0];
            debug_prev[1] = mouse_px[1];
        }

        double frame_now = get_time();
        frame_ms = ema(frame_ms, (frame_now - frame_prev) * 1000.0, 0.1);
        frame_prev = frame_now;
        if (epoch_counter % 120 == 0)
            fprintf(stdout, "frame=%.2f ms (%.0f fps)\n", frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0);

        // freedreno (tiled GPU): the compositor can sample mid tile-resolve, leaving stale tiles.
        // glFinish forces resolve before commit. Serialises the frame -- fine at wallpaper fps.
        glFinish();
        RGFW_window_swapBuffers_OpenGL(window);

        RGFW_event event; // drain events (keeps the window responsive; getSize picks up WM resizes)
        while (RGFW_window_checkEvent(window, &event)) {
        }

        if (fps_cap > 0) { // --fps soft cap: same proportional pacer as main.c
            double target_ms = 1000.0 / fps_cap;
            fps_sleep_ms += (target_ms - frame_ms) * 0.5;
            if (fps_sleep_ms < 0.0)
                fps_sleep_ms = 0.0;
            sleep_ms(fps_sleep_ms);
        }
        epoch_counter++;
    }

    glBindVertexArray(0);
    glDeleteTextures(8, textures);
    glDeleteFramebuffers(8, framebuffers);
    RGFW_window_close(window);
    RGFW_deinit();
    return 0;
}

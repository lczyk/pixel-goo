// Wayland layer-shell front-end -> bin/goo-wlwp. Runs the sim as a desktop
// wallpaper: a wl_surface pinned to the BACKGROUND layer via wlr-layer-shell,
// covering the output, with no keyboard/pointer input. On SIGINT/SIGTERM it tears
// the surface down cleanly, so whatever drew the wallpaper before (swaybg, the
// compositor's own bg, ...) reappears -- no save/restore needed.
//
// Works on any wlroots compositor (sway, hyprland, river, wayfire, ...) and KDE.
// NOT gnome/mutter, which refuses layer-shell. There is no portable wallpaper
// protocol; layer-shell is the broadest one.

// NOTE: EGL must be included before gl.h (sim.h -> buffer.h pulls in glad's gl.h,
// which bundles its own KHR/khrplatform; including EGL afterwards breaks EGLAPIENTRYP).
#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "sim.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl.h> // glad declarations (impl emitted by sim.c)

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h" // layer-shell pulls in xdg types

// ---- wayland / egl state ----
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_output *output; // first output the compositor advertises
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_egl_window *egl_window;
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;

static int surf_w = 0, surf_h = 0; // current layer-surface size (logical px)
static bool configured = false;    // got the first layer-surface configure
static bool dirty = false;         // size changed since last applied -> resize on next frame
static volatile sig_atomic_t running = 1;

// Pointer state (surface-local logical px). The background layer only gets pointer
// events where nothing above it (window / panel) has the cursor -- i.e. over the
// bare desktop -- which is exactly when reacting to the mouse makes sense.
static double mouse_x = 0, mouse_y = 0;
static bool mouse_in = false;

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

// ---- layer surface ----
static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0 && h > 0 && ((int)w != surf_w || (int)h != surf_h)) {
        surf_w = (int)w;
        surf_h = (int)h;
        dirty = true;
    }
    configured = true;
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)data;
    (void)ls;
    running = 0; // compositor pulled the surface (output gone, etc.) -- exit cleanly
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
    .configure = ls_configure,
    .closed = ls_closed,
};

// ---- pointer (bound at wl_pointer v1: enter/leave/motion/button/axis) ----
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)p;
    (void)serial;
    (void)s;
    mouse_x = wl_fixed_to_double(x);
    mouse_y = wl_fixed_to_double(y);
    mouse_in = true;
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d;
    (void)p;
    (void)serial;
    (void)s;
    mouse_in = false;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)p;
    (void)t;
    mouse_x = wl_fixed_to_double(x);
    mouse_y = wl_fixed_to_double(y);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void)d;
    (void)p;
    (void)serial;
    (void)t;
    (void)b;
    (void)st;
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)d;
    (void)p;
    (void)t;
    (void)axis;
    (void)v;
}
static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
};

// ---- seat ----
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &ptr_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {
    (void)d;
    (void)s;
    (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

// ---- registry ----
static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && output == NULL) {
        // First output only. NOTE: single-monitor -- multi-output would need one
        // surface per output; add when someone runs goo-wlwp on a multi-head setup.
        output = wl_registry_bind(reg, name, &wl_output_interface, ver < 2 ? ver : 2);
    } else if (strcmp(iface, wl_seat_interface.name) == 0 && seat == NULL) {
        // Bind at v1 so wl_pointer is v1 -- matches the 5-event listener above (no
        // frame/axis_* members to keep in sync with newer pointer versions).
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_global_remove,
};

static void die(const char *msg) {
    fprintf(stderr, "goo-wlwp: %s\n", msg);
    exit(EXIT_FAILURE);
}

// Stand up the EGL desktop-GL context on the wayland surface and load glad.
static void egl_setup(void) {
    egl_display = eglGetDisplay((EGLNativeDisplayType)display);
    if (egl_display == EGL_NO_DISPLAY)
        die("eglGetDisplay failed");
    if (!eglInitialize(egl_display, NULL, NULL))
        die("eglInitialize failed");
    if (!eglBindAPI(EGL_OPENGL_API)) // desktop GL (shaders are #version 330 core), not GLES
        die("eglBindAPI(OPENGL) failed");

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(egl_display, cfg_attr, &config, 1, &n) || n == 0)
        die("no matching EGL config (desktop GL)");

    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attr);
    if (egl_context == EGL_NO_CONTEXT)
        die("eglCreateContext failed (no 3.3 core?)");

    egl_window = wl_egl_window_create(surface, surf_w, surf_h);
    if (!egl_window)
        die("wl_egl_window_create failed");
    egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)egl_window, NULL);
    if (egl_surface == EGL_NO_SURFACE)
        die("eglCreateWindowSurface failed");

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
        die("eglMakeCurrent failed");

    eglSwapInterval(egl_display, vsync ? 1 : 0);

    if (!gladLoadGL((GLADloadfunc)eglGetProcAddress))
        die("gladLoadGL failed");
}

int main(int argc, char **argv) {
    parse_args(argc, argv, true);

    display = wl_display_connect(NULL);
    if (!display)
        die("cannot connect to a wayland display (is WAYLAND_DISPLAY set?)");

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(display); // populate globals

    if (!compositor)
        die("compositor missing wl_compositor");
    if (!layer_shell)
        die("compositor has no wlr-layer-shell -- wallpaper mode needs a wlroots/KDE compositor (not gnome)");

    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "goo");
    zwlr_layer_surface_v1_add_listener(layer_surface, &ls_listener, NULL);
    // Cover the whole output: anchor all 4 edges, size 0,0 (compositor fills in the
    // output size), exclusive zone -1 (ignore other panels' reservations), no input.
    zwlr_layer_surface_v1_set_anchor(layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(surface);

    // Block until the first configure so we know the output size before creating the
    // egl window. dispatch (not roundtrip) so the configure callback actually runs.
    while (!configured && wl_display_dispatch(display) != -1)
        ;
    if (surf_w <= 0 || surf_h <= 0)
        die("layer surface configured with no size");
    dirty = false;

    egl_setup();

    // Wayland gives logical px; treat the framebuffer as logical (output scale 1).
    // NOTE: fractional/hidpi output scale ignored -- on a 2x output the wallpaper
    // renders at logical res and the compositor upscales. wire wl_output scale +
    // wp_fractional_scale here if it looks soft on hidpi.
    sim_set_dims(surf_w, surf_h, surf_w, surf_h);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    sim_setup();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // Mouse repel: track the pointer in sim space and feed its per-frame velocity,
    // same as the windowed front-end. Parked sentinel ({-1e9,..}) when the cursor is
    // off the wallpaper (over a window) or --no-mouse, so the repel never fires.
    float prev_mouse[2] = {-1e9f, -1e9f};

    int epoch_counter = 0;
    while (running) {
        if (max_iterations > 0 && epoch_counter >= max_iterations)
            break;

        // Drain pending wayland events (configure, closed, pointer) without blocking.
        if (wl_display_dispatch_pending(display) == -1)
            break;

        if (dirty) {
            wl_egl_window_resize(egl_window, surf_w, surf_h, 0, 0);
            sim_resize(surf_w, surf_h, surf_w, surf_h);
            dirty = false;
        }

        float mouse_position[2] = {-1e9f, -1e9f};
        float mouse_velocity[2] = {0.0f, 0.0f};
        if (!no_mouse && mouse_in) {
            mouse_position[0] = (float)mouse_x * mouse_scale;
            mouse_position[1] = (float)mouse_y * mouse_scale;
            // velocity only if the previous frame was also on the wallpaper -- else the
            // delta is the teleport from the parked sentinel (re-entry jump).
            if (prev_mouse[0] > -1e8f) {
                mouse_velocity[0] = mouse_position[0] - prev_mouse[0];
                mouse_velocity[1] = mouse_position[1] - prev_mouse[1];
            }
        }
        prev_mouse[0] = mouse_position[0];
        prev_mouse[1] = mouse_position[1];

        sim_step(epoch_counter, mouse_position, mouse_velocity);
        sim_present();
        // eglSwapBuffers commits the wl_surface; vsync (swap interval) paces the loop.
        eglSwapBuffers(egl_display, egl_surface);
        epoch_counter++;
    }

    // Clean teardown -> compositor repaints whatever wallpaper was underneath.
    glBindVertexArray(0);
    glDeleteTextures(8, textures);
    glDeleteFramebuffers(8, framebuffers);
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(egl_display, egl_surface);
    if (egl_window)
        wl_egl_window_destroy(egl_window);
    if (egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);
    zwlr_layer_surface_v1_destroy(layer_surface);
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    return 0;
}

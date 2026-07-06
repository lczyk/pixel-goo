// RGFW front-end -> bin/goo. Owns the window, GL context, event pump, and the
// present/headless glue; the simulation itself lives in sim.c (see sim.h).

#include "sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp

#include <gl.h> // glad declarations (impl emitted by sim.c)

#define RGFW_OPENGL
#define RGFWDEF // external linkage: implementation lives in rgfw_impl.c, not inline here
#include <RGFW.h>

RGFW_window *window;

// Actual GL framebuffer size in device px -- the present viewport / upscale target.
// NOTE: only macos has a separate backing store where getSizeInPixels is the real
// framebuffer size. on x11/xwayland/wayland RGFW never applies a buffer scale, so the
// framebuffer is the logical window size -- getSizeInPixels inflates by the monitor dpi
// ratio and oversizes the viewport (one quadrant in fullscreen, 3/4-black in windowed).
// confirmed via --corners-debug: the real framebuffer == getSize in both modes.
static void window_drawable_px(i32 *w, i32 *h) {
#ifdef RGFW_MACOS
    RGFW_window_getSizeInPixels(window, w, h);
#else
    RGFW_window_getSize(window, w, h);
#endif
}

void window_setup(void);
void handle_framebuffer_resize(int width, int height);

int main(int argc, char **argv) {
    parse_args(argc, argv, false, false);

    // Headless needs a fixed frame count and ffmpeg to pipe into. Check both before any
    // GL/window/sim init, so a bad invocation fails instantly rather than after setup.
    if (headless_path) {
        if (max_iterations <= 0) {
            fprintf(stderr, "goo: --headless needs -N <frames> (a fixed frame count)\n");
            exit(EXIT_FAILURE);
        }
        if (system("command -v ffmpeg >/dev/null 2>&1") != 0) {
            fprintf(stderr, "goo: --headless needs ffmpeg in PATH\n");
            exit(EXIT_FAILURE);
        }
    }

    window_setup();
    sim_setup();

    i32 xpos = 0;
    i32 ypos = 0;
    bool mouse_in_window = RGFW_window_isMouseInside(window);

    // Loop counter passed to the shaders for use in random()
    int epoch_counter = 0;

    // Physics timing preamble
    float exp_average_flip_time = 0.0f;
    float alpha_flip_time = 0.1;

    // True pipelined frame time (no glFinish); printed every 60 frames when not
    // profiling, so --no-vsync shows real fps without the serialisation penalty.
    double frame_prev = get_time();
    double frame_ms = 0.0;
    double fps_sleep_ms = 0.0; // --fps pacing: slack we sleep each frame

    //======================================
    //
    //  ##       #####    #####   #####
    //  ##      ##   ##  ##   ##  ##  ##
    //  ##      ##   ##  ##   ##  #####
    //  ##      ##   ##  ##   ##  ##
    //  ######   #####    #####   ##
    //
    //======================================

    // Headless: open the ffmpeg pipe now that width/height (render res) are final. Frames are
    // raw rgb24 at render res; -vf vflip undoes glReadPixels' bottom-up order. Framerate from
    // --fps (default 60). Output codec is picked from the extension: .gif -> palette gif,
    // anything else -> h264 mp4. ponytail: two formats, add more branches if you need them.
    if (headless_path) {
        int fr = fps_cap > 0 ? fps_cap : 60;
        // Upscale render-res -> logical size with nearest-neighbour, mirroring the
        // window's GL_NEAREST upscale, so pixels stay crisp instead of the player
        // bilinear-blurring the tiny frame. Even dims keep yuv420p happy (gif doesn't care).
        int outw = (int)lround(width * render_scale);
        outw -= outw & 1;
        int outh = (int)lround(height * render_scale);
        outh -= outh & 1;
        const char *dot = strrchr(headless_path, '.');
        bool is_gif = dot && strcasecmp(dot, ".gif") == 0;
        char cmd[1280];
        if (is_gif)
            // gif: build a per-clip optimal 256-colour palette in one streaming pass
            // (ffmpeg buffers the stream for palettegen), default sierra2_4a dither.
            // yuv420p/-crf are mp4-only. NOTE: fps is honoured verbatim; many gif
            // players floor high rates -- drop --fps if playback looks fast.
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - "
                     "-vf \"vflip,scale=%d:%d:flags=neighbor,split[s0][s1];"
                     "[s0]palettegen[p];[s1][p]paletteuse\" \"%s\"",
                     width, height, fr, outw, outh, headless_path);
        else
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - "
                     "-vf vflip,scale=%d:%d:flags=neighbor -pix_fmt yuv420p -crf 18 \"%s\"",
                     width, height, fr, outw, outh, headless_path);
        ffmpeg_pipe = popen(cmd, "w");
        if (!ffmpeg_pipe) {
            fprintf(stderr, "goo: failed to start ffmpeg\n");
            exit(EXIT_FAILURE);
        }
        headless_px = (unsigned char *)malloc((size_t)width * height * 3);
    }

    while (!RGFW_window_shouldClose(window)) {
        // -N counts PRESENTED frames; warmup frames run first and don't count, so we
        // stop after warmup + max_iterations total iterations.
        if (max_iterations > 0 && epoch_counter - warmup >= max_iterations)
            break;
        bool presenting = epoch_counter >= warmup; // false during the warmup ramp

        // Poll mouse position (--no-mouse parks it far off so the repel never triggers)
        // and the per-frame velocity (delta vs last frame, same sim units as position).
        static float prev_mouse_position[2] = {-1e9f, -1e9f};
        float mouse_position[2] = {-1e9f, -1e9f};
        float mouse_velocity[2] = {0.0f, 0.0f};
        if (!no_mouse && mouse_in_window) {
            RGFW_window_getMouse(window, &xpos, &ypos);
            mouse_position[0] = (float)xpos * mouse_scale;
            mouse_position[1] = (float)ypos * mouse_scale;
            // velocity only if the previous frame was also in-window -- otherwise the delta
            // is the teleport from the parked sentinel (re-entry jump), not a real motion.
            if (prev_mouse_position[0] > -1e8f) {
                mouse_velocity[0] = mouse_position[0] - prev_mouse_position[0];
                mouse_velocity[1] = mouse_position[1] - prev_mouse_position[1];
            }
        }
        prev_mouse_position[0] = mouse_position[0];
        prev_mouse_position[1] = mouse_position[1];

        sim_step(epoch_counter, mouse_position, mouse_velocity);

        // Headless: renderBuffer holds the final composited frame at render res. Read it back
        // and pipe to ffmpeg, then skip all window present (upscale/swap/events) below.
        if (headless_path && presenting) // warmup: render but don't encode (skip the ramp)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[renderBuffer.current]);
            // tightly-packed rgb24: default GL_PACK_ALIGNMENT=4 pads rows when width*3 % 4
            // != 0 (e.g. 125 -> 375), shearing the frame. force byte packing.
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, headless_px);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            fwrite(headless_px, 1, (size_t)width * height * 3, ffmpeg_pipe);
        }

        // Sync the GL drawable + upscale viewport to the window's CURRENT geometry, as late
        // as possible (right before the only pass that touches the window). The window can
        // change backing scale mid-frame when dragged across monitors of different dpi.
        if (!headless_path && presenting) // warmup: simulate in the bg, present nothing yet
        {
#ifdef RGFW_MACOS
            // macos retina can flip backing scale mid-frame (window dragged across
            // monitors of different dpi); resync the gl drawable. x11/wayland don't.
            RGFW_window_updateContext_OpenGL(window);
#endif
            {
                i32 fbw = 0, fbh = 0;
                window_drawable_px(&fbw, &fbh);
                window_width = fbw;
                window_height = fbh;
                screenBuffer.width = window_width;
                screenBuffer.height = window_height;
            }

            sim_present();

            // Corners debug (--corners-debug): green squares at the 4 corners of what we
            // believe is the window framebuffer (window_width/height). Tells us if the present
            // viewport actually matches the visible window -- a corner you don't see means the
            // assumed size overshoots the real framebuffer on that edge. scissor + clear, no
            // shader. NOTE: leaves FB 0 bound (sim_present bound it) so it draws to the window.
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

            if (profile && epoch_counter % 60 == 0) {
                double tot = 0;
                fprintf(stdout, "ms:");
                for (int i = 0; i < 8; i++) {
                    fprintf(stdout, " %s=%.2f", pass_name[i], pass_ms[i]);
                    tot += pass_ms[i];
                }
                fprintf(stdout, " | total=%.2f (%.0f fps)\n", tot, tot > 0 ? 1000.0 / tot : 0);
            }

            double frame_now = get_time();
            frame_ms = ema(frame_ms, (frame_now - frame_prev) * 1000.0, 0.1);
            frame_prev = frame_now;
            if (!profile && epoch_counter % 60 == 0) {
                fprintf(stdout, "frame=%.2f ms (%.0f fps)\n", frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0);
            }

            float flip_buffer_start = get_time();
            RGFW_window_swapBuffers_OpenGL(window); // Swap draw and screen buffer
            float delta_flip_time = (get_time() - flip_buffer_start) * 1000;
            exp_average_flip_time == 0.0f
                ? exp_average_flip_time = delta_flip_time
                : (exp_average_flip_time = alpha_flip_time * delta_flip_time + (1 - alpha_flip_time) * exp_average_flip_time);

            // Poll and dispatch window events. Escape-to-quit is handled by the
            // exit key set in window_setup, so we only need to react to resizes here.
            RGFW_event event;
            while (RGFW_window_checkEvent(window, &event)) {
                if (event.type == RGFW_windowResized) {
                    handle_framebuffer_resize(event.update.w, event.update.h);
                } else if (event.type == RGFW_mouseLeave) {
                    mouse_in_window = false;
                } else if (event.type == RGFW_mouseEnter) {
                    mouse_in_window = true;
                }
                // 'q' quits, same as escape (escape handled via exit key)
                else if (event.type == RGFW_keyPressed && event.key.value == RGFW_keyQ) {
                    RGFW_window_setShouldClose(window, RGFW_TRUE);
                }
            }

            // --fps: nudge a per-frame sleep so the EMA frame time settles at the
            // target. frame_ms (measured top-to-top) already includes this sleep, so a
            // simple proportional controller converges. Clamps at 0 when we're already
            // slower than target -- no slack to give back.
            // ponytail: plain P-controller, fine for a soft cap; add I/D if it hunts.
            if (fps_cap > 0) {
                double target_ms = 1000.0 / fps_cap;
                fps_sleep_ms += (target_ms - frame_ms) * 0.5;
                if (fps_sleep_ms < 0.0)
                    fps_sleep_ms = 0.0;
                sleep_ms(fps_sleep_ms);
            }
        } // !headless_path: window present skipped in headless mode
        epoch_counter++;
    }

    // Final benchmark summary (one parseable line at exit, e.g. after -N frames).
    if (profile) {
        double tot = 0;
        fprintf(stdout, "SUMMARY ms:");
        for (int i = 0; i < 8; i++) {
            fprintf(stdout, " %s=%.2f", pass_name[i], pass_ms[i]);
            tot += pass_ms[i];
        }
        fprintf(stdout, " | total=%.2f (%.0f fps)\n", tot, tot > 0 ? 1000.0 / tot : 0);
    } else {
        fprintf(stdout, "SUMMARY frame=%.2f ms (%.0f fps)\n", frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0);
    }

    // Headless: flush + close the ffmpeg pipe so it finalises the output file.
    if (ffmpeg_pipe) {
        pclose(ffmpeg_pipe);
        free(headless_px);
    }

    // Debug: dump the final frame + density/trail fields for inspection (--dump).
    if (dump_path) {
        char path[1024];
        snprintf(path, sizeof path, "%s_frame.ppm", dump_path);
        dump_ppm_rgb(path, width, height, framebuffers[renderBuffer.current]);
        snprintf(path, sizeof path, "%s_density.ppm", dump_path);
        dump_ppm_scalar(path, density_width, density_height, framebuffers[densityBuffer.current]);
        snprintf(path, sizeof path, "%s_trail.ppm", dump_path);
        dump_ppm_scalar(path, trail_width, trail_height, framebuffers[trailBuffer.current]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    // Clean up
    // TODO: clean up after oneself better
    glBindVertexArray(0);
    glDeleteTextures(9, textures);
    glDeleteFramebuffers(9, framebuffers);
    RGFW_window_close(window);
    RGFW_deinit();
    return 0;
}

//============================================================
//
//  ##      ##  ##  ##     ##  ####     #####   ##      ##
//  ##      ##  ##  ####   ##  ##  ##  ##   ##  ##      ##
//  ##  ##  ##  ##  ##  ## ##  ##  ##  ##   ##  ##  ##  ##
//  ##  ##  ##  ##  ##    ###  ##  ##  ##   ##  ##  ##  ##
//   ###  ###   ##  ##     ##  ####     #####    ###  ###
//
//============================================================

void window_setup(void) {
    fprintf(stdout, "Setting up RGFW window...\n");

    RGFW_init();

    // Request a core OpenGL context. macos caps at 4.1 core, which is the
    // highest that still supports our "#version 330 core" shaders (a 3.2 core
    // context would not), so ask for 4.1 directly.
    RGFW_glHints *hints = RGFW_getGlobalHints_OpenGL();
    hints->major = 4;
    hints->minor = 1;
    hints->profile = RGFW_glCore;

    RGFW_windowFlags flags = RGFW_windowOpenGL;
    if (no_keyfocus_steal)
        flags |= RGFW_windowNoFocusOnCreate;
    // Same-space fullscreen path (--no-keyfocus-steal): cover the monitor with a
    // borderless window in the *current* Space instead of native fullscreen, which
    // would open a new Space and yank focus. Born borderless so no resize dance.
    if (fullscreen && no_keyfocus_steal)
        flags |= RGFW_windowNoBorder;
    // Windowed mode defaults to a normal bordered window. --no-border removes
    // the title bar / border when the user explicitly wants that look.
    if (!fullscreen && no_border)
        flags |= RGFW_windowNoBorder;

    // Window spawn position. In fullscreen we place the window at the target
    // monitor's origin so RGFW_window_setFullscreen (which fullscreens onto
    // whichever monitor the window is on) picks the right one. In windowed mode
    // we center it on the primary instead.
    int win_x = 0;
    int win_y = 0;

    if (fullscreen) {
        size_t monitorCount;
        RGFW_monitor **monitors = RGFW_getMonitors(&monitorCount);
        printf("%zu monitors found\n", monitorCount);
        if (monitorCount == 0) {
            fprintf(stderr, "No monitors found\n");
            exit(EXIT_FAILURE);
        }
        if ((size_t)whichMonitor >= monitorCount) {
            whichMonitor = 0;
        };
        printf("using monitor %d\n", whichMonitor);
        RGFW_monitor *monitor = monitors[whichMonitor];
        win_x = monitor->x;
        win_y = monitor->y;
        width = monitor->mode.w;
        height = monitor->mode.h;
        printf("%d %d\n", width, height);
    } else {
        if (whichMonitor != 0) {
            size_t monitorCount;
            RGFW_monitor **monitors = RGFW_getMonitors(&monitorCount);
            if ((size_t)whichMonitor < monitorCount) {
                RGFW_monitor *monitor = monitors[whichMonitor];
                win_x = monitor->x + (monitor->mode.w - width) / 2;
                win_y = monitor->y + (monitor->mode.h - height) / 2;
                printf("monitor %d: origin=(%d,%d) size=(%d,%d) -> win=(%d,%d)\n",
                       whichMonitor, monitor->x, monitor->y,
                       monitor->mode.w, monitor->mode.h, win_x, win_y);
            }
        } else {
            flags |= RGFW_windowCenter;
        }
    }

    window = RGFW_createWindow(title, win_x, win_y, width, height, flags);

    if (!window) {
        fprintf(stderr, "Failed to create RGFW window\n");
        exit(EXIT_FAILURE);
    }

    if (!fullscreen && whichMonitor != 0)
        RGFW_window_move(window, win_x, win_y);

    // Fullscreen has two modes: same-space borderless cover (no focus steal) vs
    // native fullscreen Space (steals focus by design). See each branch.
#ifdef RGFW_MACOS
    if (fullscreen && no_keyfocus_steal) {
        // Same-space cover: no native toggleFullScreen (which opens a new Space and
        // steals focus). Cover the display in the current Space, above the menu bar.
        // NoFocusOnCreate keeps key focus off at launch, so the keyboard stays with
        // whatever the user was doing. Clicking the window grabs key focus on demand
        // (canBecomeKeyWindow override in RGFW.h), which enables escape-to-quit;
        // benchmarks that never click stay hands-off and use -N to auto-exit.
        RGFW_window_move(window, win_x, win_y);
        RGFW_window_coverDisplay(window);
    } else if (fullscreen) {
        // Native fullscreen Space; steals key focus by design (that's how macos
        // opening a window in a new Space works, and what we want when we *do* want
        // the keyboard). Move onto the target monitor first -- cocoa's makeKeyWindow
        // can pull a freshly-created window onto the active screen.
        RGFW_window_move(window, win_x, win_y);
        RGFW_window_setFullscreen(window, RGFW_TRUE);
        // setFullscreen leaves a borderless status-level window. With the
        // canBecomeKeyWindow override in RGFW.h it can now take key focus.
        RGFW_window_focus(window);
    }
#else
    // x11/wayland: no Spaces, fullscreen focus is compositor-controlled. plain fullscreen.
    if (fullscreen) {
        RGFW_window_move(window, win_x, win_y);
        RGFW_window_setFullscreen(window, RGFW_TRUE);
    }
#endif

    i32 fb_width, fb_height, logical_width, logical_height;
    RGFW_window_getSize(window, &logical_width, &logical_height);
    window_drawable_px(&fb_width, &fb_height);
    sim_set_dims(logical_width, logical_height, fb_width, fb_height);

    // Quit when escape is pressed (reported through RGFW_window_shouldClose)
    RGFW_window_setExitKey(window, RGFW_keyEscape);

    RGFW_window_swapInterval_OpenGL(window, vsync ? 1 : 0); // --no-vsync disables

    // GLAD
    if (!gladLoadGL((GLADloadfunc)RGFW_getProcAddress_OpenGL)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        exit(EXIT_FAILURE);
    }

    // Enable point size rendering and alpha blending
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void handle_framebuffer_resize(int new_width, int new_height) {
    // The resize event carries logical points. Query the GL framebuffer (device px)
    // for the upscale target and the logical size, then let sim_resize recompute the
    // render/sim geometry and reallocate the resolution-dependent buffers.
    (void)new_width;
    (void)new_height;
    i32 fb_width, fb_height, logical_width, logical_height;
    RGFW_window_getSize(window, &logical_width, &logical_height);
    window_drawable_px(&fb_width, &fb_height);
    sim_resize(logical_width, logical_height, fb_width, fb_height);
}

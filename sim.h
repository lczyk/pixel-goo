#ifndef SIM_H
#define SIM_H

// Window-agnostic simulation core, shared by the two front-ends:
//   - main.c       -> bin/goo        (RGFW window, X11/cocoa/win)
//   - main-wlwp.c  -> bin/goo-wlwp   (wayland layer-shell wallpaper)
// Everything here is pure GL + cli config; no windowing-toolkit symbols. Each
// front-end owns its own context creation, event pump, and buffer-swap, then
// drives the sim through the small API at the bottom of this file.

#include "buffer.h"
#include "shader.h"

#include <stdbool.h>
#include <stdio.h>

// ---- render / sim geometry (set by the front-end via sim_set_dims) ----
extern int width;          // internal render resolution (logical / render_scale)
extern int height;         //
extern int window_width;   // actual framebuffer size in device px (upscale target)
extern int window_height;  //
extern double render_scale;
extern const double SIM_SCALE;
extern int sim_width, sim_height; // simulation field size (logical / SIM_SCALE)
extern float mouse_scale;         // logical-point -> sim-space
extern float dpi_scale;           // logical-point -> physical-pixel
extern const char *title;

// ---- runtime flags / params (filled by parse_args) ----
extern bool fullscreen;
extern bool vsync;
extern bool profile;
extern int max_iterations;
extern int warmup;
extern int fps_cap;
extern bool no_keyfocus_steal;
extern bool no_border;
extern bool no_mouse;
extern bool mouse_debug;
extern bool corners_debug;
extern char *dump_path;
extern char *headless_path;
extern FILE *ffmpeg_pipe;
extern unsigned char *headless_px;
extern int rng_seed;
extern double init_warp;
extern double init_density;
extern bool p_given;
extern int whichMonitor;

// ---- gl objects / buffers ----
extern GLuint textures[9];
extern GLuint framebuffers[9];
extern const PBindex densityBufferIndex;
extern const PBindex positionBufferIndex1;
extern const PBindex positionBufferIndex2;
extern const PBindex velocityBufferIndex1;
extern const PBindex velocityBufferIndex2;
extern const PBindex trailBufferIndex1;
extern const PBindex trailBufferIndex2;
extern const PBindex renderBufferIndex;
extern int PB_width, PB_height;
extern GLuint pbo_pos, pbo_vel;

extern Shader screenShader, densityShader, positionShader, velocityShader;
extern Shader copyShader, trailShader, upscaleShader, debugShader;

extern Buffer trailBuffer, positionBuffer, velocityBuffer, densityBuffer;
extern Buffer screenBuffer, renderBuffer;

// ---- tunables (default-params is the source of truth; see nob.c) ----
extern double densityAlpha, kernelRadius;
extern double densityForce, trailForce, edgeRepel, densityReach, trailReach;
extern int densitySubsample, trailSubsample, dens_every, trail_every;
extern double cullAmount;
extern double renderHeadroom, headroomMargin, headroomAttack, headroomRelease, renderGamma;
extern bool densityNearest;
extern double headroomPct;
extern float headroom_ema;
extern float *density_readback;
extern int densityBufferDownsampling, density_width, density_height;
extern double dragCoefficient, ditherCoefficient, ditherDensityGain, ditherOrtho;
extern bool legacyWedge;
extern double trailIntensity;
extern double trailRadius;
extern int trailBufferDownsampling;
extern double trailVelocityFloor;
extern int trail_width, trail_height;
extern int P;

// --gl-refresh <dur>: how often (seconds) to recreate the GL context to reclaim
// the freedreno per-submit leak (see debug/). default 30m; 0 = never. wallpaper only.
extern double gl_refresh_seconds;

// tile-coherent draw order
extern GLuint ibo;
extern unsigned int *sort_idx;
extern int *sort_key;
extern int *sort_counts;
extern const int sort_tile;
extern int sort_every;

// per-pass GPU timing (--profile)
extern double pass_ms[8];
extern const char *pass_name[8];

// ---- utilities (front-ends use these for pacing / debug dumps) ----
double get_time(void);
void sleep_ms(double ms);
double ema(double prev, double sample, double a);
void dump_ppm_rgb(const char *path, int w, int h, GLuint fbo);
void dump_ppm_scalar(const char *path, int w, int h, GLuint fbo);

// ---- core API ----
// parse cli + config into the globals. wlwp = true drops the window-only flags
// (monitor/width/height/fps/mouse/headless/...) from both the option table and the
// baked-in defaults, so goo-wlwp only exposes flags that concern a wallpaper.
// wlwp = wallpaper build (drops window-only flags). macwp additionally exposes -m
// (monitor index; -1 = clone to all monitors) -- only the macos wallpaper uses it.
void parse_args(int argc, char **argv, bool wlwp, bool macwp);
void shader_setup(void);
void buffer_setup(void);
void updateShaderWindowShape(int width, int height);
void updateShaderBufferShape(int width, int height);

// set the geometry globals from the front-end's measured sizes (no GL side effects).
void sim_set_dims(int logical_w, int logical_h, int fb_w, int fb_h);
// derive P (unless -p given), compile shaders, allocate buffers, push static uniforms.
// call after the GL context is current and sim_set_dims has run.
void sim_setup(void);
// recompute geometry + reallocate the resolution-dependent buffers (resize path).
void sim_resize(int logical_w, int logical_h, int fb_w, int fb_h);
// one physics+render frame -> renderBuffer (render resolution). mouse_* in sim space;
// pass a parked sentinel ({-1e9,-1e9}) and zero velocity to disable the repel.
void sim_step(int epoch_counter, const float mouse_position[2], const float mouse_velocity[2]);
// nearest-upscale renderBuffer onto the default framebuffer (the window). caller
// must have screenBuffer.width/height set to the current framebuffer size.
void sim_present(void);

// ---- GL-context refresh (freedreno leak workaround; see debug/) ----
// The whole sim state lives in GL objects, so recreating the EGL context to
// reclaim the driver leak means saving + restoring that state around it. Also the
// groundwork for a future dump-to-file / load-from-file of the sim.
typedef struct SimSnapshot {
    int pb_w, pb_h;     // physics buffer dims (holds P particles)
    int tr_w, tr_h;     // trail field dims
    float *pos;         // pb_w*pb_h*2 (RG)
    float *vel;         // pb_w*pb_h*2 (RG)
    float *trail;       // tr_w*tr_h   (R)
    float headroom_ema; // auto-exposure state
} SimSnapshot;

// read the live particle + trail buffers back to CPU. call with the GL context
// still current (i.e. before tearing it down).
SimSnapshot *sim_snapshot(void);
// hand a snapshot to the NEXT sim_setup, which uploads it instead of generating
// fresh initial state. consumed (cleared) by that sim_setup.
void sim_restore(const SimSnapshot *s);
void sim_snapshot_free(SimSnapshot *s);
// free the CPU-side scratch (sort arrays, density readback) so a re-run of
// sim_setup in a fresh context doesn't leak the old allocations. the GL objects
// themselves are freed by the context teardown, so this only touches CPU memory.
void sim_teardown_cpu(void);

#endif /* SIM_H */

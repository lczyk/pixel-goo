#include "shader.h"
#include "buffer.h"

#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#define DROPT_IMPLEMENTATION // header-only dropt: this is the single TU that emits the impl
#include <dropt.h> // cli option parser (lib/dropt/)

#define GLAD_GL_IMPLEMENTATION // header-only glad: this is the single TU that emits the impl
#include <gl.h>

#define RGFW_OPENGL
#define RGFWDEF // external linkage: implementation lives in rgfw_impl.c, not inline here
#include <RGFW.h>

#include <cglm/cglm.h>
#include <cglm/noise.h>

// Window
RGFW_window *window;
// width/height are the INTERNAL render (sim) resolution -- logical points divided
// by render_scale. Everything (physics-independent visual passes, density/trail
// buffers, window_shape) is computed against this, then nearest-upscaled to the
// actual framebuffer (window_width/height). This decouples fill cost from device
// DPI (retina renders at 1x) and gives a single perf/quality knob via render_scale.
int width = 800;
int height = 600;
int window_width = 800;  // actual framebuffer size in device px; the upscale target
int window_height = 600;
double render_scale = 2.0; // internal res = logical points / render_scale (>1 = lower res, faster); --render-scale
float mouse_scale = 1.0f;  // logical-point -> render-resolution scale
float dpi_scale = 1.0f;   // logical-point -> physical-pixel scale (>1 on Retina)
// int width = 400; int height = 400;
const char *title = "Pixel Goo";
// runtime-tunable via cli (see parse_args); defaults below
bool fullscreen = true;
bool vsync = true;
bool profile = false; // --profile: glFinish per pass + print ms (disables pipelining)
int max_iterations = 0; // -N: exit after N loop iterations (0 = unlimited); for benchmarking
int fps_cap = 60;       // --fps-cap: throttle the average fps to this (0 = uncapped)
bool no_keyfocus_steal = false; // --no-keyfocus-steal: show window w/out grabbing key focus
bool no_border = false;         // --no-border: hide the title bar / border in windowed mode
bool no_mouse = false;          // --no-mouse: disable the mouse repel (park the cursor far off)
bool mouse_debug = false;       // --mouse-debug: draw green dot + trail at cursor
char *dump_path = NULL; // --dump <prefix>: debug. at exit, write frame + density + trail to <prefix>_*.ppm
int rng_seed = 0;       // --seed <N>: fixed RNG seed for reproducible runs (0 = time-based)
int whichMonitor = 0;
// int whichMonitor = 1;

// Textures and framebuffers
GLuint textures[8];
GLuint framebuffers[8];
const PBindex densityBufferIndex = 0;
const PBindex positionBufferIndex1 = 1;
const PBindex positionBufferIndex2 = 2;
const PBindex velocityBufferIndex1 = 3;
const PBindex velocityBufferIndex2 = 4;
const PBindex trailBufferIndex1 = 5;
const PBindex trailBufferIndex2 = 6;
const PBindex renderBufferIndex = 7; // internal colour target, upscaled to the window

// Physics buffer dims (square-ish texture holding P particles), set in buffer_setup.
int PB_width = 0;
int PB_height = 0;
// PBOs holding position/velocity: filled each frame by glReadPixels(FBO->PBO) and bound
// as vertex attributes for the point passes (faster than vertex texture fetch on this gpu).
GLuint pbo_pos = 0;
GLuint pbo_vel = 0;

Shader screenShader = {.name = "screenShader"};
Shader densityShader = {.name = "densityShader"};
Shader positionShader = {.name = "positionShader"};
Shader velocityShader = {.name = "velocityShader"};
Shader copyShader = {.name = "copyShader"};
Shader trailShader = {.name = "trailShader"};
Shader upscaleShader = {.name = "upscaleShader"};
Shader debugShader = {.name = "debugShader"};

// Include shader source files
#include "copy.h"
#include "trail.h"
#include "screen.h"
#include "density.h"
#include "position.h"
#include "velocity.h"
#include "upscale.h"
#include "debug.h"

Buffer trailBuffer = {.name = "Trail Buffer", .textures = textures, .framebuffers = framebuffers, .current = 0, .other = 1};
Buffer positionBuffer = {.name = "Position buffer", .textures = textures, .framebuffers = framebuffers, .current = 0, .other = 1};
Buffer velocityBuffer = {.name = "Velocity buffer", .textures = textures, .framebuffers = framebuffers, .current = 0, .other = 1};
Buffer densityBuffer = {.name = "Density buffer", .textures = textures, .framebuffers = framebuffers, .current = 0, .other = 1};
Buffer screenBuffer = {.name = "Screen buffer", .textures = NULL, .framebuffers = NULL, .current = 0, .other = 1};
// Internal render target: the visual passes draw here at render resolution, then
// upscaleShader nearest-samples it onto screenBuffer (the actual window).
Buffer renderBuffer = {.name = "Render buffer", .textures = textures, .framebuffers = framebuffers, .current = 0, .other = 1};

// Alpha blending of each of the fragments
const float densityAlpha = 0.005f;
// const float densityAlpha = 0.9f;
const float kernelRadius = 30.0f;

// The density/trail buffers are heavily downsampled, lerped physics fields -- not
// the rendered image. Scattering every Nth particle into them (with the per-point
// alpha scaled up to compensate) cuts the overdraw/ROP cost without a visible
// change, since the final screen pass still draws all P points. 1 = no subsample.
int densitySubsample = 4; // --dens-sub
int trailSubsample = 2;   // --trail-sub
int dens_every = 3;       // --dens-every: rebuild density field every N frames (reuse between)
int trail_every = 2;      // --trail-every: deposit a rotating 1/N trail subset per frame (decay every frame)

// Screen render: density-weighted cull (--cull), 0 = off .. 1 = maximum thinning.
// Each particle gets a fixed random threshold, but the probability is scaled by
// capped sqrt(local density / render_headroom), so denser regions cull more
// aggressively without blacking out hot cores. Only thins the screen point pass,
// not the physics.
double cullAmount = 0.8; // --cull

// Screen colormap: density is additive/unbounded; the render log-maps it (a compressor),
// and the headroom is the makeup gain. By default it AUTO-tracks the live density max with
// an EMA (an envelope follower), so the brightest region always lands just under clipping
// regardless of particle count / cluster state -- auto-exposure. --headroom overrides with
// a fixed value (disables auto); --headroom-margin scales the tracked max (>1 = darker /
// more headroom, <1 = brighter / some clip); --headroom-attack is the EMA speed.
double renderHeadroom = 0.0;     // --headroom: fixed value (0 = auto-track)
double headroomMargin = 1.15;    // --headroom-margin: tracked-max multiplier
double headroomAttack = 0.05;    // --headroom-attack: EMA alpha for the max follower
double renderGamma = 0.6;        // --gamma: colormap curve (1 = linear/punchy, <1 lifts faint)
bool densityNearest = false;     // --density-nearest: GL_NEAREST density (chunky pixels) vs default GL_LINEAR
double headroomPct = 0.98;       // --headroom-pct: track this density percentile, not the max
                                 // (1.0 = max; <1 ignores outlier hot-spots so the network gets the bright range)
float headroom_ema = 0.0f;       // running EMA of the density max (the auto headroom)
float *density_readback = NULL;  // CPU scratch for reading the density buffer back

// Buffer resolution divisor: the density field is render-res / this. Can be high because
// the field is lerped and particles dither into it. --dens-downsample.
int densityBufferDownsampling = 20;
int density_width = 0;  // computed in buffer_setup once width/height are final
int density_height = 0;

const float dragCoefficient = 0.11;
// const float ditherCoefficient = 0.08;
const float ditherCoefficient = 0.1;

// Alpha blending of each of the fragments
const float trailIntensity = 0.06f;
const float trailAlpha = 0.85f;
// const float trailAlpha = 0.90f;
const float trailRadius = 15.0f;
int trailBufferDownsampling = 10; // trail field resolution = render-res / this. --trail-downsample.
const float trailVelocityFloor = 0.6;
int trail_width = 0;
int trail_height = 0;

// Particles
// const int P = 16384; // <- render buffer max
// const int P = 30000;
// const int P = 160000;
// const int P = 300000;
// const int P = 500000;
int P = 200000; // --particles
// const int P = 1000000; // emmmmm...

void window_setup();
void shader_setup();
void buffer_setup();
void handle_framebuffer_resize(int width, int height);
void updateShaderWindowShape(int width, int height);
void updateShaderBufferShape(int width, int height);

// Monotonic seconds; replaces glfwGetTime
static double get_time()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Sleep for ms milliseconds (frame pacing). <=0 returns immediately.
static void sleep_ms(double ms)
{
    if (ms <= 0.0) return;
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000.0),
        .tv_nsec = (long)(fmod(ms, 1000.0) * 1e6),
    };
    nanosleep(&req, NULL);
}

// Uniform random float in [0, 1]; replaces glm::linearRand
static float frand01()
{
    return (float)rand() / (float)RAND_MAX;
}

// Exponential moving average; seeds on first sample (prev == 0).
static double ema(double prev, double sample, double a)
{
    return prev == 0.0 ? sample : a * sample + (1.0 - a) * prev;
}

// Ascending float compare, for the auto-headroom percentile (qsort over the tiny density buffer).
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

// Debug dump tooling (--dump). Read a framebuffer back and write it as a PPM so it can be
// eyeballed as an image. glReadPixels is bottom-up, so flip to top-down on write.
static void dump_ppm_rgb(const char *path, int w, int h, GLuint fbo)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        fprintf(stdout, "dumped %s\n", path);
    }
    free(px);
}

// Single-channel R32F field -> grayscale PPM, normalised by its own max (printed too, so
// the actual value range is visible -- important once density goes unbounded/additive).
static void dump_ppm_scalar(const char *path, int w, int h, GLuint fbo)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    float *v = (float *)malloc((size_t)w * h * sizeof(float));
    glReadPixels(0, 0, w, h, GL_RED, GL_FLOAT, v);
    float mx = 0.0f;
    for (size_t i = 0; i < (size_t)w * h; i++) if (v[i] > mx) mx = v[i];
    float norm = mx > 0.0f ? mx : 1.0f;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float t = v[i] / norm;
        px[i] = (unsigned char)(255.0f * (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t)));
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P5\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w, 1, (size_t)w, f);
        fclose(f);
        fprintf(stdout, "dumped %s (max value %.4f)\n", path, mx);
    }
    free(px);
    free(v);
}

// Tile-coherent draw order. The screen pass binds the goo's scattered points into
// random TBDR tiles, which is the bottleneck; drawing them in screen-tile order via
// an index buffer fixes it. The order is rebuilt by an O(P) counting sort over the
// positions read back into pbo_pos (mapped on the CPU). sort_every controls cadence.
GLuint ibo = 0;
unsigned int *sort_idx = NULL; // P entries: particle indices in tile order
int *sort_key = NULL;          // P entries: per-particle tile bucket (scratch)
int *sort_counts = NULL;       // (nbuckets+1) histogram / prefix-sum scratch
const int sort_tile = 32;      // screen tile size in px (~AGX tile granularity)
int sort_every = 8;            // rebuild the order every N frames (1 = every frame)

// Counting-sort the P particle indices by screen tile, using the positions currently
// in pbo_pos. Uploads the result into ibo. O(P), two linear passes. Out-of-bounds
// positions are clamped (only affects coherence, never correctness).
static void rebuild_sort_order(void)
{
    int tilesW = (width + sort_tile - 1) / sort_tile;
    int tilesH = (height + sort_tile - 1) / sort_tile;
    int nb = tilesW * tilesH;
    float inv_tile = 1.0f / (float)sort_tile;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_pos);
    float *pos = (float *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (!pos)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return;
    }

    for (int i = 0; i <= nb; i++) sort_counts[i] = 0;
    for (int i = 0; i < P; i++)
    {
        int tx = (int)(pos[2 * i] * inv_tile);
        int ty = (int)(pos[2 * i + 1] * inv_tile);
        tx = tx < 0 ? 0 : (tx >= tilesW ? tilesW - 1 : tx);
        ty = ty < 0 ? 0 : (ty >= tilesH ? tilesH - 1 : ty);
        int b = ty * tilesW + tx;
        sort_key[i] = b;
        sort_counts[b + 1]++;
    }
    for (int i = 0; i < nb; i++) sort_counts[i + 1] += sort_counts[i];
    for (int i = 0; i < P; i++) sort_idx[sort_counts[sort_key[i]]++] = (unsigned int)i;

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)P * sizeof(unsigned int), sort_idx, GL_DYNAMIC_DRAW);
}

// Per-pass GPU timing. LAP(i) drains the pipe (glFinish) and folds the elapsed
// ms since the previous lap into pass_ms[i]. Needs _tp, pass_ms, profile in scope.
#define LAP(i)                                                              \
    do {                                                                    \
        if (profile) {                                                      \
            glFinish();                                                     \
            double _tn = get_time();                                        \
            pass_ms[i] = ema(pass_ms[i], (_tn - _tp) * 1000.0, 0.1);        \
            _tp = _tn;                                                      \
        }                                                                   \
    } while (0)

// dropt handler for --render-scale: optional val; bare -r enables low-res mode at 2.0.
static dropt_error parse_render_scale(dropt_context *ctx, const dropt_option *opt, const char *arg, void *dest)
{
    (void)ctx; (void)opt;
    if (arg == NULL) { *(double *)dest = 2.0; return dropt_error_none; }
    return dropt_handle_double(ctx, opt, arg, dest);
}

// dropt handler for --cull: optional val; bare --cull enables culling at 0.8.
static dropt_error parse_cull(dropt_context *ctx, const dropt_option *opt, const char *arg, void *dest)
{
    (void)ctx; (void)opt;
    if (arg == NULL) { *(double *)dest = 0.8; return dropt_error_none; }
    return dropt_handle_double(ctx, opt, arg, dest);
}

// dropt handler for --seed: optional val; bare --seed picks a random seed and prints it.
static dropt_error parse_seed(dropt_context *ctx, const dropt_option *opt, const char *arg, void *dest)
{
    (void)ctx; (void)opt;
    if (arg == NULL)
    {
        int s = (int)(time(NULL) & 0x7fffffff);
        fprintf(stdout, "goo: seed %d\n", s);
        *(int *)dest = s;
        return dropt_error_none;
    }
    return dropt_handle_int(ctx, opt, arg, dest);
}

// dropt handler for a particle count with an optional k/m suffix (case-insensitive):
// "10000", "10", "1k", "1.5k", "10M" all work. fractional values round to nearest int.
static dropt_error parse_count(dropt_context *ctx, const dropt_option *opt, const char *arg, void *dest)
{
    (void)ctx; (void)opt;
    if (arg == NULL || arg[0] == '\0') return dropt_error_insufficient_arguments;
    char *end;
    double v = strtod(arg, &end);
    if (end == arg) return dropt_error_mismatch;
    if (*end == 'k' || *end == 'K') { v *= 1e3; end++; }
    else if (*end == 'm' || *end == 'M') { v *= 1e6; end++; }
    if (*end != '\0') return dropt_error_mismatch; // trailing junk
    *(int *)dest = (int)(v + 0.5);
    return dropt_error_none;
}

// Parse command-line options into the runtime globals above. Exits the process
// on --help or on a bad option; returns normally to let the sim start.
static void parse_args(int argc, char **argv)
{
    dropt_bool show_help = 0;
    dropt_bool windowed = 0;
    dropt_bool no_vsync = 0;
    dropt_bool prof = 0;
    dropt_bool no_focus = 0;
    dropt_bool dens_near = 0;
    dropt_bool nomouse = 0;

    dropt_option options[] = {
        {'h', "help", "Show this help and exit.", NULL,
         dropt_handle_bool, &show_help, dropt_attr_halt},

        // display
        {'m', "monitor", "Index of the monitor to fullscreen onto.", "N",
         dropt_handle_int, &whichMonitor},
        {'\0', "windowed", "Run in a window instead of fullscreen.", NULL,
         dropt_handle_bool, &windowed},
        {'\0', "no-border", "Hide the window border/title bar.", NULL,
         dropt_handle_bool, &no_border},
        {'\0', "width", "Window width in pixels (windowed mode).", "N",
         dropt_handle_int, &width},
        {'\0', "height", "Window height in pixels (windowed mode).", "N",
         dropt_handle_int, &height},
        {'\0', "no-vsync", "Disable vsync (uncap the frame rate).", NULL,
         dropt_handle_bool, &no_vsync},
        {'\0', "fps-cap", "Throttle the average frame rate to N fps (0 = uncapped).", "N",
         dropt_handle_int, &fps_cap},

        // particles
        {'p', "particles", "Number of particles to simulate (accepts k/m suffix, e.g. 1.5k, 10M).", "N",
         parse_count, &P},
        {'r', "render-scale", "Internal render resolution divisor (>1 = lower res, faster, chunkier; bare -r = 2).", "N",
         parse_render_scale, &render_scale, dropt_attr_optional_val},
        {'\0', "seed", "Fixed RNG seed (bare --seed = random but printed for replay; 0 = silent random).", "N",
         parse_seed, &rng_seed, dropt_attr_optional_val},
        {'\0', "cull", "Screen density cull, 0..1 (bare --cull = 0.8; thins denser regions).", "F",
         parse_cull, &cullAmount, dropt_attr_optional_val},

        // colormap
        {'\0', "gamma", "Colormap curve: 1 = linear/punchy, <1 lifts faint regions.", "F",
         dropt_handle_double, &renderGamma},
        {'\0', "density-nearest", "Sample the density field with GL_NEAREST (chunky pixels) instead of GL_LINEAR.", NULL,
         dropt_handle_bool, &dens_near},
        {'\0', "headroom", "Fixed colormap headroom (0 = auto-track the density max).", "F",
         dropt_handle_double, &renderHeadroom},
        {'\0', "headroom-margin", "Auto-headroom: multiplier on the tracked max (>1 darker, <1 brighter).", "F",
         dropt_handle_double, &headroomMargin},
        {'\0', "headroom-attack", "Auto-headroom: EMA speed of the max follower (0..1).", "F",
         dropt_handle_double, &headroomAttack},
        {'\0', "headroom-pct", "Auto-headroom: track this density percentile (1 = max; <1 ignores hot-spots).", "F",
         dropt_handle_double, &headroomPct},

        // density / trail
        {'\0', "dens-sub", "Density buffer: scatter every Nth particle (higher = less overdraw).", "N",
         dropt_handle_int, &densitySubsample},
        {'\0', "dens-downsample", "Density field resolution divisor (render-res / N; higher = coarser/cheaper).", "N",
         dropt_handle_int, &densityBufferDownsampling},
        {'\0', "dens-every", "Rebuild the density field every N frames (reuse between; cheaper).", "N",
         dropt_handle_int, &dens_every},
        {'\0', "trail-sub", "Trail buffer: scatter every Nth particle (higher = less overdraw).", "N",
         dropt_handle_int, &trailSubsample},
        {'\0', "trail-downsample", "Trail field resolution divisor (render-res / N; higher = coarser/cheaper).", "N",
         dropt_handle_int, &trailBufferDownsampling},
        {'\0', "trail-every", "Update the trail field every N frames (decay/deposit compensated).", "N",
         dropt_handle_int, &trail_every},
        {'\0', "sort-every", "Rebuild tile-coherent draw order every N frames (0 = never).", "N",
         dropt_handle_int, &sort_every},

        // debug
        {'\0', "no-mouse", "Disable the mouse repel interaction.", NULL,
         dropt_handle_bool, &nomouse},
        {'\0', "mouse-debug", "Draw a green dot and trail at the cursor position.", NULL,
         dropt_handle_bool, &mouse_debug},
        {'\0', "profile", "Print per-pass GPU times in ms (forces glFinish, disables pipelining).", NULL,
         dropt_handle_bool, &prof},
        {'N', "iterations", "Exit after N loop iterations (0 = unlimited); for benchmarking.", "N",
         dropt_handle_int, &max_iterations},
        {'\0', "no-keyfocus-steal", "Show the window without stealing keyboard focus (for benchmarking).", NULL,
         dropt_handle_bool, &no_focus},
        {'\0', "dump", "At exit, write frame + density + trail to <prefix>_*.ppm.", "PREFIX",
         dropt_handle_string, &dump_path},

        {0} // sentinel
    };

    dropt_context *ctx = dropt_new_context(options);
    if (ctx == NULL)
    {
        fprintf(stderr, "goo: out of memory setting up option parser\n");
        exit(EXIT_FAILURE);
    }
    dropt_allow_concatenated_arguments(ctx, true);

    dropt_parse(ctx, argc - 1, &argv[1]);

    if (dropt_get_error(ctx) != dropt_error_none)
    {
        fprintf(stderr, "goo: %s\n", dropt_get_error_message(ctx));
        dropt_free_context(ctx);
        exit(EXIT_FAILURE);
    }

    if (show_help)
    {
        fprintf(stdout, "usage: goo [options]\n\n");
        dropt_print_help(stdout, ctx, NULL);
        dropt_free_context(ctx);
        exit(EXIT_SUCCESS);
    }

    dropt_free_context(ctx);

    // Apply the negating switches onto the defaults.
    if (windowed) fullscreen = false;
    if (no_vsync) vsync = false;
    if (prof) profile = true;
    if (no_focus) no_keyfocus_steal = true;
    if (dens_near) densityNearest = true;
    if (nomouse) no_mouse = true;

    // Guard against values that would crash or hang the sim.
    if (P < 1)
    {
        fprintf(stderr, "goo: --particles must be >= 1\n");
        exit(EXIT_FAILURE);
    }
    if (width < 1 || height < 1)
    {
        fprintf(stderr, "goo: --width/--height must be >= 1\n");
        exit(EXIT_FAILURE);
    }
    if (render_scale < 1.0)
    {
        fprintf(stderr, "goo: --render-scale must be >= 1\n");
        exit(EXIT_FAILURE);
    }
    if (densityBufferDownsampling < 1 || trailBufferDownsampling < 1)
    {
        fprintf(stderr, "goo: --dens-downsample/--trail-downsample must be >= 1\n");
        exit(EXIT_FAILURE);
    }
    if (densitySubsample < 1 || trailSubsample < 1)
    {
        fprintf(stderr, "goo: --dens-sub/--trail-sub must be >= 1\n");
        exit(EXIT_FAILURE);
    }
}

//========================================
//
//  ###    ###    ###    ##  ##     ##
//  ## #  # ##   ## ##   ##  ####   ##
//  ##  ##  ##  ##   ##  ##  ##  ## ##
//  ##      ##  #######  ##  ##    ###
//  ##      ##  ##   ##  ##  ##     ##
//
//========================================

int main(int argc, char **argv)
{
    parse_args(argc, argv);

    window_setup();
    shader_setup();
    buffer_setup();

    // Write uniforms to shaders
    shader_set_uniform_int(&screenShader, "density_buffer", densityBuffer.current);
    shader_set_uniform_float(&screenShader, "cull_amount", (float)cullAmount);
    shader_set_uniform_float(&screenShader, "render_headroom", (float)renderHeadroom);
    shader_set_uniform_float(&screenShader, "render_gamma", (float)renderGamma);
    shader_set_uniform_int(&densityShader, "density_buffer_downsampling", densityBufferDownsampling);
    shader_set_uniform_float(&densityShader, "density_alpha", densityAlpha * densitySubsample);
    shader_set_uniform_float(&densityShader, "kernel_radius", kernelRadius);
    shader_set_uniform_float(&velocityShader, "drag_coefficient", dragCoefficient);
    shader_set_uniform_float(&velocityShader, "dither_coefficient", ditherCoefficient);
    shader_set_uniform_int(&velocityShader, "density_buffer", densityBuffer.current);
    // Trail spread-deposit (--trail-every N): the trail decays EVERY frame (smooth fade,
    // no frozen-field discontinuity), but each frame deposits only a rotating 1/N subset
    // of the trail particles. With the per-point intensity scaled by N, the per-frame
    // deposit total -- and so the field the physics sees -- is unchanged, while the
    // expensive point scatter drops ~N x. Decay alpha stays per-frame (no compounding).
    shader_set_uniform_float(&copyShader, "alpha", trailAlpha);
    shader_set_uniform_int(&trailShader, "trail_buffer_downsampling", trailBufferDownsampling);
    shader_set_uniform_float(&trailShader, "trail_intensity", trailIntensity * trailSubsample * trail_every);
    shader_set_uniform_float(&trailShader, "velocity_floor", trailVelocityFloor);
    shader_set_uniform_float(&trailShader, "kernel_radius", trailRadius);

    i32 xpos = 0;
    i32 ypos = 0;
    bool mouse_in_window = RGFW_window_isMouseInside(window);
    RGFW_window_getMouse(window, &xpos, &ypos);
    float mouse_position[] = {(float)xpos * mouse_scale, (float)ypos * mouse_scale};
    shader_set_uniform_vec(&velocityShader, "mouse_position", 2, mouse_position);

    // Position and velocity double buffer pointers

    // Loop counter passed to the shaders for use in random()
    int epoch_counter = 0;

    // Physics timing preamble
    float exp_average_flip_time = 0.0f;
    float alpha_flip_time = 0.1;

    // Per-pass GPU timing (only populated under --profile); see LAP macro.
    double pass_ms[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const char *pass_name[8] = {"vel", "pos", "dens", "trail", "scr", "up", "rdbk", "sort"};

    // True pipelined frame time (no glFinish); printed every 60 frames when not
    // profiling, so --no-vsync shows real fps without the serialisation penalty.
    double frame_prev = get_time();
    double frame_ms = 0.0;
    double fps_sleep_ms = 0.0; // --fps-cap pacing: slack we sleep each frame

    //======================================
    //
    //  ##       #####    #####   #####
    //  ##      ##   ##  ##   ##  ##  ##
    //  ##      ##   ##  ##   ##  #####
    //  ##      ##   ##  ##   ##  ##
    //  ######   #####    #####   ##
    //
    //======================================

    while (!RGFW_window_shouldClose(window))
    {
        if (max_iterations > 0 && epoch_counter >= max_iterations) break;


        // Poll mouse position (--no-mouse parks it far off so the repel never triggers)
        // and the per-frame velocity (delta vs last frame, same sim units as position).
        static float prev_mouse_position[2] = {-1e9f, -1e9f};
        float mouse_position[2] = {-1e9f, -1e9f};
        float mouse_velocity[2] = {0.0f, 0.0f};
        if (!no_mouse && mouse_in_window)
        {
            RGFW_window_getMouse(window, &xpos, &ypos);
            mouse_position[0] = (float)xpos * mouse_scale;
            mouse_position[1] = (float)ypos * mouse_scale;
            // velocity only if the previous frame was also in-window -- otherwise the delta
            // is the teleport from the parked sentinel (re-entry jump), not a real motion.
            if (prev_mouse_position[0] > -1e8f)
            {
                mouse_velocity[0] = mouse_position[0] - prev_mouse_position[0];
                mouse_velocity[1] = mouse_position[1] - prev_mouse_position[1];
            }
        }
        prev_mouse_position[0] = mouse_position[0];
        prev_mouse_position[1] = mouse_position[1];

        double _tp;
        if (profile) { glFinish(); _tp = get_time(); }

        // Velocity pass
        shader_use(&velocityShader);
        shader_set_uniform_vec(&velocityShader, "mouse_position", 2, mouse_position);
        shader_set_uniform_vec(&velocityShader, "mouse_velocity", 2, mouse_velocity);
        shader_set_uniform_int(&velocityShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&velocityShader, "velocity_buffer", velocityBuffer.current);
        shader_set_uniform_int(&velocityShader, "trail_buffer", trailBuffer.current);
        shader_set_uniform_int(&velocityShader, "epoch_counter", epoch_counter);
        buffer_bind(&velocityBuffer, other);
        buffer_update(&velocityBuffer);
        buffer_flip(&velocityBuffer);
        LAP(0);

        // Position pass
        shader_use(&positionShader);
        shader_set_uniform_int(&positionShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&positionShader, "velocity_buffer", velocityBuffer.current); // read from updated velocity buffer
        // shader_set_uniform_int(&positionShader, "epoch_counter", epoch_counter);
        buffer_bind(&positionBuffer, other);
        // buffer_update(&positionBuffer);
        buffer_update(&positionBuffer);
        buffer_flip(&positionBuffer);
        LAP(1);

        // Copy the fresh position + velocity textures into PBOs (FBO -> GL_PIXEL_PACK_BUFFER,
        // stays on-gpu). Those PBOs are bound as the point passes' vertex attributes, and
        // pbo_pos is also CPU-mapped to build the tile-coherent draw order.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, positionBuffer.framebuffers[positionBuffer.current]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_pos);
        glReadPixels(0, 0, PB_width, PB_height, GL_RG, GL_FLOAT, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, velocityBuffer.framebuffers[velocityBuffer.current]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_vel);
        glReadPixels(0, 0, PB_width, PB_height, GL_RG, GL_FLOAT, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        LAP(6);

        // Rebuild the tile-coherent draw order from the freshly read-back positions.
        if (sort_every > 0 && epoch_counter % sort_every == 0)
        {
            double s0 = get_time();
            rebuild_sort_order();
            pass_ms[7] = ema(pass_ms[7], (get_time() - s0) * 1000.0, 0.1);
        }

        // Density buffer pass. Amortised: the density field is a slow-moving snapshot,
        // and the buffer isn't flipped, so on skipped frames it just keeps the last
        // rebuild (1..N-1 frame stale -- negligible to the velocity integral that reads
        // it). buffer_bind clears, so skip the whole block (not just the draw).
        if (dens_every <= 1 || epoch_counter % dens_every == 0)
        {
            shader_use(&densityShader);
            shader_set_uniform_int(&densityShader, "density_buffer_downsampling", densityBufferDownsampling);
            buffer_bind(&densityBuffer, current);
            glBlendFunc(GL_ONE, GL_ONE); // additive density accumulation (unbounded)
            buffer_update_n(&densityBuffer, P / densitySubsample);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // restore the global blend

            // Auto-headroom: read the fresh (tiny) density field back, take a high
            // percentile (robust to outlier hot-spots, unlike the raw max), and EMA it --
            // the envelope follower that sets the colormap's makeup gain.
            if (renderHeadroom <= 0.0)
            {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[densityBuffer.current]);
                glReadPixels(0, 0, density_width, density_height, GL_RED, GL_FLOAT, density_readback);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                int n = density_width * density_height;
                qsort(density_readback, n, sizeof(float), cmp_float);
                int k = (int)(headroomPct * (n - 1));
                if (k < 0) k = 0; else if (k >= n) k = n - 1;
                float level = density_readback[k];
                headroom_ema = (float)ema(headroom_ema, level * headroomMargin, headroomAttack);
            }
        }
        LAP(2);

        // Trail buffer. Decay every frame (smooth), but deposit only a rotating 1/N
        // subset of the trail particles (--trail-every N), cycling the contiguous block
        // by frame. Particle ids aren't spatially sorted, so each block is a fair random
        // subset; over N frames all contribute. No frozen field -> no discontinuity.
        buffer_bind(&trailBuffer, other);

        shader_use(&copyShader); // First pass (decay the previous trail, every frame)
        shader_set_uniform_float(&copyShader, "alpha", trailAlpha);
        shader_set_uniform_int(&copyShader, "source_buffer", trailBuffer.current);
        buffer_update(&trailBuffer);

        shader_use(&trailShader); // Second pass (deposit this frame's rotating subset)
        int trail_total = P / trailSubsample;
        int trail_block = trail_every <= 1 ? trail_total : trail_total / trail_every;
        int trail_start = trail_every <= 1 ? 0 : (epoch_counter % trail_every) * trail_block;
        glDrawArrays(GL_POINTS, trail_start, trail_block);
        buffer_flip(&trailBuffer);
        LAP(3);

        // Screen rendering pass -> internal render target (render resolution)
        buffer_bind(&renderBuffer, current);

        shader_use(&screenShader);
        // Headroom: auto-tracked envelope (renderHeadroom == 0) or the fixed override.
        shader_set_uniform_float(&screenShader, "render_headroom",
                                 renderHeadroom > 0.0 ? (float)renderHeadroom : headroom_ema);
        // Draw in tile-coherent order via the index buffer (ibo stays bound in the VAO).
        glDrawElements(GL_POINTS, P, GL_UNSIGNED_INT, 0);
        LAP(4);

        // View density buffer
        // shader_use(&copyShader);
        // shader_set_uniform_int(&copyShader, "source_buffer", densityBuffer.current);
        // shader_set_uniform_float(&copyShader, "alpha", 1.0f);
        // buffer_update(&renderBuffer);

        // View trail buffer
        // shader_use(&copyShader);
        // shader_set_uniform_int(&copyShader, "source_buffer", trailBuffer.current);
        // shader_set_uniform_float(&copyShader, "alpha", 1.0f);
        // buffer_update(&renderBuffer);

        // Sync the GL drawable + upscale viewport to the window's CURRENT geometry, as late
        // as possible (right before the only pass that touches the window). The window can
        // change backing scale mid-frame when dragged across monitors of different dpi; the
        // passes above are render-res (dpi-independent), so syncing here -- not at the top of
        // the loop -- closes the 1-frame gap where the view grew but our blit hadn't, which
        // showed the window background. [update] re-fits the drawable; it's a no-op when stable.
        RGFW_window_updateContext_OpenGL(window);
        {
            i32 fbw = 0, fbh = 0;
            RGFW_window_getSizeInPixels(window, &fbw, &fbh);
            window_width = fbw;
            window_height = fbh;
            screenBuffer.width = window_width;
            screenBuffer.height = window_height;
        }

        // Upscale pass: nearest-sample the render target onto the window. Blending
        // is off -- the render target already holds particles composited over black,
        // so this is a straight opaque blit (chunky when render_scale > 1).
        buffer_bind(&screenBuffer, screen);
        glDisable(GL_BLEND);
        shader_use(&upscaleShader);
        shader_set_uniform_int(&upscaleShader, "source_buffer", renderBuffer.current);
        buffer_update(&screenBuffer);
        glEnable(GL_BLEND);

        // Mouse debug overlay: green dot at cursor + line from prev to current position.
        // Use raw logical window coords (xpos/ypos) so the overlay matches the actual cursor.
        static float debug_prev[2] = {0.0f, 0.0f};
        if (mouse_debug && mouse_in_window)
        {
            float win_shape[2] = {(float)window_width, (float)window_height};
            float debug_cur[2] = {(float)xpos * dpi_scale, (float)ypos * dpi_scale};
            float positions[4] = {debug_prev[0], debug_prev[1], debug_cur[0], debug_cur[1]};
            shader_use(&debugShader);
            shader_set_uniform_vec(&debugShader, "window_shape", 2, win_shape);
            glUniform2fv(glGetUniformLocation(debugShader.program, "positions"), 2, positions);
            // NOTE: GL_PROGRAM_POINT_SIZE is enabled once globally at setup and the density/
            // trail splat passes depend on it -- don't toggle it here or those passes collapse
            // to 1px points and the whole field jumps. gl_PointSize=8 in debug.vert still applies.
            glDrawArrays(GL_LINES, 0, 2);   // trail from prev to current
            glDrawArrays(GL_POINTS, 1, 1);  // dot at current (index 1)
            debug_prev[0] = debug_cur[0];
            debug_prev[1] = debug_cur[1];
        }
        LAP(5);

        if (profile && epoch_counter % 60 == 0)
        {
            double tot = 0;
            fprintf(stdout, "ms:");
            for (int i = 0; i < 8; i++)
            {
                fprintf(stdout, " %s=%.2f", pass_name[i], pass_ms[i]);
                tot += pass_ms[i];
            }
            fprintf(stdout, " | total=%.2f (%.0f fps)\n", tot, tot > 0 ? 1000.0 / tot : 0);
        }

        double frame_now = get_time();
        frame_ms = ema(frame_ms, (frame_now - frame_prev) * 1000.0, 0.1);
        frame_prev = frame_now;
        if (!profile && epoch_counter % 60 == 0)
        {
            fprintf(stdout, "frame=%.2f ms (%.0f fps)\n", frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0);
        }

        float flip_buffer_start = get_time();
        RGFW_window_swapBuffers_OpenGL(window); // Swap draw and screen buffer
        float delta_flip_time = (get_time() - flip_buffer_start) * 1000;
        exp_average_flip_time == 0.0f
            ? exp_average_flip_time = delta_flip_time
            : (exp_average_flip_time = alpha_flip_time * delta_flip_time + (1 - alpha_flip_time) * exp_average_flip_time);
        // fprintf(stdout, "epoch: %03d buffer flip time: %.2f ms\n", epoch_counter, exp_average_flip_time);

        // Poll and dispatch window events. Escape-to-quit is handled by the
        // exit key set in window_setup, so we only need to react to resizes here.
        RGFW_event event;
        while (RGFW_window_checkEvent(window, &event))
        {
            if (event.type == RGFW_windowResized)
            {
                handle_framebuffer_resize(event.update.w, event.update.h);
            }
            else if (event.type == RGFW_mouseLeave)
            {
                mouse_in_window = false;
            }
            else if (event.type == RGFW_mouseEnter)
            {
                mouse_in_window = true;
            }
            // 'q' quits, same as escape (escape handled via exit key)
            else if (event.type == RGFW_keyPressed && event.key.value == RGFW_keyQ)
            {
                RGFW_window_setShouldClose(window, RGFW_TRUE);
            }
        }

        // --fps-cap: nudge a per-frame sleep so the EMA frame time settles at the
        // target. frame_ms (measured top-to-top) already includes this sleep, so a
        // simple proportional controller converges. Clamps at 0 when we're already
        // slower than target -- no slack to give back.
        // ponytail: plain P-controller, fine for a soft cap; add I/D if it hunts.
        if (fps_cap > 0)
        {
            double target_ms = 1000.0 / fps_cap;
            fps_sleep_ms += (target_ms - frame_ms) * 0.5;
            if (fps_sleep_ms < 0.0) fps_sleep_ms = 0.0;
            sleep_ms(fps_sleep_ms);
        }
        epoch_counter++;
    }

    // Final benchmark summary (one parseable line at exit, e.g. after -N frames).
    if (profile)
    {
        double tot = 0;
        fprintf(stdout, "SUMMARY ms:");
        for (int i = 0; i < 8; i++)
        {
            fprintf(stdout, " %s=%.2f", pass_name[i], pass_ms[i]);
            tot += pass_ms[i];
        }
        fprintf(stdout, " | total=%.2f (%.0f fps)\n", tot, tot > 0 ? 1000.0 / tot : 0);
    }
    else
    {
        fprintf(stdout, "SUMMARY frame=%.2f ms (%.0f fps)\n", frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0);
    }

    // Debug: dump the final frame + density/trail fields for inspection (--dump).
    if (dump_path)
    {
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
    glDeleteTextures(8, textures);
    glDeleteFramebuffers(8, framebuffers);
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

void window_setup()
{
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
    if (no_keyfocus_steal) flags |= RGFW_windowNoFocusOnCreate;
    // Same-space fullscreen path (--no-keyfocus-steal): cover the monitor with a
    // borderless window in the *current* Space instead of native fullscreen, which
    // would open a new Space and yank focus. Born borderless so no resize dance.
    if (fullscreen && no_keyfocus_steal) flags |= RGFW_windowNoBorder;
    // Windowed mode defaults to a normal bordered window. --no-border removes
    // the title bar / border when the user explicitly wants that look.
    if (!fullscreen && no_border) flags |= RGFW_windowNoBorder;

    // Window spawn position. In fullscreen we place the window at the target
    // monitor's origin so RGFW_window_setFullscreen (which fullscreens onto
    // whichever monitor the window is on) picks the right one. In windowed mode
    // we center it on the primary instead.
    int win_x = 0;
    int win_y = 0;

    if (fullscreen)
    {
        size_t monitorCount;
        RGFW_monitor **monitors = RGFW_getMonitors(&monitorCount);
        printf("%zu monitors found\n", monitorCount);
        if (monitorCount == 0)
        {
            fprintf(stderr, "No monitors found\n");
            exit(EXIT_FAILURE);
        }
        if ((size_t)whichMonitor >= monitorCount)
        {
            whichMonitor = 0;
        };
        printf("using monitor %d\n", whichMonitor);
        RGFW_monitor *monitor = monitors[whichMonitor];
        win_x = monitor->x;
        win_y = monitor->y;
        width = monitor->mode.w;
        height = monitor->mode.h;
        printf("%d %d\n", width, height);
        density_width = width / densityBufferDownsampling + 1;
        density_height = height / densityBufferDownsampling + 1;
        trail_width = width / trailBufferDownsampling + 1;
        trail_height = height / trailBufferDownsampling + 1;
    }
    else
    {
        if (whichMonitor != 0)
        {
            size_t monitorCount;
            RGFW_monitor **monitors = RGFW_getMonitors(&monitorCount);
            if ((size_t)whichMonitor < monitorCount)
            {
                RGFW_monitor *monitor = monitors[whichMonitor];
                win_x = monitor->x + (monitor->mode.w - width) / 2;
                win_y = monitor->y + (monitor->mode.h - height) / 2;
                printf("monitor %d: origin=(%d,%d) size=(%d,%d) -> win=(%d,%d)\n",
                       whichMonitor, monitor->x, monitor->y,
                       monitor->mode.w, monitor->mode.h, win_x, win_y);
            }
        }
        else
        {
            flags |= RGFW_windowCenter;
        }
    }

    window = RGFW_createWindow(title, win_x, win_y, width, height, flags);

    if (!window)
    {
        fprintf(stderr, "Failed to create RGFW window\n");
        exit(EXIT_FAILURE);
    }

    if (!fullscreen && whichMonitor != 0)
        RGFW_window_move(window, win_x, win_y);

    // Fullscreen has two modes: same-space borderless cover (no focus steal) vs
    // native fullscreen Space (steals focus by design). See each branch.
    if (fullscreen && no_keyfocus_steal)
    {
        // Same-space cover: no native toggleFullScreen (which opens a new Space and
        // steals focus). Cover the display in the current Space, above the menu bar.
        // NoFocusOnCreate keeps key focus off at launch, so the keyboard stays with
        // whatever the user was doing. Clicking the window grabs key focus on demand
        // (canBecomeKeyWindow override in RGFW.h), which enables escape-to-quit;
        // benchmarks that never click stay hands-off and use -N to auto-exit.
        RGFW_window_move(window, win_x, win_y);
        RGFW_window_coverDisplay(window);
    }
    else if (fullscreen)
    {
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

    // The actual framebuffer (window_width/height) is in device pixels and is only
    // the final upscale target. The sim renders at render resolution (width/height)
    // = logical points / render_scale, so on hidpi/retina it renders at 1x rather
    // than paying the device-pixel fill cost. The mouse comes in logical points, so
    // remember the ratio to scale it into render space.
    i32 fb_width, fb_height, logical_width, logical_height;
    RGFW_window_getSizeInPixels(window, &fb_width, &fb_height);
    RGFW_window_getSize(window, &logical_width, &logical_height);
    window_width = fb_width;
    window_height = fb_height;
    width = (int)((float)logical_width / render_scale + 0.5f);
    height = (int)((float)logical_height / render_scale + 0.5f);
    mouse_scale = (logical_width > 0) ? (float)width / (float)logical_width : 1.0f;
    dpi_scale   = (logical_width > 0) ? (float)fb_width / (float)logical_width : 1.0f;

    // Quit when escape is pressed (reported through RGFW_window_shouldClose)
    RGFW_window_setExitKey(window, RGFW_keyEscape);

    RGFW_window_swapInterval_OpenGL(window, vsync ? 1 : 0); // --no-vsync disables

    // GLAD
    if (!gladLoadGL((GLADloadfunc)RGFW_getProcAddress_OpenGL))
    {
        fprintf(stderr, "Failed to initialize GLAD\n");
        exit(EXIT_FAILURE);
    }

    // Enable point size rendering and alpha blending
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void handle_framebuffer_resize(int new_width, int new_height)
{
    // The resize event carries logical points. Query the GL framebuffer (device px)
    // for the upscale target, recompute the render resolution from logical points /
    // render_scale, and refresh mouse_scale in case dpi changed.
    (void)new_width;
    (void)new_height;
    i32 fb_width, fb_height, logical_width, logical_height;
    RGFW_window_getSizeInPixels(window, &fb_width, &fb_height);
    RGFW_window_getSize(window, &logical_width, &logical_height);
    window_width = fb_width;
    window_height = fb_height;
    width = (int)((float)logical_width / render_scale + 0.5f);
    height = (int)((float)logical_height / render_scale + 0.5f);
    mouse_scale = (logical_width > 0) ? (float)width / (float)logical_width : 1.0f;
    dpi_scale   = (logical_width > 0) ? (float)fb_width / (float)logical_width : 1.0f;
    glViewport(0, 0, window_width, window_height);
    updateShaderWindowShape(width, height);
    buffer_reallocate(&renderBuffer, current, width, height);
    density_width = width / densityBufferDownsampling + 1;
    density_height = height / densityBufferDownsampling + 1;
    buffer_reallocate(&densityBuffer, current, density_width, density_height);
    trail_width = width / trailBufferDownsampling + 1;
    trail_height = height / trailBufferDownsampling + 1;
    buffer_reallocate(&trailBuffer, current, trail_width, trail_height);
    buffer_reallocate(&trailBuffer, other, trail_width, trail_height);
}

//=====================================================
//
//   ####  ##   ##    ###    ####    #####  #####
//  ##     ##   ##   ## ##   ##  ##  ##     ##  ##
//   ###   #######  ##   ##  ##  ##  #####  #####
//     ##  ##   ##  #######  ##  ##  ##     ##  ##
//  ####   ##   ##  ##   ##  ####    #####  ##   ##
//
//=====================================================

void shader_setup()
{
    fprintf(stdout, "Compiling shaders...\n");
    shader_create(&screenShader);
    shader_compile(&screenShader, GL_VERTEX_SHADER, screen_VertexShaderSource);
    shader_compile(&screenShader, GL_FRAGMENT_SHADER, screen_FragmentShaderSource);
    shader_link(&screenShader);

    shader_create(&densityShader);
    shader_compile(&densityShader, GL_VERTEX_SHADER, density_VertexShaderSource);
    shader_compile(&densityShader, GL_FRAGMENT_SHADER, density_FragmentShaderSource);
    shader_link(&densityShader);

    shader_create(&positionShader);
    shader_compile(&positionShader, GL_VERTEX_SHADER, position_VertexShaderSource);
    shader_compile(&positionShader, GL_FRAGMENT_SHADER, position_FragmentShaderSource);
    shader_link(&positionShader);

    shader_create(&velocityShader);
    shader_compile(&velocityShader, GL_VERTEX_SHADER, velocity_VertexShaderSource);
    shader_compile(&velocityShader, GL_FRAGMENT_SHADER, velocity_FragmentShaderSource);
    shader_link(&velocityShader);

    shader_create(&copyShader);
    shader_compile(&copyShader, GL_VERTEX_SHADER, copy_VertexShaderSource);
    shader_compile(&copyShader, GL_FRAGMENT_SHADER, copy_FragmentShaderSource);
    shader_link(&copyShader);

    shader_create(&trailShader);
    shader_compile(&trailShader, GL_VERTEX_SHADER, trail_VertexShaderSource);
    shader_compile(&trailShader, GL_FRAGMENT_SHADER, trail_FragmentShaderSource);
    shader_link(&trailShader);

    shader_create(&upscaleShader);
    shader_compile(&upscaleShader, GL_VERTEX_SHADER, upscale_VertexShaderSource);
    shader_compile(&upscaleShader, GL_FRAGMENT_SHADER, upscale_FragmentShaderSource);
    shader_link(&upscaleShader);

    shader_create(&debugShader);
    shader_compile(&debugShader, GL_VERTEX_SHADER, debug_VertexShaderSource);
    shader_compile(&debugShader, GL_FRAGMENT_SHADER, debug_FragmentShaderSource);
    shader_link(&debugShader);

    updateShaderWindowShape(width, height);
}

void updateShaderWindowShape(int new_width, int new_height)
{
    float window_shape[2] = {(float)new_width, (float)new_height};
    shader_set_uniform_vec(&screenShader, "window_shape", 2, window_shape);
    shader_set_uniform_vec(&densityShader, "window_shape", 2, window_shape);
    shader_set_uniform_vec(&positionShader, "window_shape", 2, window_shape);
    shader_set_uniform_vec(&velocityShader, "window_shape", 2, window_shape);
    // shader_set_uniform_vec(&copyShader, "window_shape", 2, window_shape);
    shader_set_uniform_vec(&trailShader, "window_shape", 2, window_shape);

    // window_shape is the render resolution (new_width/height). The screen buffer is
    // the final upscale target, so it takes the actual window (framebuffer) size.
    screenBuffer.width = window_width;
    screenBuffer.height = window_height;
}

void updateShaderBufferShape(int new_width, int new_height)
{
    // buffer_size is dead: the point passes now read positions as PBO attributes
    // instead of mapping gl_VertexID -> texel, so no shader references it anymore.
    (void)new_width;
    (void)new_height;
}

//===================================================
//
//  #####   ##   ##  #####  #####  #####  #####
//  ##  ##  ##   ##  ##     ##     ##     ##  ##
//  #####   ##   ##  #####  #####  #####  #####
//  ##  ##  ##   ##  ##     ##     ##     ##  ##
//  #####    #####   ##     ##     #####  ##   ##
//
//===================================================

void buffer_setup()
{
    fprintf(stdout, "Setting up buffers...\n");

    // Derived buffer sizes, now that width/height are final (set in window_setup)
    density_width = width / densityBufferDownsampling + 1;
    density_height = height / densityBufferDownsampling + 1;
    trail_width = width / trailBufferDownsampling + 1;
    trail_height = height / trailBufferDownsampling + 1;

    // Vertex array. The point passes get their attributes from the PBOs (wired below);
    // the fullscreen-quad passes use gl_VertexID -- so the VAO needs no static buffer.
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // Calculate size of the physics buffer
    int PBwidth = ceil(sqrt(P));
    int PBheight = ceil(P / sqrt(P));
    PB_width = PBwidth;
    PB_height = PBheight;
    fprintf(stdout, "%d points\n", P);
    fprintf(stdout, "Physics framebuffer shape: %d x %d\n", PBwidth, PBheight);
    updateShaderBufferShape(PBwidth, PBheight);

    // PBOs sized to the full physics buffer (RG = 2 floats/texel). Filled each frame
    // by glReadPixels(FBO->PBO), then reused as GL_ARRAY_BUFFER so the point passes
    // read positions/velocities as streamed attributes instead of vertex texture fetch.
    size_t pb_bytes = (size_t)PBwidth * PBheight * 2 * sizeof(float);
    glGenBuffers(1, &pbo_pos);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_pos);
    glBufferData(GL_PIXEL_PACK_BUFFER, pb_bytes, NULL, GL_DYNAMIC_COPY);
    glGenBuffers(1, &pbo_vel);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_vel);
    glBufferData(GL_PIXEL_PACK_BUFFER, pb_bytes, NULL, GL_DYNAMIC_COPY);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // Wire the PBOs as vertex attributes (position = loc 0, velocity = loc 1) on the
    // still-bound VAO, so the point passes stream them instead of vertex texture fetch.
    glBindBuffer(GL_ARRAY_BUFFER, pbo_pos);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, pbo_vel);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Index buffer + CPU scratch for the tile-coherent draw order (rebuild_sort_order).
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)PBwidth * PBheight * sizeof(unsigned int), NULL, GL_DYNAMIC_DRAW);
    {
        int tilesW = (width + sort_tile - 1) / sort_tile;
        int tilesH = (height + sort_tile - 1) / sort_tile;
        sort_idx = (unsigned int *)malloc((size_t)P * sizeof(unsigned int));
        sort_key = (int *)malloc((size_t)P * sizeof(int));
        sort_counts = (int *)malloc(((size_t)tilesW * tilesH + 1) * sizeof(int));
        // Identity order until the first rebuild, so frame 0 still draws every particle.
        for (int i = 0; i < P; i++) sort_idx[i] = (unsigned int)i;
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (size_t)P * sizeof(unsigned int), sort_idx);
    }

    // Initalise textures and the associated framebuffers
    glGenTextures(8, textures);
    glGenFramebuffers(8, framebuffers);

    // Texture 0 - Density buffer
    densityBuffer.minmag_filter = densityNearest ? GL_NEAREST : GL_LINEAR; // --density-nearest for chunky pixels
    densityBuffer.wrap_st = GL_REPEAT;
    densityBuffer.dim = BE_1D;
    buffer_allocate(&densityBuffer, current, densityBufferIndex, density_width, density_height, NULL);
    // Scratch for the auto-headroom: read the (tiny, downsampled) density buffer back to
    // find its max each density update. Sized generously so a resize can't overflow it.
    density_readback = (float *)malloc((size_t)(density_width + 8) * (density_height + 8) * sizeof(float));
    headroom_ema = (float)(renderHeadroom > 0.0 ? renderHeadroom : 50.0); // seed the follower

    positionBuffer.minmag_filter = GL_NEAREST;
    positionBuffer.dim = BE_2D;
    velocityBuffer.minmag_filter = GL_NEAREST;
    velocityBuffer.dim = BE_2D;

    // Texture 1 - Position buffer 1
    // --seed makes the initial positions (and so the whole deterministic gpu evolution)
    // reproducible, which is what lets before/after dumps actually be comparable.
    srand(rng_seed ? (unsigned int)rng_seed : (unsigned int)time(NULL));
    float margin = (width < height ? width : height) * 0.1f;
    float noise_seed = rng_seed ? (float)rng_seed : (10 * get_time() + frand01());
    fprintf(stdout, "Generating starting positions...\n");
    int N = PBwidth * PBheight * 2;
    float *positions = (float *)malloc(N * sizeof(float));
    for (int i = 0; i < N; i += 2)
    {
        // Generate random position in unit range
        vec2 position = {frand01(), frand01()};
        vec3 p0 = {10 * position[0], 10 * position[1], noise_seed};
        vec3 p1 = {10 * position[0], 10 * position[1], noise_seed + 1};
        vec2 noise = {glm_perlin_vec3(p0), glm_perlin_vec3(p1)};
        position[0] += 0.1f * fmodf(noise[0], 1.0f);
        position[1] += 0.1f * fmodf(noise[1], 1.0f);

        // Cull position to the center of the screen
        position[0] = position[0] * (width - 2 * margin) + margin;
        position[1] = position[1] * (height - 2 * margin) + margin;

        positions[i] = position[0];
        positions[i + 1] = position[1];
    }

    fprintf(stdout, "Allocating buffers...\n");
    buffer_allocate(&positionBuffer, current, positionBufferIndex1, PBwidth, PBheight, (const char *)positions);

    free(positions);

    // Texture 2,3,4 - position buffer 2, velocity buffer 1 and 2
    buffer_allocate(&positionBuffer, other, positionBufferIndex2, PBwidth, PBheight, NULL);
    buffer_allocate(&velocityBuffer, current, velocityBufferIndex1, PBwidth, PBheight, NULL);
    buffer_allocate(&velocityBuffer, other, velocityBufferIndex2, PBwidth, PBheight, NULL);

    // Texture 5,6 - Trail double buffer
    trailBuffer.minmag_filter = GL_NEAREST;
    trailBuffer.wrap_st = GL_REPEAT;
    trailBuffer.dim = BE_1D;
    buffer_allocate(&trailBuffer, current, trailBufferIndex1, trail_width, trail_height, NULL);
    buffer_allocate(&trailBuffer, other, trailBufferIndex2, trail_width, trail_height, NULL);

    // Texture 7 - internal render target (rgba colour) at render resolution.
    // GL_NEAREST so the upscale to the window stays crisp/pixelated at low res.
    renderBuffer.minmag_filter = GL_NEAREST;
    renderBuffer.wrap_st = GL_CLAMP_TO_EDGE;
    renderBuffer.dim = BE_4D;
    buffer_allocate(&renderBuffer, current, renderBufferIndex, width, height, NULL);

    // The screen buffer is the window itself (the upscale target), in device px.
    screenBuffer.width = window_width;
    screenBuffer.height = window_height;
}

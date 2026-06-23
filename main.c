#include "shader.h"
#include "buffer.h"

#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

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
double render_scale = 1.0; // internal res = logical points / render_scale (>1 = lower res, faster); --render-scale
float mouse_scale = 1.0f;  // logical-point -> render-resolution scale
// int width = 400; int height = 400;
const char *title = "Pixel Goo";
// runtime-tunable via cli (see parse_args); defaults below
bool fullscreen = true;
bool vsync = true;
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

GLuint vertex_buffer;

Shader screenShader = {.name = "screenShader"};
Shader densityShader = {.name = "densityShader"};
Shader positionShader = {.name = "positionShader"};
Shader velocityShader = {.name = "velocityShader"};
Shader copyShader = {.name = "copyShader"};
Shader trailShader = {.name = "trailShader"};
Shader upscaleShader = {.name = "upscaleShader"};

// Include shader source files
#include "copy.h"
#include "trail.h"
#include "screen.h"
#include "density.h"
#include "position.h"
#include "velocity.h"
#include "upscale.h"

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

// This can be quite a lot because the density buffer is lerped and particles dither
// const int densityBufferDownsampling = 10;
const int densityBufferDownsampling = 20;
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
const int trailBufferDownsampling = 10;
// const int trailBufferDownsampling = 20;
const float trailVelocityFloor = 0.6;
int trail_width = 0;
int trail_height = 0;

// Particles
// const int P = 16384; // <- render buffer max
// const int P = 30000;
// const int P = 160000;
// const int P = 200000;
// const int P = 300000;
int P = 500000; // --particles
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

// Uniform random float in [0, 1]; replaces glm::linearRand
static float frand01()
{
    return (float)rand() / (float)RAND_MAX;
}

// Parse command-line options into the runtime globals above. Exits the process
// on --help or on a bad option; returns normally to let the sim start.
static void parse_args(int argc, char **argv)
{
    dropt_bool show_help = 0;
    dropt_bool windowed = 0;
    dropt_bool no_vsync = 0;

    dropt_option options[] = {
        {'h', "help", "Show this help and exit.", NULL,
         dropt_handle_bool, &show_help, dropt_attr_halt},
        {'p', "particles", "Number of particles to simulate.", "N",
         dropt_handle_int, &P},
        {'m', "monitor", "Index of the monitor to fullscreen onto.", "N",
         dropt_handle_int, &whichMonitor},
        {'\0', "windowed", "Run in a window instead of fullscreen.", NULL,
         dropt_handle_bool, &windowed},
        {'\0', "width", "Window width in pixels (windowed mode).", "N",
         dropt_handle_int, &width},
        {'\0', "height", "Window height in pixels (windowed mode).", "N",
         dropt_handle_int, &height},
        {'\0', "no-vsync", "Disable vsync (uncap the frame rate).", NULL,
         dropt_handle_bool, &no_vsync},
        {'r', "render-scale", "Internal render resolution divisor (>1 = lower res, faster, chunkier).", "N",
         dropt_handle_double, &render_scale},
        {0} // sentinel
    };

    dropt_context *ctx = dropt_new_context(options);
    if (ctx == NULL)
    {
        fprintf(stderr, "goo: out of memory setting up option parser\n");
        exit(EXIT_FAILURE);
    }

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
    shader_set_uniform_int(&densityShader, "density_buffer_downsampling", densityBufferDownsampling);
    shader_set_uniform_float(&densityShader, "density_alpha", densityAlpha);
    shader_set_uniform_float(&densityShader, "kernel_radius", kernelRadius);
    shader_set_uniform_float(&velocityShader, "drag_coefficient", dragCoefficient);
    shader_set_uniform_float(&velocityShader, "dither_coefficient", ditherCoefficient);
    shader_set_uniform_int(&velocityShader, "density_buffer", densityBuffer.current);
    shader_set_uniform_float(&copyShader, "alpha", trailAlpha);
    shader_set_uniform_int(&trailShader, "trail_buffer_downsampling", trailBufferDownsampling);
    shader_set_uniform_float(&trailShader, "trail_intensity", trailIntensity);
    shader_set_uniform_float(&trailShader, "velocity_floor", trailVelocityFloor);
    shader_set_uniform_float(&trailShader, "kernel_radius", trailRadius);

    i32 xpos = 0;
    i32 ypos = 0;
    RGFW_window_getMouse(window, &xpos, &ypos);
    float mouse_position[] = {(float)xpos * mouse_scale, (float)ypos * mouse_scale};
    shader_set_uniform_vec(&velocityShader, "mouse_position", 2, mouse_position);

    // Position and velocity double buffer pointers

    // Loop counter passed to the shaders for use in random()
    int epoch_counter = 0;

    // Physics timing preamble
    float exp_average_flip_time = 0.0f;
    float alpha_flip_time = 0.1;

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

        // Poll mouse position
        RGFW_window_getMouse(window, &xpos, &ypos);
        float mouse_position[] = {(float)xpos * mouse_scale, (float)ypos * mouse_scale};

        // Velocity pass
        shader_use(&velocityShader);
        shader_set_uniform_vec(&velocityShader, "mouse_position", 2, mouse_position);
        shader_set_uniform_int(&velocityShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&velocityShader, "velocity_buffer", velocityBuffer.current);
        shader_set_uniform_int(&velocityShader, "trail_buffer", trailBuffer.current);
        shader_set_uniform_int(&velocityShader, "epoch_counter", epoch_counter);
        buffer_bind(&velocityBuffer, other);
        buffer_update(&velocityBuffer);
        buffer_flip(&velocityBuffer);

        // Position pass
        shader_use(&positionShader);
        shader_set_uniform_int(&positionShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&positionShader, "velocity_buffer", velocityBuffer.current); // read from updated velocity buffer
        // shader_set_uniform_int(&positionShader, "epoch_counter", epoch_counter);
        buffer_bind(&positionBuffer, other);
        // buffer_update(&positionBuffer);
        buffer_update(&positionBuffer);
        buffer_flip(&positionBuffer);

        // Density buffer pass
        shader_use(&densityShader);
        shader_set_uniform_int(&densityShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&densityShader, "density_buffer_downsampling", densityBufferDownsampling);
        buffer_bind(&densityBuffer, current);
        buffer_update_n(&densityBuffer, P);

        // Trail buffer
        buffer_bind(&trailBuffer, other);

        shader_use(&copyShader); // First pass (alpha blend of the double buffer)
        shader_set_uniform_float(&copyShader, "alpha", trailAlpha);
        shader_set_uniform_int(&copyShader, "source_buffer", trailBuffer.current);
        buffer_update(&trailBuffer);

        shader_use(&trailShader); // Second pass
        shader_set_uniform_int(&trailShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&trailShader, "velocity_buffer", velocityBuffer.current);
        buffer_update_n(&trailBuffer, P);
        buffer_flip(&trailBuffer);

        // Screen rendering pass -> internal render target (render resolution)
        buffer_bind(&renderBuffer, current);

        shader_use(&screenShader);
        shader_set_uniform_int(&screenShader, "position_buffer", positionBuffer.current);
        shader_set_uniform_int(&screenShader, "velocity_buffer", velocityBuffer.current);
        shader_set_uniform_int(&screenShader, "epoch_counter", epoch_counter);
        buffer_update_n(&renderBuffer, P);

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

        // Upscale pass: nearest-sample the render target onto the window. Blending
        // is off -- the render target already holds particles composited over black,
        // so this is a straight opaque blit (chunky when render_scale > 1).
        buffer_bind(&screenBuffer, screen);
        glDisable(GL_BLEND);
        shader_use(&upscaleShader);
        shader_set_uniform_int(&upscaleShader, "source_buffer", renderBuffer.current);
        buffer_update(&screenBuffer);
        glEnable(GL_BLEND);

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
        }
        epoch_counter++;
    }

    // Clean up
    // TODO: clean up after oneself better
    glBindVertexArray(0);
    glDeleteBuffers(1, &vertex_buffer);
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
        flags |= RGFW_windowCenter;
    }

    window = RGFW_createWindow(title, win_x, win_y, width, height, flags);

    if (!window)
    {
        fprintf(stderr, "Failed to create RGFW window\n");
        exit(EXIT_FAILURE);
    }

    // Toggle fullscreen after creation (not via a create flag): RGFW targets the
    // monitor the window currently sits on. Move the window onto the target
    // monitor first -- cocoa's makeKeyWindow can pull a freshly-created window
    // onto the active screen, ignoring the spawn coordinates.
    if (fullscreen)
    {
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
    printf("handle_framebuffer_resize()\n");
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
    float buffer_shape[2] = {(float)new_width, (float)new_height};
    shader_set_uniform_vec(&screenShader, "buffer_size", 2, buffer_shape);
    shader_set_uniform_vec(&densityShader, "buffer_size", 2, buffer_shape);
    // shader_set_uniform_vec(&positionShader, "buffer_size", 2, buffer_shape);
    // shader_set_uniform_vec(&velocityShader, "buffer_size", 2, buffer_shape);
    // shader_set_uniform_vec(&copyShader, "buffer_size", 2, buffer_shape);
    shader_set_uniform_vec(&trailShader, "buffer_size", 2, buffer_shape);
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

    // Setup vertex array
    int *vertices = (int *)malloc(2 * P * sizeof(int));
    for (int i = 0; i < 2 * P; i++)
    {
        vertices[i] = 0;
    }

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, 2 * P * sizeof(int), vertices, GL_STATIC_DRAW);
    free(vertices);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // Calculate size of the physics buffer
    int PBwidth = ceil(sqrt(P));
    int PBheight = ceil(P / sqrt(P));
    fprintf(stdout, "%d points\n", P);
    fprintf(stdout, "Physics framebuffer shape: %d x %d\n", PBwidth, PBheight);
    updateShaderBufferShape(PBwidth, PBheight);

    // Initalise textures and the associated framebuffers
    glGenTextures(8, textures);
    glGenFramebuffers(8, framebuffers);

    // Texture 0 - Density buffer
    densityBuffer.minmag_filter = GL_NEAREST; // GL_LINEAR
    densityBuffer.wrap_st = GL_REPEAT;
    densityBuffer.dim = BE_1D;
    buffer_allocate(&densityBuffer, current, densityBufferIndex, density_width, density_height, NULL);

    positionBuffer.minmag_filter = GL_NEAREST;
    positionBuffer.dim = BE_2D;
    velocityBuffer.minmag_filter = GL_NEAREST;
    velocityBuffer.dim = BE_2D;

    // Texture 1 - Position buffer 1
    srand((unsigned int)time(NULL));
    float margin = (width < height ? width : height) * 0.1f;
    float noise_seed = 10 * get_time() + frand01();
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

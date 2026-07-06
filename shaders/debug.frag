#version 330 core
out vec4 color;

layout(pixel_center_integer) in vec4 gl_FragCoord;

// Unified debug overlay -- the old point/line cursor shader and the exclmask fill pass, merged.
// One fullscreen pass composited into the render buffer (SRC_ALPHA-over, so translucent fills let
// the scene show through; shows in --dump/--headless too since they read renderBuffer). Each
// overlay is behind its own draw_* uniform, so a future debug key can toggle any of them live; the
// pass runs whenever ANY draw_* is set (sim.c sim_step). All geometry is in render-pixel space
// (gl_FragCoord); sim.c converts the mouse/rect coords to match.
//  - draw_exclusions (--exclusions-debug): flat-fill each exclusion rect (translucent).
//  - draw_edges (--edge-debug): fill the bounding-edge repel bands, 0.05 of each wall (translucent;
//    mirrors repel.frag's edge_repell_radius = 0.05 * window_shape).
//  - draw_mouse (--mouse-debug): opaque cursor dot + per-frame motion line, over the translucent
//    repel-radius disc (radius = the speed-scaled value the physics uses). Replaces the dot/line
//    the front-ends used to draw to the window directly.
#define MAX_EXCLUSION_RECTS 16
uniform vec4 exclusion_rects[MAX_EXCLUSION_RECTS];
uniform int exclusion_rect_count;
uniform int draw_exclusions;
uniform int draw_edges;
uniform int draw_mouse;
uniform vec2 render_shape;   // render-buffer resolution (edge bands)
uniform vec2 buffer_shape;   // repel buffer resolution (for the downsample snap)
uniform vec2 mouse_center;   // render px, current cursor
uniform vec2 mouse_prev;     // render px, cursor last frame (motion line)
uniform float mouse_radius;  // render px, repel disc

const vec4 tint  = vec4(0.0, 1.0, 0.0, 0.35); // translucent fills
const vec4 solid = vec4(0.0, 1.0, 0.0, 1.0);  // opaque markers (dot / line)

// distance from point p to segment a-b
float seg_dist(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    return length(p - (a + t * ab));
}

void main() {
    vec2 raw = gl_FragCoord.xy; // crisp, for the cursor dot/line (UI markers, not the field)

    // The field overlays (exclusion / edge / mouse disc) represent the DOWNSAMPLED repel buffer the
    // physics actually samples -- so snap the fragment to that buffer's texel grid. Every fragment
    // in one repel-buffer texel then evaluates identically -> the overlay is blocky at the true
    // coarse resolution, not the ideal infinite-res shape.
    vec2 cell = render_shape / buffer_shape;          // render px per repel-buffer texel
    vec2 p = (floor(raw / cell) + 0.5) * cell;        // snapped to the texel centre

    if (draw_exclusions != 0) {
        for (int i = 0; i < exclusion_rect_count && i < MAX_EXCLUSION_RECTS; i++) {
            vec4 r = exclusion_rects[i];
            if (p.x >= r.x && p.x <= r.z && p.y >= r.y && p.y <= r.w) { color = tint; return; }
        }
    }

    if (draw_edges != 0) {
        // uniform band (5% of the shorter side) -- mirrors repel.frag's edge_repell_radius
        float band = 0.05 * min(render_shape.x, render_shape.y);
        if (p.x < band || p.x > render_shape.x - band ||
            p.y < band || p.y > render_shape.y - band) { color = tint; return; }
    }

    if (draw_mouse != 0) {
        // opaque cursor dot + motion line stay CRISP (raw) -- they mark the cursor, not the field.
        // The repel disc is part of the field, so it uses the snapped (downsampled) p.
        if (length(raw - mouse_center) < 3.0) { color = solid; return; }
        if (seg_dist(raw, mouse_prev, mouse_center) < 1.0) { color = solid; return; }
        if (mouse_radius > 0.0 && length(p - mouse_center) < mouse_radius) { color = tint; return; }
    }

    discard;
}

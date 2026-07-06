#version 330 core

// Fullscreen quad (triangle strip, 4 verts). Maps the internal render target 1:1
// onto the output framebuffer; the nearest filter on the source texture gives the
// chunky upscale at low internal resolutions.
const vec2[] corners = vec2[](
    vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0)
);

// Source-texture crop. Identity (0,0)..(1,1) samples the whole render target -- the
// straight upscale. macwp -m=-2 narrows it to a centred sub-rect so each monitor
// presents its own 1:1 slice of the shared field (crop, not stretch).
uniform vec2 uv_min;
uniform vec2 uv_max;

out vec2 uv;

void main() {
    vec2 c = corners[gl_VertexID];
    gl_Position = vec4(c, 0.0, 1.0);
    // quad still fills the window; uv walks only the crop sub-rect of the source.
    uv = mix(uv_min, uv_max, c * 0.5 + 0.5);
}

#version 330 core

// Fullscreen quad (triangle strip, 4 verts). Maps the internal render target 1:1
// onto the output framebuffer; the nearest filter on the source texture gives the
// chunky upscale at low internal resolutions.
const vec2[] corners = vec2[](
    vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0)
);

out vec2 uv;

void main() {
    vec2 c = corners[gl_VertexID];
    gl_Position = vec4(c, 0.0, 1.0);
    uv = c * 0.5 + 0.5; // identity: same GL framebuffer/texture convention on write and read
}

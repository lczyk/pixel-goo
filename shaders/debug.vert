#version 330 core

// Fullscreen quad (triangle strip, 4 verts) for the unified debug overlay. Covers the render
// target 1:1 so the fragment shader can fill/mark in render-pixel space (gl_FragCoord).
// gl_VertexID only -- no vertex attributes, so whatever VAO is bound is fine.
const vec2[] corners = vec2[](
    vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0)
);

void main() {
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}

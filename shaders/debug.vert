#version 330 core

// Draws primitives at positions supplied via a uniform vec2 array.
// Positions are in window pixels (origin top-left, y down).
// gl_VertexID indexes into the array.

uniform vec2 positions[2];
uniform vec2 window_shape; // logical window size in pixels

void main() {
    vec2 p = positions[gl_VertexID];
    // convert pixel coords (top-left origin, y-down) -> NDC (centre origin, y-up)
    vec2 ndc = vec2(
         2.0 * p.x / window_shape.x - 1.0,
        -2.0 * p.y / window_shape.y + 1.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    gl_PointSize = 8.0;
}

#version 330 core

uniform vec2 window_shape;
uniform sampler2D density_buffer;
uniform float cull_from; // density where culling begins (--cull-from)
uniform float cull_max;  // ceiling on cull fraction, < 1 keeps cores populated (--cull-max)

// Position/velocity streamed from PBOs as attributes (no vertex texture fetch).
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_velocity;

out float velocity;

float random(float seed) { return fract(sin(seed * 0.890) * 43758.5453123); }

vec2 screenNormalisedCoords(vec2 coordinate) {
    coordinate = mod(coordinate, window_shape);
    return vec2(
        +coordinate.x/(window_shape.x/2)-1,
        -(coordinate.y/(window_shape.y/2)-1)
        );
}

void main() {
    velocity = length(a_velocity);

    // Stochastic density cull, pre-binning (pushed offscreen -> clipped before the
    // tiler bins it, shrinking the dense/expensive tiles). Cull probability ramps from 0
    // at cull_from density to cull_max at full density; compared against a fixed-per-
    // particle random so the decision is stable (no per-frame flicker). Nothing below
    // cull_from is culled (no gaps in sparse/mid regions); cull_max < 1 keeps the densest
    // cores populated (no holes).
    float density = texture(density_buffer, mod(a_position, window_shape)/window_shape).x;
    float cull_prob = cull_max * smoothstep(cull_from, 1.0, density);
    if (cull_prob > random(3.0 + float(gl_VertexID))) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside clip space -> never binned
        return;
    }

    gl_Position = vec4(screenNormalisedCoords(a_position), 0.0f, 1.0f);
    gl_PointSize = 1.0f;
}

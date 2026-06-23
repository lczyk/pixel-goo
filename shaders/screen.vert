#version 330 core

uniform vec2 window_shape;
uniform float cull_amount; // fraction of particles culled, uniformly (--cull)

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

    // Uniform stable cull: keep iff the particle's fixed random > cull_amount. It's
    // density-independent, so the culled set is identical every frame -> cannot flicker
    // (unlike the old density-driven cull, which popped as the live density jittered).
    // culled points go offscreen -> clipped before the tiler ever bins them.
    if (random(3.0 + float(gl_VertexID)) < cull_amount) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    gl_Position = vec4(screenNormalisedCoords(a_position), 0.0f, 1.0f);
    gl_PointSize = 1.0f;
}

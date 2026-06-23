#version 330 core

uniform vec2 window_shape;
uniform sampler2D density_buffer;
uniform float cull_amount;    // max density-weighted cull probability (--cull)
uniform float render_headroom; // density normalization shared with the screen colormap

// Position/velocity streamed from PBOs as attributes; density is sampled for culling.
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

vec2 textureNormalisedCoords(vec2 coordinate) {
    coordinate = mod(coordinate, window_shape);
    return vec2(coordinate.x/window_shape.x, 1.0 - coordinate.y/window_shape.y);
}

void main() {
    velocity = length(a_velocity);

    float density = texture(density_buffer, textureNormalisedCoords(a_position)).x;
    float norm_density = clamp(density / max(render_headroom, 0.0001), 0.0, 1.0);
    float density_cull = cull_amount * min(sqrt(norm_density), 0.65);

    // Stable density cull: each particle has a fixed threshold, while local density
    // controls the cull probability. The concave curve gives sparse edges meaningful
    // thinning, while the cap keeps dense cores from dropping every particle.
    // Culled points are clipped before rasterization.
    if (random(3.0 + float(gl_VertexID)) < density_cull) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    gl_Position = vec4(screenNormalisedCoords(a_position), 0.0f, 1.0f);
    gl_PointSize = 1.0f;
}

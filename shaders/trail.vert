#version 330 core

uniform vec2 window_shape;
uniform int trail_buffer_downsampling;
uniform float kernel_radius;

// Position/velocity streamed from PBOs as attributes (no vertex texture fetch).
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_velocity;

out vec2 velocity;

vec2 screenNormalisedCoordinates(vec2 coordinate) {
    coordinate = mod(coordinate, window_shape);
    return vec2(+coordinate.x/(window_shape.x/2)-1, -(coordinate.y/(window_shape.y/2)-1));
}

void main() {
    velocity = a_velocity;

    gl_Position = vec4(screenNormalisedCoordinates(a_position), 0.0f, 1.0f);
    gl_PointSize = kernel_radius/trail_buffer_downsampling;
}

#version 330 core

uniform vec2 window_shape;
uniform int density_buffer_downsampling;
uniform float kernel_radius;

// Position streamed from a PBO as an attribute (no vertex texture fetch).
layout(location = 0) in vec2 a_position;

void main() {
    vec2 position = a_position;
    vec2 normalised_coordinates = vec2(
        position.x/(window_shape.x/2)-1,
        -(position.y/(window_shape.y/2)-1)
        );

    gl_Position = vec4(normalised_coordinates.x, normalised_coordinates.y, 0.0f, 1.0f);
    gl_PointSize = kernel_radius/density_buffer_downsampling;
}

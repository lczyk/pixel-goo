#version 330 core

uniform vec2 window_shape;
uniform vec2 buffer_size;
uniform sampler2D position_buffer;
uniform int density_buffer_downsampling;
uniform float kernel_radius;

void main() {
    ivec2 buffer_position = ivec2(gl_VertexID % int(buffer_size.x), gl_VertexID / int(buffer_size.y));
    vec2 position = vec2(texelFetch(position_buffer, buffer_position, 0));
    vec2 normalised_coordinates = vec2(
        position.x/(window_shape.x/2)-1,
        -(position.y/(window_shape.y/2)-1)
        );

    gl_Position = vec4(normalised_coordinates.x, normalised_coordinates.y, 0.0f, 1.0f);
    gl_PointSize = kernel_radius/density_buffer_downsampling;
}
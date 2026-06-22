#version 330 core

// uniform sampler2D trail_buffer;

uniform vec2 window_shape;
uniform vec2 buffer_size;
uniform int trail_buffer_downsampling;
uniform float kernel_radius;

uniform sampler2D position_buffer;
uniform sampler2D velocity_buffer;

out vec2 velocity;

vec2 screenNormalisedCoordinates(vec2 coordinate) {
    coordinate = mod(coordinate, window_shape);
    return vec2(+coordinate.x/(window_shape.x/2)-1, -(coordinate.y/(window_shape.y/2)-1));
}

void main() {
    ivec2 buffer_position = ivec2(gl_VertexID % int(buffer_size.x), gl_VertexID / int(buffer_size.y));
    vec2 position = vec2(texelFetch(position_buffer, buffer_position, 0));
    velocity = vec2(texelFetch(velocity_buffer, buffer_position, 0));

    gl_Position = vec4(screenNormalisedCoordinates(position), 0.0f, 1.0f);
    gl_PointSize = kernel_radius/trail_buffer_downsampling;
}
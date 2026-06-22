#version 330 core

uniform vec2 window_shape;
uniform vec2 buffer_size;
uniform sampler2D position_buffer;
uniform sampler2D velocity_buffer;

out float velocity;
out float VertexID;

vec2 screenNormalisedCoords(vec2 coordinate) {
    coordinate = mod(coordinate, window_shape);
    return vec2(
        +coordinate.x/(window_shape.x/2)-1,
        -(coordinate.y/(window_shape.y/2)-1)
        );
}

void main() {
    ivec2 buffer_position = ivec2(gl_VertexID % int(buffer_size.x), gl_VertexID / int(buffer_size.y));
    vec2 position = vec2(texelFetch(position_buffer, buffer_position, 0));
    velocity = length(vec2(texelFetch(velocity_buffer, buffer_position, 0)));

    gl_Position = vec4(screenNormalisedCoords(position), 0.0f, 1.0f);
    gl_PointSize = 1.0f;
    VertexID = float(gl_VertexID);
}
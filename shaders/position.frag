#version 330 core

// Access fragmet coordinates in integer steps
// TODO: explain more
layout(pixel_center_integer) in vec4 gl_FragCoord;
out vec4 out_position;

in float VertexID;

uniform vec2 window_shape;
uniform sampler2D position_buffer;
uniform sampler2D velocity_buffer;

// uniform int epoch_counter;

// float random_float (float seed) { // random_vec2 from -1 to +1
//     return fract(sin(seed * 0.890)*43758.5453123);
// }

void main() {
    ivec2 buffer_position = ivec2(gl_FragCoord.xy);
    vec2 position = vec2(texelFetch(position_buffer, buffer_position, 0)); // previous position
    vec2 velocity = vec2(texelFetch(velocity_buffer, buffer_position, 0)); // velocity
    // vec2 velocity = vec2(0, 1);
    vec2 delta_position = velocity;
    // delta_position += random(vec2(0,0) + VertexID + epoch_counter);
    // delta_position += +vec2(1.0, 1.0); // drift

    vec2 new_position = position + delta_position;
    new_position = mod(new_position, window_shape);
    
    if ( isinf(new_position.x) || isnan(new_position.x ) ) { new_position.x = window_shape.x/2; }
    if ( isinf(new_position.y) || isnan(new_position.y ) ) { new_position.y = window_shape.y/2; }

    out_position = vec4(new_position, 0.0, 1.0);
}
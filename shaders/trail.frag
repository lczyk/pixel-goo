#version 330 core
uniform float trail_intensity;
uniform float velocity_floor;
in vec2 velocity;

out vec4 color;

#define ISQRT2 0.7071067811865476 // COS PI/4
#define COSPID8 0.9238795325112867 // COS PI/8
#define COSPID16 0.9807852804032304 // COS PI/16
#define PI 3.141592653589793

void main() {
    // Discard each point outside of the circle radius
    // https://stackoverflow.com/a/27099691/2531987
    vec2 circCoord = 2.0 * gl_PointCoord.xy - 1.0;
    if (dot(circCoord, circCoord) > 1.0) { discard; }

    float velocity_magnitude = length(velocity);

    vec2 velocity_normal = velocity / velocity_magnitude;
    // if (velocity_magnitude == 0) {discard;}
    if (velocity_magnitude < velocity_floor) {discard;}

    float r = circCoord.x*circCoord.x + circCoord.y*circCoord.y;
    float theta = dot(circCoord.xy,velocity_normal)/length(circCoord);
    if (theta > -COSPID16) {discard;}

    color = vec4(1.0f, 0.0f, 0.0f, trail_intensity);    
}
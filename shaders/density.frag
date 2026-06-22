#version 330 core
out vec4 color;
uniform float density_alpha;

void main() {
    // Discard each point outside of the circle radius
    // https://stackoverflow.com/a/27099691/2531987
    vec2 circCoord = 2.0 * gl_PointCoord - 1.0;
    if (dot(circCoord, circCoord) > 1.0) { discard; }

    color = vec4(1.0f, 0.0f, 0.0f, density_alpha);
}
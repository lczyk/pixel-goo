#version 330 core

in vec2 uv;
out vec4 color;

uniform sampler2D source_buffer;

void main() {
    // Opaque copy: the internal target already has the particles composited over
    // black, so blit its rgb straight out (blending is disabled for this pass).
    color = vec4(texture(source_buffer, uv).rgb, 1.0);
}

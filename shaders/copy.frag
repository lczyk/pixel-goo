#version 330 core
out vec4 color;

uniform sampler2D source_buffer;
uniform float alpha;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(source_buffer, 0));
    vec4 source_value = texture(source_buffer, uv);
    color = vec4(source_value.xyz, source_value.a * alpha);
}
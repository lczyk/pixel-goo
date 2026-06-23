#version 330 core
out vec4 color;

uniform sampler2D source_buffer;
uniform float alpha;

void main() {    
    vec4 source_value = texture(source_buffer, (gl_PointCoord.xy+1)*0.5);
    color = vec4(source_value.xyz, source_value.a * alpha);
}
#version 330 core

out float VertexID;

const vec2[] corners = vec2[](
    vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0)
);

void main() {
    gl_Position = vec4(corners[gl_VertexID], 1.0, 1.0);
    VertexID = float(gl_VertexID);
}
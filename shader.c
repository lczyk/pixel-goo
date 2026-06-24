#include "shader.h"

#include <stdio.h>
#include <stdlib.h>

#include <gl.h>

void shader_create(Shader *s) {
    s->program = glCreateProgram();
}

void shader_use(Shader *s) {
    glUseProgram(s->program);
}

void shader_compile(Shader *s, const GLuint shaderTypeEnum, const GLchar *shaderSource) {
    GLuint shader = glCreateShader(shaderTypeEnum);
    glShaderSource(shader, 1, &shaderSource, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[INFOLOG_LEN];
        glGetShaderInfoLog(shader, INFOLOG_LEN, NULL, infoLog);
        fprintf(stderr, "[%s] Compilation error\n%s\n", s->name, infoLog);
        exit(EXIT_FAILURE);
    }

    // Attach shader to the program
    glAttachShader(s->program, shader);
    glDeleteShader(shader);
}

void shader_link(Shader *s) {
    glLinkProgram(s->program);

    GLint success;
    glGetProgramiv(s->program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[INFOLOG_LEN];
        glGetProgramInfoLog(s->program, INFOLOG_LEN, NULL, infoLog);
        fprintf(stderr, "[%s] Link error\n%s\n", s->name, infoLog);
        exit(EXIT_FAILURE);
    }
}

void shader_set_uniform_int(Shader *s, const GLchar *uniform_name, const int value) {
    shader_use(s);
    glUniform1i(shader_get_uniform_location(s, uniform_name), value);
}

void shader_set_uniform_float(Shader *s, const GLchar *uniform_name, const float value) {
    shader_use(s);
    glUniform1f(shader_get_uniform_location(s, uniform_name), value);
}

void shader_set_uniform_vec(Shader *s, const GLchar *uniform_name, const int dim, const float value[]) {
    shader_use(s);
    switch (dim) {
    case 2: {
        glUniform2fv(shader_get_uniform_location(s, uniform_name), 1, value);
    } break;
    case 3: {
        glUniform3fv(shader_get_uniform_location(s, uniform_name), 1, value);
    } break;
    case 4: {
        glUniform4fv(shader_get_uniform_location(s, uniform_name), 1, value);
    } break;
    default:
        fprintf(stderr, "[%s] Cannot set only set vec{2|3|4} uniforms", s->name);
        exit(EXIT_FAILURE);
    }
}

GLint shader_get_uniform_location(Shader *s, const GLchar *uniform_name) {
    GLint location = glGetUniformLocation(s->program, uniform_name);
    if (location < 0) {
        fprintf(stderr, "[%s] Uniform \"%s\" not found in the program (location = %d)", s->name, uniform_name, location);
        exit(EXIT_FAILURE);
    }
    return location;
}

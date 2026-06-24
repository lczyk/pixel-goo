#ifndef SHADER_H
#define SHADER_H

#include <gl.h>

#define INFOLOG_LEN 512

typedef struct Shader {
    const char *name;
    GLint program;
} Shader;

void shader_create(Shader *s);
void shader_use(Shader *s);
void shader_compile(Shader *s, const GLuint shaderTypeEnum, const GLchar *shaderSource);
void shader_link(Shader *s);

void shader_set_uniform_int(Shader *s, const GLchar *uniform_name, const int value);
void shader_set_uniform_float(Shader *s, const GLchar *uniform_name, const float value);
void shader_set_uniform_vec(Shader *s, const GLchar *uniform_name, const int dim, const float value[]);

GLint shader_get_uniform_location(Shader *s, const GLchar *uniform_name);

#endif /* SHADER_H */

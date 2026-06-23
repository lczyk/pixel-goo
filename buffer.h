#ifndef buffer_H
#define buffer_H

#include <gl.h>

typedef int PBindex;

typedef enum Bwhich {
    current = 0,
    other = 1,
    screen = 2
} Bwhich;

typedef struct format {
    GLint format;
    GLint iformat;
} format;

#define BE_1D (format){ .format = GL_RED, .iformat = GL_R32F }
#define BE_2D (format){ .format = GL_RG, .iformat = GL_RG32F }
#define BE_3D (format){ .format = GL_RGB, .iformat = GL_RGB32F }
#define BE_4D (format){ .format = GL_RGBA, .iformat = GL_RGBA32F }

typedef struct Buffer {
    const char* name;
    PBindex current;
    PBindex other;
    int width;
    int height;
    GLuint minmag_filter;
    GLuint wrap_st;
    format dim;
    const GLuint* textures;
    const GLuint* framebuffers;
} Buffer;

void buffer_allocate(Buffer* b, const Bwhich which, const PBindex index, const int width, const int height, const char* data);
void buffer_reallocate(Buffer* b, const Bwhich which, const int width, const int height);
void buffer_flip(Buffer* b);
void buffer_bind(Buffer* b, const Bwhich which);
void buffer_update(Buffer* b);             // draw a fullscreen quad (4-vertex triangle strip)
void buffer_update_n(Buffer* b, const int P); // draw P points

#endif /* buffer_H */

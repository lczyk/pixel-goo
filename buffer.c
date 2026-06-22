#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>

#include <gl.h>

static void buffer_set_index(Buffer* b, const Bwhich which, const PBindex index);
static void buffer_check_shape(Buffer* b, const int width, const int height);

void buffer_allocate(Buffer* b, const Bwhich which, const PBindex index, const int width, const int height, const char* data) {
    buffer_check_shape(b, width, height);
    if (which == screen) {
        fprintf(stderr, "[%s] cannot allocate the screen buffer", b->name);
        exit(EXIT_FAILURE);
    }

    glActiveTexture(GL_TEXTURE0 + index);
    glBindTexture(GL_TEXTURE_2D, b->textures[index]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, b->minmag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, b->minmag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, b->wrap_st);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, b->wrap_st);

    // Write data to texture
    glTexImage2D(GL_TEXTURE_2D, 0, b->dim.iformat, width, height, 0, b->dim.format, GL_FLOAT, data);

    // Bind framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, b->framebuffers[index]);

    // Clear buffer to 0
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Attach the framebuffer to the texture
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->textures[index], 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[%s] framebuffer is not complete", b->name);
        exit(EXIT_FAILURE);
    }

    buffer_set_index(b, which, index);
    b->width = width;
    b->height = height;
}

void buffer_reallocate(Buffer* b, const Bwhich which, const int width, const int height) {
    buffer_allocate(b, which, b->current, width, height, NULL);
}

void buffer_flip(Buffer* b) {
    PBindex temp = b->current;
    b->current = b->other;
    b->other = temp;
}

void buffer_bind(Buffer* b, const Bwhich which) {
    GLint framebuffer = 0;
    switch (which) {
        case current: { framebuffer = b->framebuffers[b->current]; } break;
        case other: { framebuffer = b->framebuffers[b->other]; } break;
        case screen: { framebuffer = 0; } break;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, b->width, b->height); // Change the viewport to the size of the 1D texture vector
    glClear(GL_COLOR_BUFFER_BIT); // Dont need to clear it as its writing to each pixel anyway
}

void buffer_update(Buffer* b) {
    (void)b;
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // Need to only write 4 points since vertex shader makes them into a quad over the entire screen anyway
}

void buffer_update_n(Buffer* b, const int P) {
    (void)b;
    glDrawArrays(GL_POINTS, 0, P);
}

static void buffer_set_index(Buffer* b, const Bwhich which, const PBindex index) {
    switch (which) {
        case current: { b->current = index; } break;
        case other: { b->other = index; } break;
        case screen: {
            fprintf(stderr, "[%s] can't set object index to screen buffer", b->name);
            exit(EXIT_FAILURE);
        } break;
    }
}

static void buffer_check_shape(Buffer* b, const int width, const int height) {
    GLint max_renderbuffer_size;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer_size);
    if (width > max_renderbuffer_size) {
        fprintf(stderr, "[%s] Physics framebuffer width (%d) larger than renderbuffer size (%d)", b->name, width, max_renderbuffer_size);
        exit(EXIT_FAILURE);
    }
    if (height > max_renderbuffer_size) {
        fprintf(stderr, "[%s] Physics framebuffer width (%d) larger than renderbuffer size (%d)", b->name, height, max_renderbuffer_size);
        exit(EXIT_FAILURE);
    }
}

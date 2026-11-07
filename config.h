#ifndef CTERMFLUID_CONFIG_H
#define CTERMFLUID_CONFIG_H

#include <stddef.h>

#define MAX_SOURCES 32

typedef struct {
    float r, g, b;
} vec3;

typedef struct {
    float x, y;                 /* Normalized position within the interior. */
    int radius;
    vec3 color;
    int palette;                /* Optional terminal palette index, or -1. */
    float vx, vy, rate;
} Source;

typedef struct {
    float dissipation, viscosity;
    int max_fps;
    vec3 mouse_color;
    int mouse_palette;          /* Optional terminal palette index, or -1. */
    Source sources[MAX_SOURCES];
    size_t count;
} Program;

int load_program(const char *path, Program *program, char *error, size_t error_size);

#endif

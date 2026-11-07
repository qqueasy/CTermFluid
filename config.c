#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static int number(const char *text, float lo, float hi, float *out) {
    char *end;
    errno = 0;
    float value = strtof(text, &end);
    if (errno || end == text || *end || !isfinite(value) || value < lo || value > hi)
        return 0;
    *out = value;
    return 1;
}

static int integer(const char *text, int lo, int hi, int *out) {
    float value;
    if (!number(text, (float)lo, (float)hi, &value) || floorf(value) != value) return 0;
    *out = (int)value;
    return 1;
}

static int color(const char *text, vec3 *out) {
    if (strlen(text) != 7 || text[0] != '#') return 0;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)text[i])) return 0;
    unsigned long rgb = strtoul(text + 1, NULL, 16);
    *out = (vec3){((rgb >> 16) & 255) / 255.0f,
                  ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
    return 1;
}

int load_program(const char *path, Program *program, char *error, size_t error_size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, error_size, "%s: %s", path, strerror(errno));
        return 0;
    }
    Program parsed = {.dissipation = 0.05f, .viscosity = 0.0001f,
                      .mouse_color = {0, 1, 1}, .mouse_palette = -1, .max_fps = 40};
    enum {NONE, SIMULATION, SOURCE} section = NONE;
    unsigned seen = 0, simulation_seen = 0;
    size_t line_number = 0;
    const char *reason = NULL;
    char line[512];
    while (fgets(line, sizeof line, file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file)) { reason = "line too long"; break; }
        char *comment = strchr(line, ';');
        if (comment) *comment = '\0';
        char *text = trim(line);
        if (!*text || *text == '#') continue;
        if (*text == '[') {
            if (section == SOURCE && (seen & 7) != 7) {
                reason = "each source requires x, y, and color"; break;
            }
            seen = 0;
            if (strcmp(text, "[simulation]") == 0) section = SIMULATION;
            else if (strcmp(text, "[source]") == 0) {
                if (parsed.count == MAX_SOURCES) { reason = "too many sources (maximum 32)"; break; }
                section = SOURCE;
                parsed.sources[parsed.count++] = (Source){.radius = 3, .palette = -1, .rate = 3};
            } else { reason = "unknown section"; break; }
            continue;
        }
        char *equals = strchr(text, '=');
        if (!equals || section == NONE) { reason = "expected a section and key=value"; break; }
        *equals = '\0';
        char *key = trim(text), *value = trim(equals + 1);
        unsigned bit = 0;
        int valid = 0;
        if (section == SIMULATION) {
            if (!strcmp(key, "dissipation")) {
                bit = 1; valid = number(value, 0, 10, &parsed.dissipation);
            } else if (!strcmp(key, "viscosity")) {
                bit = 2; valid = number(value, 0, 1, &parsed.viscosity);
            } else if (!strcmp(key, "mouse_color")) {
                bit = 4; valid = color(value, &parsed.mouse_color);
            } else if (!strcmp(key, "mouse_palette")) {
                bit = 8; valid = integer(value, 0, 15, &parsed.mouse_palette);
            } else if (!strcmp(key, "max_fps")) {
                bit = 16; valid = integer(value, 1, 240, &parsed.max_fps);
            }
            if (simulation_seen & bit) { reason = "duplicate simulation key"; break; }
            simulation_seen |= bit;
        } else {
            Source *source = &parsed.sources[parsed.count - 1];
            if (!strcmp(key, "x")) { bit = 1; valid = number(value, 0, 1, &source->x); }
            else if (!strcmp(key, "y")) { bit = 2; valid = number(value, 0, 1, &source->y); }
            else if (!strcmp(key, "color")) { bit = 4; valid = color(value, &source->color); }
            else if (!strcmp(key, "radius")) { bit = 8; valid = integer(value, 0, 20, &source->radius); }
            else if (!strcmp(key, "vx")) { bit = 16; valid = number(value, -1000, 1000, &source->vx); }
            else if (!strcmp(key, "vy")) { bit = 32; valid = number(value, -1000, 1000, &source->vy); }
            else if (!strcmp(key, "rate")) { bit = 64; valid = number(value, 0, 100, &source->rate); }
            else if (!strcmp(key, "palette")) { bit = 128; valid = integer(value, 0, 15, &source->palette); }
            if (seen & bit) { reason = "duplicate source key"; break; }
            seen |= bit;
        }
        if (!bit) { reason = "unknown key"; break; }
        if (!valid) { reason = "invalid or out-of-range value"; break; }
    }
    if (!reason && ferror(file)) reason = "error reading file";
    if (!reason && section == SOURCE && (seen & 7) != 7)
        reason = "each source requires x, y, and color";
    if (!reason && section == NONE) reason = "empty program";
    fclose(file);
    if (reason) {
        snprintf(error, error_size, "%s:%zu: %s", path, line_number, reason);
        return 0;
    }
    *program = parsed;
    return 1;
}

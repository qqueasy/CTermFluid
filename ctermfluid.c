#define _POSIX_C_SOURCE 200809L
#include <ncurses.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "config.h"
#include "build_config.h"
#include "program_defaults.h"

#define MAX_WIDTH 1026
#define MAX_HEIGHT 514
int width;
int height;
int displayWidth;
int displayHeight;

typedef struct {
    float x;
    float y;
} vec2;

typedef struct {
    int fps;
    int max_fps;
    int col;  
    int mouse;
    int no_dissipation;
    int check_config;
    const char *program;
} Args;

Args args;
static int color_ready;
static volatile sig_atomic_t stop_requested;
static Program active_program;

static void usage(FILE *stream, const char *program) {
    fprintf(stream,
            "Usage: %s [OPTIONS]\n"
            "CTermFluid: ASCII fluid simulation for the terminal.\n"
            "\n"
            "Options:\n"
            "  -c, --no-color         Disable color (enabled by default)\n"
            "      --no-colour        Alias for --no-color\n"
            "      --color, --colour  Enable color explicitly\n"
            "  -f, --fps              Show frame rate and simulation statistics\n"
            "  -F, --max-fps N        Cap rendering at 1-240 FPS (default: 40)\n"
            "  -m, --mouse            Start empty; drag the left mouse button to add smoke\n"
            "  -p, --program NAME|FILE Load a program (default: default)\n"
            "  -n, --no-dissipation   Disable smoke dissipation\n"
            "  -t, --check-config     Validate the selected program and exit\n"
            "  -h, --help             Show this help and exit\n"
            "  -V, --version          Show version information and exit\n"
            "\n"
            "Press q, Escape, or Ctrl+C to quit. Resizing resets the simulation.\n",
            program);
}

static int parse_max_fps(const char *text, int *out) {
    if (!isdigit((unsigned char)*text)) return 0;
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || *end || value < 1 || value > 240) return 0;
    *out = (int)value;
    return 1;
}

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void sleep_seconds(double seconds) {
    struct timespec delay = {(time_t)seconds,
        (long)((seconds - (time_t)seconds) * 1000000000.0)};
    while (!stop_requested && nanosleep(&delay, &delay) == -1 && errno == EINTR) {}
}

static int named_program(const char *directory, const char *name,
                         char *path, size_t path_size) {
    int length = snprintf(path, path_size, "%s/%s.conf", directory, name);
    if (length < 0 || (size_t)length >= path_size) return 0;
    /* Only a missing file permits fallback to the next directory. */
    if (access(path, F_OK) == 0) return 1;
    return errno != ENOENT && errno != ENOTDIR;
}

/* First-use seeds come from the .conf files at build time, not C demos.
   Create them as the running user, never during a privileged installation. */
static int seed_program(const char *directory, const char *name,
                        char *path, size_t path_size) {
    const char *contents = NULL;
    for (size_t i = 0; i < sizeof bundled_programs / sizeof bundled_programs[0]; i++)
        if (!strcmp(name, bundled_programs[i].name)) contents = bundled_programs[i].contents;
    if (!contents) return 0;
    int length = snprintf(path, path_size, "%s/%s.conf", directory, name);
    if (length < 0 || (size_t)length >= path_size) return 0;
    char parents[4096];
    length = snprintf(parents, sizeof parents, "%s", directory);
    if (length < 0 || (size_t)length >= sizeof parents) return 0;
    for (char *p = parents + 1; ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            int result = mkdir(parents, 0700);
            *p = saved;
            if (result == -1 && errno != EEXIST) return 1;
            if (!saved) break;
        }
    }
    char temporary[4096];
    length = snprintf(temporary, sizeof temporary, "%s/.seed-XXXXXX", directory);
    if (length < 0 || (size_t)length >= sizeof temporary) return 0;
    int fd = mkstemp(temporary);
    if (fd == -1) return 1;
    size_t remaining = strlen(contents);
    while (remaining) {
        ssize_t written = write(fd, contents, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(fd);
            unlink(temporary);
            return 1;
        }
        contents += written;
        remaining -= (size_t)written;
    }
    int complete = close(fd) == 0;
    /* Publish atomically, without replacing an existing file or symlink. */
    if (complete) link(temporary, path);
    unlink(temporary);
    return 1;
}

static int find_program(const char *name, char *path, size_t path_size) {
    if (strchr(name, '/') || (strlen(name) >= 5 && !strcmp(name + strlen(name) - 5, ".conf"))) {
        int length = snprintf(path, path_size, "%s", name);
        return length >= 0 && (size_t)length < path_size;
    }
    if (!*name) return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return 0;
    char directory[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int length = -1;
    if (xdg && *xdg == '/')
        length = snprintf(directory, sizeof directory, "%s/ctermfluid", xdg);
    else if (home && *home == '/')
        length = snprintf(directory, sizeof directory, "%s/.config/ctermfluid", home);
    int have_directory = length >= 0 && (size_t)length < sizeof directory;
    if (have_directory && named_program(directory, name, path, path_size)) return 1;
    if (named_program("./programs", name, path, path_size)) return 1;
    return have_directory && seed_program(directory, name, path, path_size);
}

static int update_dimensions(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < 2 + args.fps || cols < 3)
        return 0;
    displayWidth = cols;
    displayHeight = rows - args.fps;
    if (displayWidth > MAX_WIDTH - 2) displayWidth = MAX_WIDTH - 2;
    if (displayHeight > MAX_HEIGHT - 2) displayHeight = MAX_HEIGHT - 2;
    width = displayWidth + 2;
    height = displayHeight + 2;
    return 1;
}

static double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

vec2 get_vec(vec2 f[MAX_HEIGHT][MAX_WIDTH], int i, int j) {
    if (i < 0) i = 0;
    if (i >= height) i = height - 1;
    if (j < 0) j = 0;
    if (j >= width) j = width - 1;
    return f[i][j];
}

float get_scalar(float f[MAX_HEIGHT][MAX_WIDTH], int i, int j) {
    if (i < 0) i = 0;
    if (i >= height) i = height - 1;
    if (j < 0) j = 0;
    if (j >= width) j = width - 1;
    return f[i][j];
}

void init_vec_field(vec2 field[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            field[i][j] = (vec2){.x = 0, .y = 0};
        }
    }
}

void init_vec3_field(vec3 field[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            field[i][j] = (vec3){0, 0, 0};
        }
    }
}

void init_scalar_field(float field[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            field[i][j] = 0;
        }
    }
}

/* Copy only active cells, keeping small terminals inexpensive at a large cap. */
static void copy_vec_field(vec2 dst[MAX_HEIGHT][MAX_WIDTH],
                           vec2 src[MAX_HEIGHT][MAX_WIDTH]) {
    for (int y = 0; y < height; y++)
        memcpy(dst[y], src[y], (size_t)width * sizeof dst[y][0]);
}

static void copy_dye_field(vec3 dst[MAX_HEIGHT][MAX_WIDTH],
                           vec3 src[MAX_HEIGHT][MAX_WIDTH]) {
    for (int y = 0; y < height; y++)
        memcpy(dst[y], src[y], (size_t)width * sizeof dst[y][0]);
}

void apply_velocity_boundaries(vec2 v[MAX_HEIGHT][MAX_WIDTH]) {
    for (int x = 0; x < width; x++) {
        v[0][x] = (vec2){0, 0};
        v[height - 1][x] = (vec2){0, 0};
    }

    for (int y = 0; y < height; y++) {
        v[y][0] = (vec2){0, 0};
        v[y][width - 1] = (vec2){0, 0};
    }
}

void calculate_divergence(vec2 velocityField[MAX_HEIGHT][MAX_WIDTH], float divergence[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            vec2 above, below, left, right;
            above = get_vec(velocityField, i - 1, j);
            below = get_vec(velocityField, i + 1, j);
            left = get_vec(velocityField, i, j - 1);
            right = get_vec(velocityField, i, j + 1);
            divergence[i][j] = 0.5f*((right.x - left.x) + (below.y - above.y));
        }
    }
}

void solve(float pressureField[MAX_HEIGHT][MAX_WIDTH], float divergence[MAX_HEIGHT][MAX_WIDTH],
           float pressureFieldNew[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            float above, below, left, right;
            above = get_scalar(pressureField, i - 1, j);
            below = get_scalar(pressureField, i + 1, j);
            left = get_scalar(pressureField, i, j - 1);
            right = get_scalar(pressureField, i, j + 1);
            pressureFieldNew[i][j] = ((above + below + left + right) - divergence[i][j]) * 0.25f;
        }
    }
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            pressureField[i][j] = pressureFieldNew[i][j];
        }
    }
}

void gradient(float pressureField[MAX_HEIGHT][MAX_WIDTH], vec2 gradient[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            float above, below, left, right;
            above = get_scalar(pressureField, i - 1, j);
            below = get_scalar(pressureField, i + 1, j);
            left = get_scalar(pressureField, i, j - 1);
            right = get_scalar(pressureField, i, j + 1);
            gradient[i][j] = (vec2){.x = 0.5f*(right - left), .y = 0.5f*(below - above)};
        } 
    }
}

void sub_vector_fields(vec2 a[MAX_HEIGHT][MAX_WIDTH], vec2 b[MAX_HEIGHT][MAX_WIDTH]) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            a[i][j].x -= b[i][j].x;
            a[i][j].y -= b[i][j].y;
        }
    }
}

void vector_diffuse(vec2 x[MAX_HEIGHT][MAX_WIDTH], vec2 x0[MAX_HEIGHT][MAX_WIDTH], float diff, float deltaTime) {
    float a = deltaTime * diff;

    for (int k = 0; k < 30; k++) {
        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                vec2 above, below, left, right;
                above = get_vec(x, i - 1, j);
                below = get_vec(x, i + 1, j);
                left = get_vec(x, i, j - 1);
                right = get_vec(x, i, j + 1);

                x[i][j].x = (x0[i][j].x + a * (above.x + below.x + left.x + right.x)) / (1.0f + 4.0f * a);
                x[i][j].y = (x0[i][j].y + a * (above.y + below.y + left.y + right.y)) / (1.0f + 4.0f * a);
            }
        }
    }
}

void scalar_diffuse(float x[MAX_HEIGHT][MAX_WIDTH], float x0[MAX_HEIGHT][MAX_WIDTH], float diff, float deltaTime) {
    float a = deltaTime * diff;

    for (int k = 0; k < 30; k++) {
        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                float above, below, left, right;
                above = get_scalar(x, i - 1, j);
                below = get_scalar(x, i + 1, j);
                left = get_scalar(x, i, j - 1);
                right = get_scalar(x, i, j + 1);

                x[i][j] = (x0[i][j] + a * (above + below + left + right)) / (1.0f + 4.0f * a);
            }
        }
    }
}

float clamp_float(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float sample_scalar(float f[MAX_HEIGHT][MAX_WIDTH], float x, float y) {
    x = clamp_float(x, 0.0f, width - 1.001f);
    y = clamp_float(y, 0.0f, height - 1.001f);

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x1 >= width) x1 = width - 1;
    if (y1 >= height) y1 = height - 1;

    float sx = x - x0;
    float sy = y - y0;

    float a = f[y0][x0];
    float b = f[y0][x1];
    float c = f[y1][x0];
    float d = f[y1][x1];

    float top = a + sx * (b - a);
    float bottom = c + sx * (d - c);

    return top + sy * (bottom - top);
}

void advect_scalar(float dst[MAX_HEIGHT][MAX_WIDTH], float src[MAX_HEIGHT][MAX_WIDTH], vec2 vel[MAX_HEIGHT][MAX_WIDTH], float dt) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float oldX = x - vel[y][x].x * dt;
            float oldY = y - vel[y][x].y * dt;

            dst[y][x] = sample_scalar(src, oldX, oldY);
        }
    }
}

float clamp_int(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

vec2 sample_vec(vec2 field[MAX_HEIGHT][MAX_WIDTH], float x, float y) {
    int x0 = clamp_int((int)floorf(x), 0, width - 1);
    int y0 = clamp_int((int)floorf(y), 0, height - 1);
    int x1 = clamp_int(x0 + 1, 0, width - 1);
    int y1 = clamp_int(y0 + 1, 0, height - 1);

    float sx = x - floorf(x);
    float sy = y - floorf(y);

    vec2 p00 = field[y0][x0];
    vec2 p10 = field[y0][x1];
    vec2 p01 = field[y1][x0];
    vec2 p11 = field[y1][x1];

    vec2 result;

    result.x = (1-sy) * ((1-sx)*p00.x + sx*p10.x) + sy * ((1-sx)*p01.x + sx*p11.x);

    result.y = (1-sy) * ((1-sx)*p00.y + sx*p10.y) + sy * ((1-sx)*p01.y + sx*p11.y);

    return result;
}

void advect_vec(vec2 dst[MAX_HEIGHT][MAX_WIDTH], vec2 src[MAX_HEIGHT][MAX_WIDTH], vec2 vel[MAX_HEIGHT][MAX_WIDTH], float dt) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float oldX = x - vel[y][x].x * dt;
            float oldY = y - vel[y][x].y * dt;

            dst[y][x] = sample_vec(src, oldX, oldY);
        }
    }
}

// dye
float dye_density(vec3 c) {
    float m = c.r > c.g ? c.r : c.g;
    return m > c.b ? m : c.b;
}

vec3 sample_vec3(vec3 f[MAX_HEIGHT][MAX_WIDTH], float x, float y) {
    x = clamp_float(x, 0.0f, width - 1.001f);
    y = clamp_float(y, 0.0f, height - 1.001f);

    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 >= width)  x1 = width - 1;
    if (y1 >= height) y1 = height - 1;

    float sx = x - x0, sy = y - y0;

    vec3 a = f[y0][x0], b = f[y0][x1], c = f[y1][x0], d = f[y1][x1];
    vec3 out;
    float top, bot;

    top = a.r + sx * (b.r - a.r); bot = c.r + sx * (d.r - c.r);  out.r = top + sy * (bot - top);
    top = a.g + sx * (b.g - a.g); bot = c.g + sx * (d.g - c.g);  out.g = top + sy * (bot - top);
    top = a.b + sx * (b.b - a.b); bot = c.b + sx * (d.b - c.b);  out.b = top + sy * (bot - top);

    return out;
}

void advect_vec3(vec3 dst[MAX_HEIGHT][MAX_WIDTH], vec3 src[MAX_HEIGHT][MAX_WIDTH], vec2 vel[MAX_HEIGHT][MAX_WIDTH], float dt) {
    /* for dissipation */
    float retention = expf(-active_program.dissipation * dt);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            vec3 c = sample_vec3(src, x - vel[y][x].x * dt, y - vel[y][x].y * dt);
            dst[y][x] = (vec3){c.r * retention, c.g * retention, c.b * retention};
        }
    }
}

void apply_dye_boundaries(vec3 d[MAX_HEIGHT][MAX_WIDTH]) {
    for (int x = 1; x < width - 1; x++) {
        d[0][x] = d[1][x];
        d[height - 1][x] = d[height - 2][x];
    }
    for (int y = 1; y < height - 1; y++) {
        d[y][0] = d[y][1];
        d[y][width - 1] = d[y][width - 2];
    }
    d[0][0] = d[1][1];
    d[0][width - 1] = d[1][width - 2];
    d[height - 1][0] = d[height - 2][1];
    d[height - 1][width - 1] = d[height - 2][width - 2];
}

static int pair_for(vec3 c, float d) {
    float r = c.r / d;
    float g = c.g / d;
    float b = c.b / d;

    int r6 = (int)(clamp_float(r, 0, 1) * 5.0f + 0.5f);
    int g6 = (int)(clamp_float(g, 0, 1) * 5.0f + 0.5f);
    int b6 = (int)(clamp_float(b, 0, 1) * 5.0f + 0.5f);

    return 1 + r6 * 36 + g6 * 6 + b6;
}

static void inject(vec3 dye[MAX_HEIGHT][MAX_WIDTH], vec2 vel[MAX_HEIGHT][MAX_WIDTH],
        int sx, int sy, int s, vec3 col, float vx, float vy, float rate_per_second, float dt) {
    float rate = rate_per_second * dt;
    for (int oy = -s; oy <= s; oy++) {
        for (int ox = -s; ox <= s; ox++) {
            int y = sy + oy, x = sx + ox;
            if (x >= 1 && x <= width - 2 && y >= 1 && y <= height - 2) {
                float r = dye[y][x].r + col.r * rate;
                float g = dye[y][x].g + col.g * rate;
                float b = dye[y][x].b + col.b * rate;
                dye[y][x].r = r < 1.0f ? r : 1.0f;
                dye[y][x].g = g < 1.0f ? g : 1.0f;
                dye[y][x].b = b < 1.0f ? b : 1.0f;
                vel[y][x].x += vx * dt;
                vel[y][x].y += vy * dt;
            }
        }
    }
}

static int init_colors(void) {
    if (!has_colors()) {
        return 0;
    }

    if (start_color() == ERR) return 0;

    if (COLORS < 256 || COLOR_PAIRS < 217) {
        return 0;
    }

    int background = use_default_colors() == OK ? -1 : COLOR_BLACK;

    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int pair = 1 + r * 36 + g * 6 + b;
                int color = 16 + r * 36 + g * 6 + b;

                if (init_pair(pair, color, background) == ERR) return 0;
            }
        }
    }
    return 1;
}

// rendering
void render(vec3 dyeField[MAX_HEIGHT][MAX_WIDTH], double deltaTime) {
    //clear();
    erase();
    const char *ascii = "  .:-=+*#%@";
    int size = (int)strlen(ascii);
    for (int i = 1; i < displayHeight + 1; i++) {
        for (int j = 0; j < displayWidth; j++) {
            vec3 c = dyeField[i][j + 1];
            float d = dye_density(c);
            int idx = (int)(d * (size - 1));
            if (idx >= size) idx = size - 1;
            if (idx < 0) idx = 0;

            int bottom_right = i == getmaxy(stdscr) && j == getmaxx(stdscr) - 1;
            short pair = color_ready && d > 1e-4f ? pair_for(c, d) : 0;
            chtype ch = (chtype)ascii[idx] | COLOR_PAIR(pair);
            /* Inserting at the bottom-right avoids cursor wrap. */
            if (bottom_right) mvinsch(i - 1, j, ch);
            else mvaddch(i - 1, j, ch);
        }
    }
    if (args.fps) {
        vec3 sc = dyeField[height - 3][width / 2];
        mvprintw(displayHeight, 0, "fps: %.1f  src=(%.3f,%.3f,%.3f)  dt=%.4f",  /*mode=%d",*/
                1.0f / deltaTime, sc.r, sc.g, sc.b, deltaTime); /*, colorMode);*/
        //printw("fps: %.1f", fps);
    }
    refresh();
}

void render_vel(vec2 velocityField[MAX_HEIGHT][MAX_WIDTH]) {
    clear();
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            printw("(%6.2f,%6.2f)", velocityField[i][j].x, velocityField[i][j].y);
        }
        printw("\n");
    }
    refresh();
}

void render_scalar(float field[MAX_HEIGHT][MAX_WIDTH]) {
    clear();
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            printw("%6.2f ", field[i][j]);
        }
        printw("\n");
    }
    refresh();
}

static int parse_component(const char *text, size_t length, float *out) {
    if (length < 1 || length > 4) return 0;
    unsigned value = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isxdigit(c)) return 0;
        value = value * 16 + (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    *out = (float)value / (float)((1U << (length * 4)) - 1);
    return 1;
}

/* Replies arrive asynchronously in getch(), alongside mouse/keyboard input. */
static void apply_palette_reply(const char *text) {
    if (strncmp(text, "4;", 2)) return;
    char *end;
    unsigned long index = strtoul(text + 2, &end, 10);
    if (end == text + 2 || index > 15 || strncmp(end, ";rgb:", 5)) return;
    const char *r = end + 5, *slash1 = strchr(r, '/');
    if (!slash1) return;
    const char *g = slash1 + 1, *slash2 = strchr(g, '/');
    if (!slash2) return;
    const char *b = slash2 + 1;
    vec3 color;
    if (!parse_component(r, (size_t)(slash1 - r), &color.r) ||
        !parse_component(g, (size_t)(slash2 - g), &color.g) ||
        !parse_component(b, strlen(b), &color.b)) return;
    if (!args.col) return;
    int changed = 0;
    if (args.mouse && active_program.mouse_palette == (int)index) {
        changed = memcmp(&active_program.mouse_color, &color, sizeof color) != 0;
        active_program.mouse_color = color;
    }
    if (!args.mouse) {
        for (size_t i = 0; i < active_program.count; i++) {
            Source *source = &active_program.sources[i];
            if (source->palette == (int)index) {
                if (memcmp(&source->color, &color, sizeof color)) changed = 1;
                source->color = color;
            }
        }
    }
    if (changed) clearok(stdscr, TRUE);
}

static void request_palette_colors(void) {
    if (!color_ready) return;
    int needed[16] = {0};
    if (args.mouse && active_program.mouse_palette >= 0)
        needed[active_program.mouse_palette] = 1;
    if (!args.mouse)
        for (size_t i = 0; i < active_program.count; i++)
            if (active_program.sources[i].palette >= 0)
                needed[active_program.sources[i].palette] = 1;
    for (int i = 0; i < 16; i++)
        if (needed[i]) fprintf(stdout, "\033]4;%d;?\033\\", i);
    fflush(stdout);
}

typedef struct {
    int down;
    int x, y;
} MouseStroke;

static void add_mouse_smoke(MEVENT event, MouseStroke *stroke,
        vec3 dye[MAX_HEIGHT][MAX_WIDTH],
        vec2 velocity[MAX_HEIGHT][MAX_WIDTH], vec3 color) {
    if (event.bstate & BUTTON1_RELEASED) {
        stroke->down = 0;
        return;
    }
    if (!(event.bstate & (BUTTON1_PRESSED | REPORT_MOUSE_POSITION))) return;
    if (!(event.bstate & BUTTON1_PRESSED) && !stroke->down) return;
    if (event.x < 0 || event.x >= displayWidth || event.y < 0 || event.y >= displayHeight) {
        stroke->down = 0;
        return;
    }
    int x = event.x + 1, y = event.y + 1;
    int dx = stroke->down ? x - stroke->x : 0;
    int dy = stroke->down ? y - stroke->y : 0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps < 1) steps = 1;
    for (int k = 1; k <= steps; k++) {
        int cx = x - dx + dx * k / steps;
        int cy = y - dy + dy * k / steps;
        for (int oy = -2; oy <= 2; oy++) {
            for (int ox = -2; ox <= 2; ox++) {
                int px = cx + ox, py = cy + oy;
                int radius2 = ox * ox + oy * oy;
                if (radius2 > 4 || px < 1 || px >= width - 1 || py < 1 || py >= height - 1)
                    continue;
                float weight = 1.0f - radius2 / 5.0f;
                vec3 *c = &dye[py][px];
                c->r = clamp_float(c->r + color.r * 0.65f * weight, 0, 1);
                c->g = clamp_float(c->g + color.g * 0.65f * weight, 0, 1);
                c->b = clamp_float(c->b + color.b * 0.65f * weight, 0, 1);
                velocity[py][px].x = clamp_float(velocity[py][px].x + 12.0f * dx / steps * weight, -80, 80);
                velocity[py][px].y = clamp_float(velocity[py][px].y + 12.0f * dy / steps * weight, -80, 80);
            }
        }
    }
    *stroke = (MouseStroke){1, x, y};
}

typedef struct {
    int kind;                   /* ']' for OSC or '[' for CSI. */
    int escaped, overflow;
    double started;
    size_t length;
    char text[256];
} TerminalReply;

static void finish_terminal_reply(TerminalReply *reply, MouseStroke *stroke,
                                 vec3 dye[MAX_HEIGHT][MAX_WIDTH],
                                 vec2 velocity[MAX_HEIGHT][MAX_WIDTH], int usable) {
    reply->text[reply->length] = '\0';
    if (!reply->overflow && reply->kind == ']') apply_palette_reply(reply->text);
    /* Support SGR mouse reports even with terminfo using the legacy format. */
    if (!reply->overflow && reply->kind == '[' && args.mouse && usable) {
        unsigned button, x, y;
        char action, extra;
        if (sscanf(reply->text, "<%u;%u;%u%c%c", &button, &x, &y, &action, &extra) == 4 &&
            button <= 63 && (button & 3) == 0 && x >= 1 && y >= 1 &&
            x <= MAX_WIDTH && y <= MAX_HEIGHT && (action == 'M' || action == 'm')) {
            MEVENT event = {0};
            event.x = (int)x - 1;
            event.y = (int)y - 1;
            event.bstate = action == 'm' ? BUTTON1_RELEASED :
                           (button & 32) ? REPORT_MOUSE_POSITION : BUTTON1_PRESSED;
            add_mouse_smoke(event, stroke, dye, velocity, active_program.mouse_color);
        }
    }
    *reply = (TerminalReply){0};
}

static int terminal_reply_input(int key, TerminalReply *reply, MouseStroke *stroke,
                                vec3 dye[MAX_HEIGHT][MAX_WIDTH],
                                vec2 velocity[MAX_HEIGHT][MAX_WIDTH], int usable) {
    if (!reply->kind) {
        if (key != 27) return 0;
        /* Distinguish a real Escape key from the start of a terminal report. */
        wtimeout(stdscr, 25);
        int next = getch();
        nodelay(stdscr, TRUE);
        if (next != ']' && next != '[') {
            if (next != ERR) ungetch(next);
            return 0;
        }
        reply->kind = next;
        reply->started = get_time();
        return 1;
    }
    if (key < 0 || key > 255) return 0;
    if ((reply->kind == ']' && (key == '\a' || (reply->escaped && key == '\\'))) ||
        (reply->kind == '[' && key >= 0x40 && key <= 0x7e)) {
        if (reply->kind == '[' && reply->length < sizeof reply->text - 1)
            reply->text[reply->length++] = (char)key;
        finish_terminal_reply(reply, stroke, dye, velocity, usable);
        return 1;
    }
    if (key == 27) { reply->escaped = 1; return 1; }
    if (reply->escaped) reply->overflow = 1;
    if (reply->length < sizeof reply->text - 1)
        reply->text[reply->length++] = (char)key;
    else reply->overflow = 1;
    return 1;
}

/* Allocate only the active rows; large static arrays can trigger costly
   transparent huge-page allocations even when a terminal is small. */
typedef struct {
    vec2 (*velocity)[MAX_WIDTH], (*old_velocity)[MAX_WIDTH], (*gradient)[MAX_WIDTH];
    vec3 (*dye)[MAX_WIDTH], (*old_dye)[MAX_WIDTH];
    float (*divergence)[MAX_WIDTH], (*pressure)[MAX_WIDTH], (*scratch)[MAX_WIDTH];
} Fields;

static void free_fields(Fields *fields) {
    free(fields->velocity); free(fields->old_velocity); free(fields->gradient);
    free(fields->dye); free(fields->old_dye);
    free(fields->divergence); free(fields->pressure); free(fields->scratch);
    *fields = (Fields){0};
}

static int allocate_fields(Fields *fields) {
    fields->velocity = calloc((size_t)height, sizeof *fields->velocity);
    fields->old_velocity = calloc((size_t)height, sizeof *fields->old_velocity);
    fields->gradient = calloc((size_t)height, sizeof *fields->gradient);
    fields->dye = calloc((size_t)height, sizeof *fields->dye);
    fields->old_dye = calloc((size_t)height, sizeof *fields->old_dye);
    fields->divergence = calloc((size_t)height, sizeof *fields->divergence);
    fields->pressure = calloc((size_t)height, sizeof *fields->pressure);
    fields->scratch = calloc((size_t)height, sizeof *fields->scratch);
    if (!fields->velocity || !fields->old_velocity || !fields->gradient ||
        !fields->dye || !fields->old_dye || !fields->divergence ||
        !fields->pressure || !fields->scratch) {
        free_fields(fields);
        return 0;
    }
    return 1;
}

int loop(void) {
    const double step = 1.0 / 40.0;
    double last = get_time();
    double accumulator = step;
    const double frame_interval = 1.0 / active_program.max_fps;
    double next_render = last, last_render = last - frame_interval;

    float viscosity = active_program.viscosity;
    Fields fields = {0};
    MouseStroke stroke = {0};
    TerminalReply reply = {0};
    double next_palette_query = 0;
    int usable = update_dimensions();
    if (usable && !allocate_fields(&fields)) return 0;
    while (!stop_requested) {
        double frame_start = get_time();
        if (reply.kind && frame_start - reply.started > 0.5)
            reply = (TerminalReply){0};
        int key;
        int resized = 0;
        while ((key = getch()) != ERR) {
            if (terminal_reply_input(key, &reply, &stroke, fields.dye, fields.velocity,
                                     usable && !resized)) continue;
            if (key == 'q' || key == 'Q' || key == 27) stop_requested = 1;
            if (key == KEY_RESIZE) resized = 1;
            if (key == KEY_MOUSE && args.mouse) {
                MEVENT event;
                if (getmouse(&event) == OK && usable && !resized)
                    add_mouse_smoke(event, &stroke, fields.dye, fields.velocity, active_program.mouse_color);
            }
        }
        if (stop_requested) break;
        if (resized) {
            stroke.down = 0;
            usable = update_dimensions();
            free_fields(&fields);
            if (usable && !allocate_fields(&fields)) return 0;
            accumulator = step;
            last = frame_start;
            next_render = frame_start;
            last_render = frame_start - frame_interval;
            clear();
        }
        if (!usable) {
            if (frame_start >= next_render) {
                erase();
                mvaddstr(0, 0, "Resize terminal");
                refresh();
                next_render = frame_start + frame_interval;
            }
            last = get_time();
            sleep_seconds(0.025);
            continue;
        }
        if (frame_start >= next_palette_query) {
            request_palette_colors();
            next_palette_query = frame_start + 1.0;
        }
        double elapsed = frame_start - last;
        last = frame_start;
        /* Bound catch-up to four steps after a stall or suspend. */
        accumulator += elapsed;
        if (accumulator > 4 * step) accumulator = 4 * step;
        while (accumulator >= step && !stop_requested) {
            float deltaTime = (float)step;
            if (!args.mouse) {
                for (size_t i = 0; i < active_program.count; i++) {
                    const Source *source = &active_program.sources[i];
                    int x = 1 + (int)lroundf(source->x * (width - 3));
                    int y = 1 + (int)lroundf(source->y * (height - 3));
                    inject(fields.dye, fields.velocity, x, y, source->radius,
                           source->color, source->vx, source->vy, source->rate, deltaTime);
                }
            }
            // ============= end setup ===========

            copy_vec_field(fields.old_velocity, fields.velocity);
            advect_vec(fields.velocity, fields.old_velocity, fields.velocity, deltaTime);
            copy_vec_field(fields.old_velocity, fields.velocity);
            vector_diffuse(fields.velocity, fields.old_velocity, viscosity, deltaTime);
            apply_velocity_boundaries(fields.velocity);
            calculate_divergence(fields.velocity, fields.divergence);

            //project
            init_scalar_field(fields.pressure); // reset pressure before every solve
            for (int i = 0; i < 30; i++) {
                solve(fields.pressure, fields.divergence, fields.scratch);
            }
            gradient(fields.pressure, fields.gradient);
            sub_vector_fields(fields.velocity, fields.gradient);
            apply_velocity_boundaries(fields.velocity);

            // dye
            copy_dye_field(fields.old_dye, fields.dye);
            advect_vec3(fields.dye, fields.old_dye, fields.velocity, deltaTime);
            apply_dye_boundaries(fields.dye);
            //copy_dye_field(fields.old_dye, fields.dye);
            //scalar_diffuse(fields.dye, fields.old_dye, dyeDiffusion, deltaTime);

            accumulator -= step;
        }
        double now = get_time();
        if (now >= next_render) {
            render(fields.dye, now - last_render);
            last_render = now;
            next_render = now + frame_interval;
        }
        now = get_time();
        /* Wake for either the next simulation step or the next render.
           A low render cap must not slow down smoke movement or input. */
        double remaining = step - accumulator - (now - frame_start);
        double until_render = next_render - now;
        if (until_render < remaining) remaining = until_render;
        if (remaining > 0) sleep_seconds(remaining);
    }
    free_fields(&fields);
    return 1;
}

int main(int argc, char*argv[]) {
    args = (Args){.program = "default", .col = 1};
    enum { COLOR_ON = 256 };
    static const struct option options[] = {
        {"color",   no_argument, NULL, COLOR_ON},
        {"colour",  no_argument, NULL, COLOR_ON},
        {"no-color", no_argument, NULL, 'c'},
        {"no-colour", no_argument, NULL, 'c'},
        {"fps",     no_argument, NULL, 'f'},
        {"max-fps", required_argument, NULL, 'F'},
        {"mouse",   no_argument, NULL, 'm'},
        {"program", required_argument, NULL, 'p'},
        {"no-dissipation", no_argument, NULL, 'n'},
        {"check-config", no_argument, NULL, 't'},
        {"help",    no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}
    };
    const char *program = argv[0];
    int option;
    opterr = 0;
    while ((option = getopt_long(argc, argv, "cfF:mnp:thV", options, NULL)) != -1) {
        switch (option) {
            case 'c': args.col = 0; break;
            case COLOR_ON: args.col = 1; break;
            case 'f': args.fps = 1; break;
            case 'F':
                if (!parse_max_fps(optarg, &args.max_fps)) {
                    fprintf(stderr, "%s: --max-fps requires an integer from 1 to 240\n", program);
                    return EXIT_FAILURE;
                }
                break;
            case 'm': args.mouse = 1; break;
            case 'p': args.program = optarg; break;
            case 'n': args.no_dissipation = 1; break;
            case 't': args.check_config = 1; break;
            case 'h': usage(stdout, program); return EXIT_SUCCESS;
            case 'V': puts("CTermFluid " CTERMFLUID_VERSION); return EXIT_SUCCESS;
            default:
                      fprintf(stderr, "%s: invalid option or missing value near '%s'\n\n",
                              program, argv[optind - 1]);
                      usage(stderr, program);
                      return EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n\n", program, argv[optind]);
        usage(stderr, program);
        return EXIT_FAILURE;
    }
    char path[4096], error[4608];
    if (!find_program(args.program, path, sizeof path)) {
        fprintf(stderr, "%s: program '%s' not found; use -p ./path/to/program.conf\n",
                program, args.program);
        return EXIT_FAILURE;
    }
    if (!load_program(path, &active_program, error, sizeof error)) {
        fprintf(stderr, "%s: %s\n", program, error);
        return EXIT_FAILURE;
    }
    if (args.no_dissipation) active_program.dissipation = 0;
    if (args.max_fps) active_program.max_fps = args.max_fps;
    if (args.check_config) {
        printf("%s: valid (%zu sources, dissipation=%g, max_fps=%d)\n", path,
               active_program.count, (double)active_program.dissipation, active_program.max_fps);
        return EXIT_SUCCESS;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "%s: an interactive terminal is required\n", program);
        return EXIT_FAILURE;
    }
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (stop_requested) return 0;

    initscr();
    int previous_cursor = curs_set(0);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    update_dimensions();
    if (args.col) color_ready = init_colors();
    if (args.mouse) {
        mouseinterval(0);
        mousemask(BUTTON1_PRESSED | BUTTON1_RELEASED | REPORT_MOUSE_POSITION, NULL);
        /* SGR mouse coordinates support the full grid; legacy reports are
           limited to about 223 columns/rows. Enable button-drag reports too. */
        fputs("\033[?1006h\033[?1002h", stdout);
        fflush(stdout);
    }

    int success = loop();

    if (args.mouse) {
        fputs("\033[?1002l\033[?1006l", stdout);
        fflush(stdout);
        mousemask(0, NULL);
    }
    if (previous_cursor != ERR) curs_set(previous_cursor);
    endwin();
    if (!success) fprintf(stderr, "%s: unable to allocate simulation buffers\n", program);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

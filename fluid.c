#include <bits/time.h>
#include <ncurses.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <ncurses.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>

#define MAX_WIDTH 200
#define MAX_HEIGHT 100
int width;
int height;
int displayWidth;
int displayHeight;

typedef struct {
    float x;
    float y;
} vec2;

typedef struct {
    float r;
    float g;
    float b;
} vec3;

typedef struct {
    int fps;
    int col;  
    int mouse;
} Args;

Args args;
static int color_ready;
static volatile sig_atomic_t stop_requested;

static void usage(FILE *stream, const char *program) {
    fprintf(stream,
            "Usage: %s [OPTIONS]\n"
            "ASCII fluid simulation for the terminal.\n"
            "\n"
            "Options:\n"
            "  -c, --color, --colour  Enable terminal palette colors\n"
            "  -f, --fps              Show frame rate and simulation statistics\n"
            "  -m, --mouse            Start empty; drag the left mouse button to add smoke\n"
            "  -h, --help             Show this help and exit\n"
            "  -V, --version          Show version information and exit\n"
            "\n"
            "Press q, Escape, or Ctrl+C to quit. Resizing resets the simulation.\n",
            program);
}

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
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

void solve(float pressureField[MAX_HEIGHT][MAX_WIDTH], float divergence[MAX_HEIGHT][MAX_WIDTH]) {
    float pressureFieldNew[MAX_HEIGHT][MAX_WIDTH];
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
    float retention = expf(-0.05f * dt);

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
        int sx, int sy, int s, vec3 col, float vy, float dt) {
    float rate = 3.0f * dt;
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

static float parse_component(const char *text, size_t length) {
    char buffer[8];

    if (length >= sizeof(buffer))
        return 0.0f;

    memcpy(buffer, text, length);
    buffer[length] = '\0';
    unsigned long value = strtoul(buffer, NULL, 16);
    unsigned long max = (1UL << (length * 4)) - 1;

    return (float)value / (float)max;
}

int terminal_palette_color(int index, vec3 *color) {
    int fd = open("/dev/tty", O_RDWR);

    if (fd == -1)
        return 0;

    struct termios old_term;
    struct termios raw_term;

    if (tcgetattr(fd, &old_term) == -1) {
        close(fd);
        return 0;
    }

    raw_term = old_term;
    raw_term.c_lflag &= ~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 0;
    raw_term.c_cc[VTIME] = 2;

    tcsetattr(fd, TCSANOW, &raw_term);
    char query[32];
    snprintf(query, sizeof(query), "\033]4;%d;?\033\\", index);
    write(fd, query, strlen(query));
    char response[128] = {0};
    size_t length = 0;

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN
    };

    while (length < sizeof(response) - 1) {
        if (poll(&pfd, 1, 100) <= 0)
            break;
        ssize_t count = read(fd, response + length, sizeof(response) - length - 1);
        if (count <= 0)
            break;
        length += count;
        response[length] = '\0';
        if (strchr(response, '\a') || strstr(response, "\033\\"))
            break;
    }

    tcsetattr(fd, TCSANOW, &old_term);
    close(fd);

    char *rgb = strstr(response, "rgb:");
    if (rgb == NULL)
        return 0;
    rgb += 4;
    char *slash1 = strchr(rgb, '/');
    if (slash1 == NULL)
        return 0;
    char *slash2 = strchr(slash1 + 1, '/');
    if (slash2 == NULL)
        return 0;
    char *end = slash2 + 1;

    while (*end && *end != '\033' && *end != '\a') {
        end++;
    }

    color->r = parse_component(rgb, slash1 - rgb);
    color->g = parse_component(slash1 + 1, slash2 - slash1 - 1);
    color->b = parse_component(slash2 + 1, end - slash2 - 1);

    return 1;
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

void loop(vec3 colA, vec3 colB) {
    const double step = 1.0 / 40.0;
    double last = get_time();
    double accumulator = step;

    float viscosity = 0.0001f;
    vec2 velocityField[MAX_HEIGHT][MAX_WIDTH];
    init_vec_field(velocityField);
    vec2 velocityOld[MAX_HEIGHT][MAX_WIDTH];
    vec3 dyeField[MAX_HEIGHT][MAX_WIDTH];
    vec3 dyeOld[MAX_HEIGHT][MAX_WIDTH];
    init_vec3_field(dyeField);
    float divergence[MAX_HEIGHT][MAX_WIDTH];
    float pressureField[MAX_HEIGHT][MAX_WIDTH];
    vec2 deltaP[MAX_HEIGHT][MAX_WIDTH];

    MouseStroke stroke = {0};
    int usable = update_dimensions();
    while (!stop_requested) {
        double frame_start = get_time();
        int key;
        int resized = 0;
        while ((key = getch()) != ERR) {
            if (key == 'q' || key == 'Q' || key == 27) stop_requested = 1;
            if (key == KEY_RESIZE) resized = 1;
            if (key == KEY_MOUSE && args.mouse) {
                MEVENT event;
                if (getmouse(&event) == OK && usable && !resized)
                    add_mouse_smoke(event, &stroke, dyeField, velocityField, colA);
            }
        }
        if (stop_requested) break;
        if (resized) {
            stroke.down = 0;
            usable = update_dimensions();
            memset(velocityField, 0, sizeof velocityField);
            memset(dyeField, 0, sizeof dyeField);
            accumulator = step;
            last = frame_start;
            clear();
        }
        if (!usable) {
            erase();
            mvaddstr(0, 0, "Resize terminal");
            refresh();
            last = get_time();
            usleep(25000);
            continue;
        }
        double elapsed = frame_start - last;
        last = frame_start;
        /* Bound catch-up to four steps after a stall or suspend. */
        accumulator += elapsed;
        if (accumulator > 4 * step) accumulator = 4 * step;
        while (accumulator >= step && !stop_requested) {
            float deltaTime = (float)step;
            if (!args.mouse) {
                inject(
                        dyeField,
                        velocityField,
                        width / 2,
                        height - 3,
                        5,
                        colA,
                        -100.0f,
                        deltaTime
                      );

                inject(
                        dyeField,
                        velocityField,
                        width / 2 + 5,
                        1,
                        5,
                        colB,
                        100.0f,
                        deltaTime
                      );
            }
            // ============= end setup ===========

            memcpy(velocityOld, velocityField, (size_t)height * sizeof velocityField[0]);
            advect_vec(velocityField, velocityOld, velocityField, deltaTime);
            memcpy(velocityOld, velocityField, (size_t)height * sizeof velocityField[0]);
            vector_diffuse(velocityField, velocityOld, viscosity, deltaTime);
            apply_velocity_boundaries(velocityField);
            calculate_divergence(velocityField, divergence);

            //project
            memset(pressureField, 0, (size_t)height * sizeof pressureField[0]); // reset pressure before every solve
            for (int i = 0; i < 30; i++) {
                solve(pressureField, divergence);
            }
            gradient(pressureField, deltaP);
            sub_vector_fields(velocityField, deltaP);
            apply_velocity_boundaries(velocityField);

            // dye
            memcpy(dyeOld, dyeField, (size_t)height * sizeof dyeField[0]);
            advect_vec3(dyeField, dyeOld, velocityField, deltaTime);
            apply_dye_boundaries(dyeField);
            //memcpy(dyeOld, dyeField, (size_t)height * sizeof dyeField[0]);
            //scalar_diffuse(dyeField, dyeOld, dyeDiffusion, deltaTime);

            accumulator -= step;
        }
        render(dyeField, elapsed > 0 ? elapsed : step);
        double remaining = step - (get_time() - frame_start);
        if (remaining > 0) usleep((useconds_t)(remaining * 1000000.0));
    }
}

int main(int argc, char*argv[]) {
    args = (Args){.fps = 0, .col = 0};
    static const struct option options[] = {
        {"color",   no_argument, NULL, 'c'},
        {"colour",  no_argument, NULL, 'c'},
        {"fps",     no_argument, NULL, 'f'},
        {"mouse",   no_argument, NULL, 'm'},
        {"help",    no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}
    };
    const char *program = argv[0];
    int option;
    opterr = 0;
    while ((option = getopt_long(argc, argv, "cfmhV", options, NULL)) != -1) {
        switch (option) {
            case 'c': args.col = 1; break;
            case 'f': args.fps = 1; break;
            case 'm': args.mouse = 1; break;
            case 'h': usage(stdout, program); return EXIT_SUCCESS;
            case 'V': puts("fluid 0.3.0"); return EXIT_SUCCESS;
            default:
                      fprintf(stderr, "%s: invalid option '%s'\n\n",
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
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    vec3 colA = {0.0f, 1.0f, 1.0f};
    vec3 colB = {0.0f, 1.0f, 0.0f};
    if (args.col) {
        terminal_palette_color(6, &colA);
        if (!stop_requested) terminal_palette_color(2, &colB);
    }
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
        fputs("\033[?1002h", stdout);
        fflush(stdout);
    }

    loop(colA, colB);

    if (args.mouse) {
        fputs("\033[?1002l", stdout);
        fflush(stdout);
        mousemask(0, NULL);
    }
    if (previous_cursor != ERR) curs_set(previous_cursor);
    endwin();
    return 0;
}

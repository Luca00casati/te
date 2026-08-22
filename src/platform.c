// nanosleep needs POSIX visibility that plain -std=c11 doesn't expose.
#define _POSIX_C_SOURCE 199309L

#include "platform.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <GL/gl.h>

#include <png.h>

static GLFWwindow *window = NULL;
static bool initialized = false;
static int target_fps = 60;
static double start_time = 0.0;
static double frame_start_time = 0.0;

// --- keyboard: per-key edge state, refreshed once per frame ---------------
#define TE_KEY_MAX (GLFW_KEY_LAST + 1)
static bool key_pressed[TE_KEY_MAX];
static bool key_pressed_repeat[TE_KEY_MAX];
static bool key_released[TE_KEY_MAX];

#define KEY_QUEUE_CAP 16
static int key_press_queue[KEY_QUEUE_CAP];
static size_t key_press_queue_len = 0, key_press_queue_pos = 0;

// --- typed-codepoint FIFO, refilled from GLFW's char callback each frame --
#define CHAR_QUEUE_CAP 64
static int char_queue[CHAR_QUEUE_CAP];
static size_t char_queue_head = 0, char_queue_tail = 0;

static void charQueuePush(int cp) {
    size_t next = (char_queue_tail + 1) % CHAR_QUEUE_CAP;
    if (next == char_queue_head) return; // full: drop (shouldn't happen at 64 deep)
    char_queue[char_queue_tail] = cp;
    char_queue_tail = next;
}
int platformCharPressed(void) {
    if (char_queue_head == char_queue_tail) return 0;
    int cp = char_queue[char_queue_head];
    char_queue_head = (char_queue_head + 1) % CHAR_QUEUE_CAP;
    return cp;
}

// --- mouse -----------------------------------------------------------------
static float mouse_x = 0, mouse_y = 0;
static float mouse_wheel = 0;
static bool mouse_left_down = false;
static bool mouse_left_pressed = false;

// --- GLFW callbacks, all firing during glfwPollEvents() --------------------
static void keyCallback(GLFWwindow *w, int key, int scancode, int action, int mods) {
    (void)w; (void)scancode; (void)mods;
    if (key < 0 || key > GLFW_KEY_LAST) return;
    if (action == GLFW_PRESS) {
        key_pressed[key] = true;
        if (key_press_queue_len < KEY_QUEUE_CAP) key_press_queue[key_press_queue_len++] = key;
    } else if (action == GLFW_REPEAT) {
        key_pressed_repeat[key] = true;
    } else if (action == GLFW_RELEASE) {
        key_released[key] = true;
    }
}
static void charCallback(GLFWwindow *w, unsigned int codepoint) {
    (void)w;
    charQueuePush((int)codepoint);
}
static void mouseButtonCallback(GLFWwindow *w, int button, int action, int mods) {
    (void)w; (void)mods;
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) mouse_left_pressed = true;
}
static void scrollCallback(GLFWwindow *w, double xoffset, double yoffset) {
    (void)w; (void)xoffset;
    mouse_wheel += (float)yoffset;
}

void platformInit(const char *title, int fps) {
    glfwInit();
    target_fps = fps;

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window = glfwCreateWindow(1, 1, title, NULL, NULL);

    GLFWmonitor *primary = glfwGetPrimaryMonitor();
    const GLFWvidmode *mode = primary ? glfwGetVideoMode(primary) : NULL;
    int win_w = mode ? mode->width / 2 : 640;
    int win_h = mode ? mode->height / 2 : 400;
    glfwSetWindowSize(window, win_w, win_h);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // manual frame pacing below, not vsync

    glfwSetKeyCallback(window, keyCallback);
    glfwSetCharCallback(window, charCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);

    glfwShowWindow(window);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    start_time = glfwGetTime();
    frame_start_time = start_time;

    initialized = true;
}

void platformShutdown(void) {
    if (window) glfwDestroyWindow(window);
    window = NULL;
    initialized = false;
    glfwTerminate();
}

void *platformGLContext(void) { return initialized ? (void *)window : NULL; }

void platformPollEvents(void) {
    if (!initialized) return;
    memset(key_pressed, 0, sizeof(key_pressed));
    memset(key_pressed_repeat, 0, sizeof(key_pressed_repeat));
    memset(key_released, 0, sizeof(key_released));
    key_press_queue_len = 0;
    key_press_queue_pos = 0;
    mouse_wheel = 0;
    mouse_left_pressed = false;

    glfwPollEvents();

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    mouse_x = (float)mx;
    mouse_y = (float)my;
    mouse_left_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    if (target_fps > 0) {
        double elapsed = glfwGetTime() - frame_start_time;
        double target = 1.0 / (double)target_fps;
        if (elapsed < target) {
            double sleep_s = target - elapsed;
            struct timespec ts;
            ts.tv_sec = (time_t)sleep_s;
            ts.tv_nsec = (long)((sleep_s - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
    }
    frame_start_time = glfwGetTime();
}

bool platformWindowShouldClose(void) {
    return initialized && glfwWindowShouldClose(window);
}

void platformScreenSize(float *w, float *h) {
    if (!initialized) { *w = 0; *h = 0; return; }
    int iw, ih;
    glfwGetFramebufferSize(window, &iw, &ih);
    *w = (float)iw;
    *h = (float)ih;
}

bool platformKeyDown(int key) {
    if (!initialized || key < 0 || key > GLFW_KEY_LAST) return false;
    return glfwGetKey(window, key) == GLFW_PRESS;
}
bool platformKeyPressed(int key) {
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    return key_pressed[key];
}
bool platformKeyPressedRepeat(int key) {
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    return key_pressed_repeat[key];
}
bool platformKeyReleased(int key) {
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    return key_released[key];
}
bool platformAnyKeyPressed(void) {
    for (int i = 0; i <= GLFW_KEY_LAST; i++)
        if (key_pressed[i] || key_pressed_repeat[i]) return true;
    return false;
}
int platformKeyPressedQueue(void) {
    if (key_press_queue_pos >= key_press_queue_len) return 0;
    return key_press_queue[key_press_queue_pos++];
}

void platformMousePos(float *x, float *y) { *x = mouse_x; *y = mouse_y; }
float platformMouseWheel(void) { return mouse_wheel; }
bool platformMouseLeftDown(void) { return mouse_left_down; }
bool platformMouseLeftPressed(void) { return mouse_left_pressed; }

const char *platformGetClipboardText(void) {
    if (!initialized) return "";
    const char *s = glfwGetClipboardString(window);
    return s ? s : "";
}
void platformSetClipboardText(const char *text) {
    if (!initialized) return;
    glfwSetClipboardString(window, text);
}

double platformTime(void) {
    if (!initialized) return 0.0;
    return glfwGetTime() - start_time;
}

void platformBeginDrawing(void) { (void)0; }

void platformClearBackground(Color color) {
    if (!initialized) return;
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    // Top-left origin, y increasing downward -- matches how the editor
    // logic already thinks about pixel coordinates.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, fb_w, fb_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void platformEndDrawing(void) {
    if (!initialized) return;
    glfwSwapBuffers(window);
}

void platformDrawRect(float x, float y, float w, float h, Color color) {
    if (!initialized) return;
    glDisable(GL_TEXTURE_2D);
    glColor4ub(color.r, color.g, color.b, color.a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void platformDrawTexture(Texture tex, float x, float y, Color color) {
    if (!initialized || tex.id == 0) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    // Texture's own RGB is white, alpha is coverage -- GL_MODULATE (GL's
    // default texture env mode) multiplies the sample by gl_Color, giving
    // the same "color mod + alpha mod" tint SDL_Renderer did.
    glColor4ub(color.r, color.g, color.b, color.a);
    float w = (float)tex.w, h = (float)tex.h;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + w, y);
    glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
    glTexCoord2f(0, 1); glVertex2f(x, y + h);
    glEnd();
}

void platformScreenshot(const char *path) {
    if (!initialized) return;
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    unsigned char *pixels = malloc((size_t)w * (size_t)h * 4);
    if (!pixels) return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(pixels);
        return;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    png_bytep *rows = NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        free(rows);
        if (png) png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(fp);
        free(pixels);
        return;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    // glReadPixels' rows run bottom-up; a row-pointer table listing them
    // top-down avoids copying to flip them.
    int stride = w * 4;
    rows = malloc((size_t)h * sizeof(png_bytep));
    if (rows) {
        for (int row = 0; row < h; row++) rows[row] = pixels + (size_t)(h - 1 - row) * (size_t)stride;
        png_write_image(png, rows);
        png_write_end(png, NULL);
    }

    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    free(pixels);
}

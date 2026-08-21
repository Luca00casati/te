#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static bool initialized = false;
static bool should_close = false;
static int target_fps = 60;
static Uint64 perf_freq = 0;
static Uint64 start_ticks = 0;
static Uint64 frame_start_ticks = 0;

// --- keyboard: per-scancode edge state, refreshed once per frame ----------
static bool key_pressed[SDL_NUM_SCANCODES];
static bool key_pressed_repeat[SDL_NUM_SCANCODES];
static bool key_released[SDL_NUM_SCANCODES];

#define KEY_QUEUE_CAP 16
static int key_press_queue[KEY_QUEUE_CAP];
static size_t key_press_queue_len = 0, key_press_queue_pos = 0;

// --- typed-codepoint FIFO, refilled from SDL_TEXTINPUT each frame ---------
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

// Decodes one UTF-8 codepoint from `s` (NUL-terminated, as SDL_TEXTINPUT
// hands it over); malformed bytes fall back to the raw byte, same
// leniency main.c's own utf8SeqLen/decodeCp use for the text buffer.
static void queueTextInputUtf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { charQueuePush(*p); p++; continue; }
        int ok = 1;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!ok) { charQueuePush(*p); p++; continue; }
        charQueuePush((int)cp);
        p += len;
    }
}

// --- mouse -----------------------------------------------------------------
static float mouse_x = 0, mouse_y = 0;
static float mouse_wheel = 0;
static bool mouse_left_down = false;
static bool mouse_left_pressed = false;

void platformInit(const char *title, int fps) {
    SDL_Init(SDL_INIT_VIDEO);
    target_fps = fps;

    SDL_Rect bounds = { 0, 0, 1280, 800 };
    SDL_GetDisplayBounds(0, &bounds);

    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1, 1, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    SDL_SetWindowSize(window, bounds.w / 2, bounds.h / 2);
    SDL_ShowWindow(window);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_StartTextInput();

    perf_freq = SDL_GetPerformanceFrequency();
    start_ticks = SDL_GetPerformanceCounter();
    frame_start_ticks = start_ticks;

    initialized = true;
}

void platformShutdown(void) {
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    renderer = NULL;
    window = NULL;
    initialized = false;
    SDL_Quit();
}

SDL_Renderer *platformRenderer(void) { return renderer; }

void platformPollEvents(void) {
    if (!initialized) return;
    memset(key_pressed, 0, sizeof(key_pressed));
    memset(key_pressed_repeat, 0, sizeof(key_pressed_repeat));
    memset(key_released, 0, sizeof(key_released));
    key_press_queue_len = 0;
    key_press_queue_pos = 0;
    mouse_wheel = 0;
    mouse_left_pressed = false;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                should_close = true;
                break;
            case SDL_KEYDOWN: {
                int sc = ev.key.keysym.scancode;
                if (sc >= 0 && sc < SDL_NUM_SCANCODES) {
                    if (ev.key.repeat) key_pressed_repeat[sc] = true;
                    else {
                        key_pressed[sc] = true;
                        if (key_press_queue_len < KEY_QUEUE_CAP) key_press_queue[key_press_queue_len++] = sc;
                    }
                }
                break;
            }
            case SDL_KEYUP: {
                int sc = ev.key.keysym.scancode;
                if (sc >= 0 && sc < SDL_NUM_SCANCODES) key_released[sc] = true;
                break;
            }
            case SDL_TEXTINPUT:
                queueTextInputUtf8(ev.text.text);
                break;
            case SDL_MOUSEWHEEL:
                mouse_wheel += (float)ev.wheel.y;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) mouse_left_pressed = true;
                break;
            default:
                break;
        }
    }

    int mx, my;
    Uint32 mstate = SDL_GetMouseState(&mx, &my);
    mouse_x = (float)mx;
    mouse_y = (float)my;
    mouse_left_down = (mstate & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

    if (target_fps > 0) {
        Uint64 now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - frame_start_ticks) / (double)perf_freq;
        double target = 1.0 / (double)target_fps;
        if (elapsed < target) SDL_Delay((Uint32)((target - elapsed) * 1000.0));
    }
    frame_start_ticks = SDL_GetPerformanceCounter();
}

bool platformWindowShouldClose(void) { return should_close; }

void platformScreenSize(float *w, float *h) {
    if (!initialized) { *w = 0; *h = 0; return; }
    int iw, ih;
    SDL_GetWindowSize(window, &iw, &ih);
    *w = (float)iw;
    *h = (float)ih;
}

bool platformKeyDown(int key) {
    if (!initialized || key < 0 || key >= SDL_NUM_SCANCODES) return false;
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    return state[key] != 0;
}
bool platformKeyPressed(int key) {
    if (key < 0 || key >= SDL_NUM_SCANCODES) return false;
    return key_pressed[key];
}
bool platformKeyPressedRepeat(int key) {
    if (key < 0 || key >= SDL_NUM_SCANCODES) return false;
    return key_pressed_repeat[key];
}
bool platformKeyReleased(int key) {
    if (key < 0 || key >= SDL_NUM_SCANCODES) return false;
    return key_released[key];
}
bool platformAnyKeyPressed(void) {
    for (int i = 0; i < SDL_NUM_SCANCODES; i++)
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

static char clipboard_buf_owned[1] = { 0 };
static char *last_clipboard = clipboard_buf_owned;
const char *platformGetClipboardText(void) {
    if (!initialized) return "";
    if (last_clipboard != clipboard_buf_owned) SDL_free(last_clipboard);
    if (SDL_HasClipboardText()) {
        last_clipboard = SDL_GetClipboardText();
        if (last_clipboard) return last_clipboard;
    }
    last_clipboard = clipboard_buf_owned;
    return "";
}
void platformSetClipboardText(const char *text) {
    if (!initialized) return;
    SDL_SetClipboardText(text);
}

double platformTime(void) {
    if (!initialized || perf_freq == 0) return 0.0;
    Uint64 now = SDL_GetPerformanceCounter();
    return (double)(now - start_ticks) / (double)perf_freq;
}

void platformBeginDrawing(void) { (void)0; }
void platformClearBackground(Color color) {
    if (!initialized) return;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
}
void platformEndDrawing(void) {
    if (!initialized) return;
    SDL_RenderPresent(renderer);
}

void platformDrawRect(float x, float y, float w, float h, Color color) {
    if (!initialized) return;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRectF(renderer, &rect);
}

void platformDrawTexture(SDL_Texture *tex, float x, float y, Color color) {
    if (!initialized || !tex) return;
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(tex, color.a);
    SDL_FRect dst = { x, y, (float)tw, (float)th };
    SDL_RenderCopyF(renderer, tex, NULL, &dst);
}

void platformScreenshot(const char *path) {
    if (!initialized) return;
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    unsigned char *pixels = malloc((size_t)w * (size_t)h * 4);
    if (!pixels) return;
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4) == 0) {
        stbi_write_png(path, w, h, 4, pixels, w * 4);
    }
    free(pixels);
}

// GLFW + OpenGL integration: the only place `te` touches GLFW/GL directly.
// Plays the same role for main.c that editor.h plays for script.c -- a
// small, explicit, named surface instead of scattering raw GLFW/GL calls
// through the editor logic. Owns the window/GL context, per-frame input
// polling (turning GLFW's callback-driven events into the same "is this
// held / was this just pressed or repeated or released / what was typed"
// query shape the editor logic wants once per frame), clipboard, timing,
// drawing primitives, and screenshot capture.
//
// Every query function here is a documented, explicit no-op/safe-default
// before platformInit() runs (returns false/0/NULL) -- tests/unit_te.c
// #includes main.c directly and never calls platformInit (no window is ever
// created for tests), so every platform* call it incidentally makes through
// ordinary editor logic (echo(), edit(), ...) must stay harmless.
#ifndef TE_PLATFORM_H
#define TE_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#include <GLFW/glfw3.h>

typedef struct { unsigned char r, g, b, a; } Color;

// A GL texture plus its pixel size (GL has no query-size-from-id call, so
// glyphs.c records it at creation time). `id == 0` means no texture (GL
// reserves 0 as "no texture", the same role SDL_Texture*'s NULL played).
typedef struct { unsigned int id; int w, h; } Texture;

// Creates a hidden 1x1 window, sizes it to half the primary display, then
// reveals it (avoids a visible resize flash), and starts the GL context.
// `target_fps` paces platformEndDrawing()'s frame delay.
void platformInit(const char *title, int target_fps);
void platformShutdown(void);

// Pumps GLFW's event queue for this frame, refreshing every query function
// below, and paces the frame against `target_fps` (nanosleep against the
// time since the last call -- GLFW has no SetTargetFPS equivalent). Call
// once per frame, before reading any input state.
void platformPollEvents(void);

bool platformWindowShouldClose(void);
void platformScreenSize(float *w, float *h);

// Keyboard: `key` is a GLFW_KEY_* code (physical key position, not layout-
// or case-dependent -- matches how te's Ctrl-chord bindings already think
// about keys). PressedRepeat fires on the OS's key-repeat while held, not
// just the initial press.
bool platformKeyDown(int key);
bool platformKeyPressed(int key);
bool platformKeyPressedRepeat(int key);
bool platformKeyReleased(int key);
// True if any key was pressed or repeated this frame (raylib's
// GetKeyPressed() != 0 -- used only to dismiss the help overlay on any key).
bool platformAnyKeyPressed(void);
// Dequeues the next GLFW_KEY_* freshly pressed this frame (FIFO, in event
// order; repeats aren't queued here, only real presses), or 0 once drained
// -- same "frame-scoped queue" shape as raylib's GetKeyPressed(), which
// detectCtrlTaps() needs to see *which* other key broke a Ctrl-tap run.
int platformKeyPressedQueue(void);

// Dequeues one typed Unicode codepoint from this frame's GLFW char-callback
// events (FIFO order), or 0 once the queue is drained -- same shape as
// raylib's GetCharPressed, which main.c's `while (...!=0)` drain loops rely
// on.
int platformCharPressed(void);

void platformMousePos(float *x, float *y);
float platformMouseWheel(void);
bool platformMouseLeftDown(void);
bool platformMouseLeftPressed(void);

// Only safe to call once the window exists (GLFW's clipboard needs a live
// window); no-ops (NULL / no-op) otherwise. The returned pointer is valid
// until the next platformGetClipboardText/platformSetClipboardText call
// (owned by GLFW, not by us -- no freeing needed).
const char *platformGetClipboardText(void);
void platformSetClipboardText(const char *text);

// Seconds, monotonic, 0 at platformInit.
double platformTime(void);

void platformBeginDrawing(void);
void platformClearBackground(Color color);
void platformEndDrawing(void);
void platformDrawRect(float x, float y, float w, float h, Color color);
// Draws `tex` at (x,y) tinted `color` -- `tex` must be a texture created
// with a white RGB + coverage-alpha format (see glyphs.c), so color mod
// tints it and alpha mod composites it.
void platformDrawTexture(Texture tex, float x, float y, Color color);

// Renders one frame's worth of pixels to a PNG at `path` (used by
// `--screenshot`). Call right before platformEndDrawing() so the just-drawn
// frame is still in the back buffer.
void platformScreenshot(const char *path);

// Non-NULL once platformInit() has run and a GL context is current --
// glyphs.c only touches GL (create/destroy textures) when this is non-NULL,
// so tests (which never call platformInit) stay GL-free.
void *platformGLContext(void);

#endif // TE_PLATFORM_H

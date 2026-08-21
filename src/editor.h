// The surface main.c exposes to src/script.c (the Prolog integration), so
// scripts can run built-in actions and touch the buffer without script.c
// reaching into main.c's static state directly.
#ifndef TE_EDITOR_H
#define TE_EDITOR_H

#include <stddef.h>
#include "binding.h"

void editorRunAction(Action action);   // repeat-count aware, like a typed key
void editorApplyAction(Action action); // runs once, like a leader chord
void editorEcho(const char *msg);
void editorInsertText(const unsigned char *bytes, size_t n);
const unsigned char *editorGetText(size_t *out_len);
size_t editorGetCursor(void);
void editorSetCursor(size_t pos);

// Modal mode (bare keys act like their Ctrl-chord) restricts bare keys to
// pure navigation, same as the old native BINDINGS table's modal check did
// -- script.c consults this so a te_action(Name)-shaped key_binding still
// gets that restriction. A custom (non-te_action) handler has no comparable
// way to self-report "this is just navigation", so script.c blocks those
// outright in modal mode rather than guessing.
bool editorIsNavAction(Action action);
// Mark (Ctrl-Space): while active, movement extends the selection.
bool editorGetMarkActive(void);
// Whether the action script.c is about to run should extend the current
// selection (mark active, or a plain Shift+navigation key) -- script.c
// computes this from the matched binding's Mod and calls it right before
// running a te_action(Name) handler, same computation the old native
// BINDINGS loop did inline.
void editorSetSelExtend(bool extend);

#endif // TE_EDITOR_H

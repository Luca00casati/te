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

#endif // TE_EDITOR_H

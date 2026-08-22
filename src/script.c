// Prolog integration -- see script.h. Registers `te_action/1`, `te_echo/1`,
// `te_insert/1`, `te_text/1`, `te_cursor/1`, `te_set_cursor/1` as native
// predicates, and loads the user's init.pl followed by te's own
// src/default_bindings.pl at startup. Bindings/hooks/commands are ordinary
// facts and rules (key_binding/3, key_binding_once/3, leader_binding/3,
// hook/2, command/2), re-solved fresh every time they're checked rather
// than cached from a one-time registration -- see prolog.h's engine and
// docs/init.pl.example.
//
// te's own .pl files (bootstrap.pl, default_bindings.pl, and every other
// src/*.pl) are read from disk at startup rather than baked into the binary
// -- same as the bundled font, found next to the running executable
// (resolvePlDir), or a plain "src" relative to the working directory as a
// fallback (which is what makes the test binary in build/ -- one directory
// below the real src/ -- work: `make test` always runs with the repo root
// as its working directory). bootstrap.pl missing is fatal (scriptSetup
// fails, scriptInit/scriptInitFromFile return false): it's the engine's own
// standard library, and default_bindings.pl/undo_history.pl/search.pl/
// movement.pl all lean on it (between/3, length/2, reverse/2, nth0/3, ...).
// Everything else in the resolved directory except bootstrap.pl and
// default_bindings.pl (which load on their own schedule -- see scriptInit)
// is consulted as te's "core" library -- add a new one by just adding the
// file, no rebuild or code edit needed.
#define _DEFAULT_SOURCE // readlink
#include "script.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "binding.h"
#include "config.h"
#include "editor.h"
#include "prolog.h"

static Prolog *pl = NULL;

// Set right before solving a matched key_binding/leader_binding/hook/command
// Handler, so te_action/1 knows whether to run the action repeat-count
// aware (a top-level key, like a typed key) or exactly once (a leader
// chord, a lifecycle hook, or a typed command) -- mirrors editorRunAction
// vs editorApplyAction in the old native dispatch.
static bool currentAllowsRepeat = false;

static void reportError(const char *msg, void *ctx) {
    (void)ctx;
    char buf[560];
    snprintf(buf, sizeof buf, "prolog error: %s", msg);
    editorEcho(buf);
}

static const struct { const char *name; int key; } NAMED_KEYS[] = {
    { "space", GLFW_KEY_SPACE }, { "enter", GLFW_KEY_ENTER }, { "kp_enter", GLFW_KEY_KP_ENTER },
    { "tab", GLFW_KEY_TAB }, { "backspace", GLFW_KEY_BACKSPACE }, { "delete", GLFW_KEY_DELETE },
    { "escape", GLFW_KEY_ESCAPE },
};
static const size_t NAMED_KEYS_COUNT = sizeof(NAMED_KEYS) / sizeof(NAMED_KEYS[0]);

// GLFW_KEY_A..Z and GLFW_KEY_0..9 both equal ASCII 'A'..'Z'/'0'..'9', but
// '0' is still handled separately below rather than folded into the '1'..'9'
// arithmetic, matching the SDL scancode ordering this replaced (where 0 sat
// after 9, not before it).
static int keyFromChar(char c) {
    char u = (char)toupper((unsigned char)c);
    if (u >= 'A' && u <= 'Z') return GLFW_KEY_A + (u - 'A');
    if (u == '0') return GLFW_KEY_0;
    if (u >= '1' && u <= '9') return GLFW_KEY_1 + (u - '1');
    return -1;
}

static int parseKeyName(const char *name) {
    if (strlen(name) == 1) return keyFromChar(name[0]);
    for (size_t i = 0; i < NAMED_KEYS_COUNT; i++)
        if (strcmp(name, NAMED_KEYS[i].name) == 0) return NAMED_KEYS[i].key;
    return -1;
}
// "ctrl_shift" (underscore): a bare Prolog atom can't contain a hyphen.
static bool parseModName(const char *name, Mod *out) {
    if (strcmp(name, "any") == 0) { *out = MOD_ANY; return true; }
    if (strcmp(name, "ctrl") == 0) { *out = MOD_CTRL; return true; }
    if (strcmp(name, "ctrl_shift") == 0) { *out = MOD_CTRL_SHIFT; return true; }
    return false;
}
static bool lookupAction(const char *name, Action *out) {
    for (size_t i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) { *out = COMMANDS[i].action; return true; }
    }
    return false;
}
// True if `handler` is shaped te_action(Name) -- Name copied into buf. Used
// both to run modal-mode's nav-only filter and to label bindings for the
// help overlay.
static bool teActionText(Prolog *p, PlTerm *handler, char *buf, size_t bufsz) {
    const char *fn = prologFunctorName(p, handler);
    if (!fn || strcmp(fn, "te_action") != 0 || prologArity(p, handler) != 1) return false;
    const char *txt; size_t len;
    if (!prologGetText(p, prologArg(p, handler, 1), &txt, &len)) return false;
    size_t n = len < bufsz - 1 ? len : bufsz - 1;
    memcpy(buf, txt, n);
    buf[n] = 0;
    return true;
}

// --- te_* native predicates ------------------------------------------------

static bool nTeAction(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *name; size_t len;
    if (!prologGetText(p, args[0], &name, &len)) return false;
    char buf[64]; size_t n = len < 63 ? len : 63;
    memcpy(buf, name, n); buf[n] = 0;
    Action action;
    if (!lookupAction(buf, &action)) prologThrowMsg(p, "te_action: unknown action");
    if (currentAllowsRepeat) editorRunAction(action); else editorApplyAction(action);
    return true;
}
static bool nTeEcho(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *s; size_t len;
    if (!prologGetText(p, args[0], &s, &len)) return false;
    char buf[512]; size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, s, n); buf[n] = 0;
    editorEcho(buf);
    return true;
}
static bool nTeInsert(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *s; size_t len;
    if (!prologGetText(p, args[0], &s, &len)) return false;
    editorInsertText((const unsigned char *)s, len);
    return true;
}
static bool nTeText(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t n;
    const unsigned char *s = editorGetText(&n);
    PlTerm *t = prologMkCodeList(p, (const char *)s, n);
    return prologUnify(p, args[0], t);
}
static bool nTeCursor(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorGetCursor()));
}
static bool nTeAnchor(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorGetAnchor()));
}
static bool nTeSetCursor(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long pos;
    if (!prologGetInt(p, args[0], &pos)) return false;
    if (pos < 0) pos = 0;
    editorSetCursor((size_t)pos);
    return true;
}
static bool nTeReplaceRange(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long start, end;
    if (!prologGetInt(p, args[0], &start) || !prologGetInt(p, args[1], &end)) return false;
    if (start < 0 || end < start) return false;
    const char *bytes; size_t len;
    if (!prologGetText(p, args[2], &bytes, &len)) return false;
    editorReplaceRange((size_t)start, (size_t)end, (const unsigned char *)bytes, len);
    return true;
}
static bool nTeUndoDepth(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorUndoDepth()));
}
static bool isTrueAtom(Prolog *p, PlTerm *t) {
    const char *n = prologFunctorName(p, t);
    return n && strcmp(n, "true") == 0;
}
// Builds [match(S0,E0), match(S1,E1), ...] from editorMatchGet(0..count-1),
// consed from the tail so the result comes out in the same (position) order
// editorMatchGet already returns them in.
static PlTerm *mkMatchList(Prolog *p, size_t count) {
    PlTerm *list = prologMkAtom(p, "[]");
    for (size_t i = count; i > 0; i--) {
        size_t start, end;
        editorMatchGet(i - 1, &start, &end);
        PlTerm *margs[2] = { prologMkInt(p, (long)start), prologMkInt(p, (long)end) };
        PlTerm *matchTerm = prologMkCompound(p, "match", 2, margs);
        PlTerm *cargs[2] = { matchTerm, list };
        list = prologMkCompound(p, ".", 2, cargs);
    }
    return list;
}
static bool nTeFindMatches(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *query; size_t qlen;
    if (!prologGetText(p, args[0], &query, &qlen)) return false;
    bool isRegex = isTrueAtom(p, args[1]);
    PlTerm *result;
    if (!editorFindMatches((const unsigned char *)query, qlen, isRegex)) {
        result = prologMkAtom(p, "bad_regex");
    } else {
        PlTerm *list = mkMatchList(p, editorMatchCount());
        PlTerm *truncated = prologMkAtom(p, editorMatchTruncated() ? "true" : "false");
        PlTerm *rargs[2] = { list, truncated };
        result = prologMkCompound(p, "matches", 2, rargs);
    }
    return prologUnify(p, args[2], result);
}
static bool nTeRegexSubstitute(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *pattern; size_t plen;
    if (!prologGetText(p, args[0], &pattern, &plen)) return false;
    long ms, me;
    if (!prologGetInt(p, args[1], &ms) || !prologGetInt(p, args[2], &me)) return false;
    const char *replacement; size_t rlen;
    if (!prologGetText(p, args[3], &replacement, &rlen)) return false;
    const unsigned char *out; size_t outlen;
    PlTerm *result;
    if (ms < 0 || me < ms ||
        !editorRegexSubstitute((const unsigned char *)pattern, plen, (size_t)ms, (size_t)me,
                                (const unsigned char *)replacement, rlen, &out, &outlen)) {
        result = prologMkAtom(p, "bad_regex");
    } else {
        PlTerm *oargs[1] = { prologMkCodeList(p, (const char *)out, outlen) };
        result = prologMkCompound(p, "ok", 1, oargs);
    }
    return prologUnify(p, args[4], result);
}
static bool nTeSetSelection(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long a, c;
    if (!prologGetInt(p, args[0], &a) || !prologGetInt(p, args[1], &c)) return false;
    if (a < 0 || c < 0) return false;
    editorSetSelection((size_t)a, (size_t)c);
    return true;
}
static bool nTeApplyReplace(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long start, end;
    if (!prologGetInt(p, args[0], &start) || !prologGetInt(p, args[1], &end)) return false;
    if (start < 0 || end < start) return false;
    const char *bytes; size_t len;
    if (!prologGetText(p, args[2], &bytes, &len)) return false;
    editorApplyReplace((size_t)start, (size_t)end, (const unsigned char *)bytes, len);
    return true;
}

// --- movement (src/movement.pl) -------------------------------------------

static bool getPos(Prolog *p, PlTerm *t, size_t *out) {
    long v;
    if (!prologGetInt(p, t, &v) || v < 0) return false;
    *out = (size_t)v;
    return true;
}
static bool nTeLineStart(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorLineStart(pos)));
}
static bool nTeLineEnd(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorLineEnd(pos)));
}
static bool nTeColsIn(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t start, end;
    if (!getPos(p, args[0], &start) || !getPos(p, args[1], &end) || end < start) return false;
    return prologUnify(p, args[2], prologMkInt(p, (long)editorColsIn(start, end)));
}
static bool nTeByteAtCol(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t start, stop, col;
    if (!getPos(p, args[0], &start) || !getPos(p, args[1], &stop) || !getPos(p, args[2], &col) || stop < start) return false;
    return prologUnify(p, args[3], prologMkInt(p, (long)editorByteAtCol(start, stop, col)));
}
static bool nTeVisRows(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t lineCols, cols;
    if (!getPos(p, args[0], &lineCols) || !getPos(p, args[1], &cols) || cols == 0) return false;
    return prologUnify(p, args[2], prologMkInt(p, (long)editorVisRows(lineCols, cols)));
}
static bool nTeStepLeft(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorStepLeft(pos)));
}
static bool nTeStepRight(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorStepRight(pos)));
}
static bool nTeWordStartLeft(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorWordStartLeft(pos)));
}
static bool nTeWordStartRight(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorWordStartRight(pos)));
}
static bool nTeWordEndRight(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorWordEndRight(pos)));
}
static bool nTeViewCols(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorViewCols()));
}
static bool nTePageLines(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorPageLines()));
}
static bool nTeBufferLen(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorBufferLen()));
}

// --- editing (src/editing.pl) ----------------------------------------------

static bool nTeTab(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkCodeList(p, CFG_TAB, strlen(CFG_TAB)));
}
// The byte at `pos`, 0-255 -- fails if pos is out of range, so callers like
// move_line_left can write a single `te_byte_at(Ls, B), ... -> ...` guard
// instead of a separate bounds check.
static bool nTeByteAt(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t pos;
    if (!getPos(p, args[0], &pos)) return false;
    size_t n;
    const unsigned char *s = editorGetText(&n);
    if (pos >= n) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)s[pos]));
}
// text[start,end) as a code list -- for swap_line's line-sized reads. Unlike
// te_copy_range, this materializes the bytes as a Prolog list, so it's only
// used where the range is bounded by a single line, never a whole selection.
static bool nTeBufferRange(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t start, end;
    if (!getPos(p, args[0], &start) || !getPos(p, args[1], &end) || end < start) return false;
    size_t n;
    const unsigned char *s = editorGetText(&n);
    if (end > n) return false;
    return prologUnify(p, args[2], prologMkCodeList(p, (const char *)s + start, end - start));
}
// Current clipboard content as a code list ('[]' if empty).
static bool nTeClipboardGet(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *s = platformGetClipboardText();
    return prologUnify(p, args[0], prologMkCodeList(p, s, strlen(s)));
}
static bool nTeCopyRange(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t start, end;
    if (!getPos(p, args[0], &start) || !getPos(p, args[1], &end)) return false;
    editorCopyRange(start, end);
    return true;
}

// --- buffers (src/buffers.pl) ----------------------------------------------
// Buffer create/list/switch/kill land as native predicates in a later
// commit; te_current_buffer/1 comes first since src/buffers.pl's
// blocal_get/2 and blocal_set/2 (and the reparameterized undo/search
// singleton facts built on top of them -- src/movement.pl's goal-column
// tracking deliberately stays untouched, see buffers.pl's own comment on
// why) need a buffer id to key on even while there's still only ever the
// one bootstrap buffer.
static bool nTeCurrentBuffer(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorCurrentBufferId()));
}
static bool nTeBufferCreate(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorBufferCreate()));
}
// Copies a Prolog text argument into a NUL-terminated C buffer -- shared by
// every native below that takes a path, since prologGetText's result isn't
// guaranteed NUL-terminated for the code-list case (matches nTeAction's own
// pattern for its action-name argument).
static bool textToCStr(Prolog *p, PlTerm *t, char *out, size_t outCap) {
    const char *s; size_t slen;
    if (!prologGetText(p, t, &s, &slen)) return false;
    size_t n = slen < outCap - 1 ? slen : outCap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}
static bool nTeBufferOpenFile(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    char path[4096];
    if (!textToCStr(p, args[0], path, sizeof path)) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)editorBufferOpenFile(path)));
}
static bool nTeBufferFindByPath(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    char path[4096];
    if (!textToCStr(p, args[0], path, sizeof path)) return false;
    int id = editorBufferFindByPath(path);
    if (id < 0) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)id));
}
static bool nTeBufferKill(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    return editorBufferKill((int)id);
}
static bool nTeBufferList(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t count = editorBufferCount();
    PlTerm *list = prologMkAtom(p, "[]");
    for (size_t i = count; i > 0; i--) {
        PlTerm *cargs[2] = { prologMkInt(p, (long)editorBufferIdAt(i - 1)), list };
        list = prologMkCompound(p, ".", 2, cargs);
    }
    return prologUnify(p, args[0], list);
}
static bool nTeBufferName(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    const char *name = editorBufferName((int)id);
    if (!name) return false;
    return prologUnify(p, args[1], prologMkCodeList(p, name, strlen(name)));
}
static bool nTeBufferFilename(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    const char *path; bool hasFile;
    if (!editorBufferFilename((int)id, &path, &hasFile)) return false;
    PlTerm *result = hasFile ? prologMkCodeList(p, path, strlen(path)) : prologMkAtom(p, "false");
    return prologUnify(p, args[1], result);
}
static bool nTeBufferDirty(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    bool dirty;
    if (!editorBufferDirty((int)id, &dirty)) return false;
    return prologUnify(p, args[1], prologMkAtom(p, dirty ? "true" : "false"));
}
static bool nTeBufferSave(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    return editorBufferSave((int)id);
}

// --- windows (src/windows.pl) -----------------------------------------------
static bool nTeSelectedWindow(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    return prologUnify(p, args[0], prologMkInt(p, (long)editorSelectedWindowId()));
}
static bool nTeSelectWindow(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    return editorSelectWindow((int)id);
}
static bool nTeWindowBuffer(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long winId;
    if (!prologGetInt(p, args[0], &winId)) return false;
    int bufId = editorWindowBufferId((int)winId);
    if (bufId < 0) return false;
    return prologUnify(p, args[1], prologMkInt(p, (long)bufId));
}
static bool nTeWindowSetBuffer(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long winId, bufId;
    if (!prologGetInt(p, args[0], &winId) || !prologGetInt(p, args[1], &bufId)) return false;
    return editorWindowSetBuffer((int)winId, (int)bufId);
}
// Dir is the atom `right` or `below`; anything else fails rather than
// silently defaulting, so a typo in src/windows.pl surfaces immediately.
static bool nTeWindowSplit(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long winId;
    if (!prologGetInt(p, args[0], &winId)) return false;
    const char *dir = prologFunctorName(p, args[1]);
    bool below;
    if (dir && strcmp(dir, "below") == 0) below = true;
    else if (dir && strcmp(dir, "right") == 0) below = false;
    else return false;
    int newId = editorWindowSplit((int)winId, below);
    if (newId < 0) return false;
    return prologUnify(p, args[2], prologMkInt(p, (long)newId));
}
static bool nTeWindowClose(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    return editorWindowClose((int)id);
}
static bool nTeWindowList(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t count = editorWindowCount();
    PlTerm *list = prologMkAtom(p, "[]");
    for (size_t i = count; i > 0; i--) {
        PlTerm *cargs[2] = { prologMkInt(p, (long)editorWindowIdAt(i - 1)), list };
        list = prologMkCompound(p, ".", 2, cargs);
    }
    return prologUnify(p, args[0], list);
}
static bool nTeWindowDeleteOthers(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long id;
    if (!prologGetInt(p, args[0], &id)) return false;
    return editorWindowDeleteOthers((int)id);
}

// --- lifecycle --------------------------------------------------------

static char plDir[4096];

// Finds the directory holding te's own .pl source files: <exe_dir>/src if
// that exists (matches how loadFontFile in main.c finds the bundled font
// next to the executable -- works regardless of the caller's working
// directory), else a plain "src" relative to the current directory (the
// test binary lives in build/, one level below the real src/, but `make
// test` always runs with the repo root as its working directory, where a
// relative "src" resolves correctly).
static bool resolvePlDir(char *out, size_t outsz) {
    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n > 0) {
        exePath[n] = 0;
        char *slash = strrchr(exePath, '/');
        if (slash) {
            char candidate[4160];
            snprintf(candidate, sizeof candidate, "%.*s/src", (int)(slash - exePath), exePath);
            struct stat st;
            if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(out, outsz, "%s", candidate);
                return true;
            }
        }
    }
    struct stat st;
    if (stat("src", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, outsz, "src");
        return true;
    }
    return false;
}

// Consults every *.pl file in `dir` except bootstrap.pl and
// default_bindings.pl (which load separately, on their own schedule -- see
// scriptInit). A missing/unreadable file here degrades gracefully same as
// a missing init.pl (whatever it defined just won't be there -- errors
// from code that depended on it are echoed, not fatal); only bootstrap.pl
// itself (the caller's job, before this runs) is treated as mandatory.
static void consultCoreLibrary(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 4 || strcmp(name + len - 3, ".pl") != 0) continue;
        if (strcmp(name, "bootstrap.pl") == 0 || strcmp(name, "default_bindings.pl") == 0) continue;
        char path[4160];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        prologConsultFile(pl, path);
    }
    closedir(d);
}

static bool scriptSetup(void) {
    pl = prologCreate();
    prologSetErrorHandler(pl, reportError, NULL);
    prologRegisterNative(pl, "te_action", 1, nTeAction, NULL);
    prologRegisterNative(pl, "te_echo", 1, nTeEcho, NULL);
    prologRegisterNative(pl, "te_insert", 1, nTeInsert, NULL);
    prologRegisterNative(pl, "te_text", 1, nTeText, NULL);
    prologRegisterNative(pl, "te_cursor", 1, nTeCursor, NULL);
    prologRegisterNative(pl, "te_anchor", 1, nTeAnchor, NULL);
    prologRegisterNative(pl, "te_set_cursor", 1, nTeSetCursor, NULL);
    prologRegisterNative(pl, "te_replace_range", 3, nTeReplaceRange, NULL);
    prologRegisterNative(pl, "te_undo_depth", 1, nTeUndoDepth, NULL);
    prologRegisterNative(pl, "te_find_matches", 3, nTeFindMatches, NULL);
    prologRegisterNative(pl, "te_regex_substitute", 5, nTeRegexSubstitute, NULL);
    prologRegisterNative(pl, "te_set_selection", 2, nTeSetSelection, NULL);
    prologRegisterNative(pl, "te_apply_replace", 3, nTeApplyReplace, NULL);
    prologRegisterNative(pl, "te_line_start", 2, nTeLineStart, NULL);
    prologRegisterNative(pl, "te_line_end", 2, nTeLineEnd, NULL);
    prologRegisterNative(pl, "te_cols_in", 3, nTeColsIn, NULL);
    prologRegisterNative(pl, "te_byte_at_col", 4, nTeByteAtCol, NULL);
    prologRegisterNative(pl, "te_vis_rows", 3, nTeVisRows, NULL);
    prologRegisterNative(pl, "te_step_left", 2, nTeStepLeft, NULL);
    prologRegisterNative(pl, "te_step_right", 2, nTeStepRight, NULL);
    prologRegisterNative(pl, "te_word_start_left", 2, nTeWordStartLeft, NULL);
    prologRegisterNative(pl, "te_word_start_right", 2, nTeWordStartRight, NULL);
    prologRegisterNative(pl, "te_word_end_right", 2, nTeWordEndRight, NULL);
    prologRegisterNative(pl, "te_view_cols", 1, nTeViewCols, NULL);
    prologRegisterNative(pl, "te_page_lines", 1, nTePageLines, NULL);
    prologRegisterNative(pl, "te_buffer_len", 1, nTeBufferLen, NULL);
    prologRegisterNative(pl, "te_tab", 1, nTeTab, NULL);
    prologRegisterNative(pl, "te_byte_at", 2, nTeByteAt, NULL);
    prologRegisterNative(pl, "te_buffer_range", 3, nTeBufferRange, NULL);
    prologRegisterNative(pl, "te_clipboard_get", 1, nTeClipboardGet, NULL);
    prologRegisterNative(pl, "te_copy_range", 2, nTeCopyRange, NULL);
    prologRegisterNative(pl, "te_current_buffer", 1, nTeCurrentBuffer, NULL);
    prologRegisterNative(pl, "te_buffer_create", 1, nTeBufferCreate, NULL);
    prologRegisterNative(pl, "te_buffer_open_file", 2, nTeBufferOpenFile, NULL);
    prologRegisterNative(pl, "te_buffer_find_by_path", 2, nTeBufferFindByPath, NULL);
    prologRegisterNative(pl, "te_buffer_kill", 1, nTeBufferKill, NULL);
    prologRegisterNative(pl, "te_buffer_list", 1, nTeBufferList, NULL);
    prologRegisterNative(pl, "te_buffer_name", 2, nTeBufferName, NULL);
    prologRegisterNative(pl, "te_buffer_filename", 2, nTeBufferFilename, NULL);
    prologRegisterNative(pl, "te_buffer_dirty", 2, nTeBufferDirty, NULL);
    prologRegisterNative(pl, "te_buffer_save", 1, nTeBufferSave, NULL);
    prologRegisterNative(pl, "te_selected_window", 1, nTeSelectedWindow, NULL);
    prologRegisterNative(pl, "te_select_window", 1, nTeSelectWindow, NULL);
    prologRegisterNative(pl, "te_window_buffer", 2, nTeWindowBuffer, NULL);
    prologRegisterNative(pl, "te_window_set_buffer", 2, nTeWindowSetBuffer, NULL);
    prologRegisterNative(pl, "te_window_split", 3, nTeWindowSplit, NULL);
    prologRegisterNative(pl, "te_window_close", 1, nTeWindowClose, NULL);
    prologRegisterNative(pl, "te_window_list", 1, nTeWindowList, NULL);
    prologRegisterNative(pl, "te_window_delete_others", 1, nTeWindowDeleteOthers, NULL);

    if (!resolvePlDir(plDir, sizeof plDir)) return false;
    char bootstrapPath[4160];
    snprintf(bootstrapPath, sizeof bootstrapPath, "%s/bootstrap.pl", plDir);
    struct stat st;
    if (stat(bootstrapPath, &st) != 0) return false; // the engine's own stdlib: mandatory
    prologConsultFile(pl, bootstrapPath);
    consultCoreLibrary(plDir);
    return true;
}
bool scriptInit(void) {
    if (!scriptSetup()) return false;
    char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(path, sizeof path, "%s/te/init.pl", xdg);
    else if (home && home[0]) snprintf(path, sizeof path, "%s/.config/te/init.pl", home);
    else path[0] = 0;
    if (path[0]) prologConsultFile(pl, path);
    // Loaded *after* the user's init.pl, so a user key_binding/leader_binding/
    // command fact at the same key/mod/name is tried first (see script.h).
    char dbPath[4160];
    snprintf(dbPath, sizeof dbPath, "%s/default_bindings.pl", plDir);
    prologConsultFile(pl, dbPath);
    return true;
}
bool scriptInitFromFile(const char *path) {
    if (!scriptSetup()) return false;
    prologConsultFile(pl, path);
    char dbPath[4160];
    snprintf(dbPath, sizeof dbPath, "%s/default_bindings.pl", plDir);
    prologConsultFile(pl, dbPath);
    return true;
}
void scriptShutdown(void) {
    if (pl) { prologDestroy(pl); pl = NULL; }
}

// --- key/leader/command dispatch -----------------------------------------
// key_binding/3, key_binding_once/3, leader_binding/3, and command/2 are
// all solved fresh every call (via findall) rather than cached, so a
// Handler rule can consult live editor state (te_text/1 etc.) -- see
// prolog.h's design note on this.

static bool matchAndRun(const char *predName, bool (*modMatches)(Mod, bool, bool),
                         bool cmd, bool shift, bool repeatable, bool isTopLevel,
                         bool blockNonNav, bool (*keyPressed)(int key)) {
    if (!pl) return false;
    size_t mark = prologMark(pl);
    char goalSrc[128];
    snprintf(goalSrc, sizeof goalSrc, "findall(b(K,M,H), %s(K,M,H), L)", predName);
    PlTerm *g = prologParseTerm(pl, goalSrc);
    bool matched = false;
    if (g && prologSolve(pl, g)) {
        PlTerm *list = prologArg(pl, g, 3);
        while (list && prologIsList(pl, list)) {
            PlTerm *item, *tail;
            prologGetListHeadTail(pl, list, &item, &tail);
            list = tail;
            const char *kName, *mName; size_t kLen, mLen;
            if (!prologGetText(pl, prologArg(pl, item, 1), &kName, &kLen)) continue;
            if (!prologGetText(pl, prologArg(pl, item, 2), &mName, &mLen)) continue;
            char kBuf[32], mBuf[32];
            size_t kn = kLen < 31 ? kLen : 31, mn = mLen < 31 ? mLen : 31;
            memcpy(kBuf, kName, kn); kBuf[kn] = 0;
            memcpy(mBuf, mName, mn); mBuf[mn] = 0;
            int key = parseKeyName(kBuf);
            Mod mod;
            if (key < 0 || !parseModName(mBuf, &mod)) continue;
            if (!modMatches(mod, cmd, shift)) continue;
            if (!keyPressed(key)) continue;
            PlTerm *handler = prologArg(pl, item, 3);
            char actionName[64];
            bool isTeAction = teActionText(pl, handler, actionName, sizeof actionName);
            if (blockNonNav) {
                Action act;
                bool navOk = isTeAction && lookupAction(actionName, &act) && editorIsNavAction(act);
                if (!navOk) continue;
            }
            if (isTopLevel) editorSetSelExtend(editorGetMarkActive() || (shift && mod == MOD_ANY));
            currentAllowsRepeat = repeatable;
            prologSolve(pl, handler);
            matched = true;
            break;
        }
    }
    prologReset(pl, mark);
    return matched;
}
static bool repeatKeyPressed(int key) { return platformKeyPressed(key) || platformKeyPressedRepeat(key); }
static bool plainKeyPressed(int key) { return platformKeyPressed(key); }
// Adapts modMatchesChord's (Mod,bool shift) signature to matchAndRun's
// (Mod,bool cmd,bool shift) callback shape -- Ctrl is irrelevant/optional
// for a chord, only shift narrows it (see binding.h).
static bool modMatchesChordAdapter(Mod mod, bool cmd, bool shift) { (void)cmd; return modMatchesChord(mod, shift); }

bool scriptHandleKey(bool ctrl, bool shift, bool modal) {
    bool cmd = ctrl || modal;
    bool blockNonNav = modal && !ctrl;
    if (matchAndRun("key_binding", modMatchesKey, cmd, shift, /*repeatable=*/true,
                     /*isTopLevel=*/true, blockNonNav, repeatKeyPressed))
        return true;
    return matchAndRun("key_binding_once", modMatchesKey, cmd, shift, /*repeatable=*/false,
                        /*isTopLevel=*/true, blockNonNav, plainKeyPressed);
}
bool scriptHandlePrefixKey(bool shift) {
    return matchAndRun("leader_binding", modMatchesChordAdapter, /*cmd=*/false, shift,
                        /*repeatable=*/false, /*isTopLevel=*/false, /*blockNonNav=*/false, plainKeyPressed);
}

void scriptRunHook(const char *name) {
    if (!pl) return;
    char atomName[64]; size_t n = 0;
    for (const char *s = name; *s && n < sizeof atomName - 1; s++) atomName[n++] = (*s == '-') ? '_' : *s;
    atomName[n] = 0;

    size_t mark = prologMark(pl);
    char goalSrc[128];
    snprintf(goalSrc, sizeof goalSrc, "findall(H, hook(%s, H), L)", atomName);
    PlTerm *g = prologParseTerm(pl, goalSrc);
    if (g && prologSolve(pl, g)) {
        PlTerm *list = prologArg(pl, g, 3);
        while (list && prologIsList(pl, list)) {
            PlTerm *handler, *tail;
            prologGetListHeadTail(pl, list, &handler, &tail);
            list = tail;
            currentAllowsRepeat = false;
            prologSolve(pl, handler);
        }
    }
    prologReset(pl, mark);
}

bool scriptRunCommand(const char *name) {
    if (!pl) return false;
    size_t mark = prologMark(pl);
    PlTerm *nameAtom = prologMkAtom(pl, name);
    PlTerm *handlerVar = prologMkVar(pl);
    PlTerm *cargs[2] = { nameAtom, handlerVar };
    PlTerm *goal = prologMkCompound(pl, "command", 2, cargs);
    bool found = prologSolve(pl, goal);
    if (found) {
        currentAllowsRepeat = false;
        prologSolve(pl, handlerVar);
    }
    prologReset(pl, mark);
    return found;
}

// --- introspection for main.c's help overlay / command completion --------

typedef struct { int key; Mod mod; char label[64]; } BindingInfo;
static BindingInfo *topBindingCache = NULL;
static size_t topBindingCacheCount = 0, topBindingCacheCap = 0;
static BindingInfo *leaderBindingCache = NULL;
static size_t leaderBindingCacheCount = 0, leaderBindingCacheCap = 0;
typedef struct { char name[64]; } CommandInfo;
static CommandInfo *commandCache = NULL;
static size_t commandCacheCount = 0, commandCacheCap = 0;

static void collectBindingsFrom(const char *predName, BindingInfo **cache, size_t *count, size_t *cap) {
    size_t mark = prologMark(pl);
    char goalSrc[128];
    snprintf(goalSrc, sizeof goalSrc, "findall(b(K,M,H), %s(K,M,H), L)", predName);
    PlTerm *g = prologParseTerm(pl, goalSrc);
    if (g && prologSolve(pl, g)) {
        PlTerm *list = prologArg(pl, g, 3);
        while (list && prologIsList(pl, list)) {
            PlTerm *item, *tail;
            prologGetListHeadTail(pl, list, &item, &tail);
            list = tail;
            const char *kName, *mName; size_t kLen, mLen;
            if (!prologGetText(pl, prologArg(pl, item, 1), &kName, &kLen)) continue;
            if (!prologGetText(pl, prologArg(pl, item, 2), &mName, &mLen)) continue;
            char kBuf[32], mBuf[32];
            size_t kn = kLen < 31 ? kLen : 31, mn = mLen < 31 ? mLen : 31;
            memcpy(kBuf, kName, kn); kBuf[kn] = 0;
            memcpy(mBuf, mName, mn); mBuf[mn] = 0;
            int key = parseKeyName(kBuf);
            Mod mod;
            if (key < 0 || !parseModName(mBuf, &mod)) continue;
            PlTerm *handler = prologArg(pl, item, 3);
            char label[64];
            if (!teActionText(pl, handler, label, sizeof label)) {
                const char *fn = prologFunctorName(pl, handler);
                snprintf(label, sizeof label, "%s", fn ? fn : "?");
            }
            if (*count == *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *cache = realloc(*cache, *cap * sizeof(BindingInfo));
            }
            (*cache)[*count].key = key;
            (*cache)[*count].mod = mod;
            snprintf((*cache)[*count].label, sizeof (*cache)[*count].label, "%s", label);
            (*count)++;
        }
    }
    prologReset(pl, mark);
}
size_t scriptTopBindingCount(void) {
    topBindingCacheCount = 0;
    if (!pl) return 0;
    collectBindingsFrom("key_binding", &topBindingCache, &topBindingCacheCount, &topBindingCacheCap);
    collectBindingsFrom("key_binding_once", &topBindingCache, &topBindingCacheCount, &topBindingCacheCap);
    return topBindingCacheCount;
}
bool scriptTopBindingGet(size_t i, int *key, Mod *mod, const char **label) {
    if (i >= topBindingCacheCount) return false;
    *key = topBindingCache[i].key; *mod = topBindingCache[i].mod; *label = topBindingCache[i].label;
    return true;
}
size_t scriptLeaderBindingCount(void) {
    leaderBindingCacheCount = 0;
    if (!pl) return 0;
    collectBindingsFrom("leader_binding", &leaderBindingCache, &leaderBindingCacheCount, &leaderBindingCacheCap);
    return leaderBindingCacheCount;
}
bool scriptLeaderBindingGet(size_t i, int *key, Mod *mod, const char **label) {
    if (i >= leaderBindingCacheCount) return false;
    *key = leaderBindingCache[i].key; *mod = leaderBindingCache[i].mod; *label = leaderBindingCache[i].label;
    return true;
}
size_t scriptCommandCount(void) {
    commandCacheCount = 0;
    if (!pl) return 0;
    size_t mark = prologMark(pl);
    PlTerm *g = prologParseTerm(pl, "findall(N, command(N,_), L)");
    if (g && prologSolve(pl, g)) {
        PlTerm *list = prologArg(pl, g, 2);
        while (list && prologIsList(pl, list)) {
            PlTerm *item, *tail;
            prologGetListHeadTail(pl, list, &item, &tail);
            list = tail;
            const char *txt; size_t len;
            if (!prologGetText(pl, item, &txt, &len)) continue;
            if (commandCacheCount == commandCacheCap) {
                commandCacheCap = commandCacheCap ? commandCacheCap * 2 : 32;
                commandCache = realloc(commandCache, commandCacheCap * sizeof(CommandInfo));
            }
            size_t n = len < sizeof commandCache[0].name - 1 ? len : sizeof commandCache[0].name - 1;
            memcpy(commandCache[commandCacheCount].name, txt, n);
            commandCache[commandCacheCount].name[n] = 0;
            commandCacheCount++;
        }
    }
    prologReset(pl, mark);
    return commandCacheCount;
}
bool scriptCommandGet(size_t i, const char **name) {
    if (i >= commandCacheCount) return false;
    *name = commandCache[i].name;
    return true;
}

// --- undo/redo history (src/undo_history.pl) ------------------------------
// removed/inserted are arbitrary buffer bytes, so these are built via
// prologMkCompound/prologMkInt/prologMkCodeList rather than a parsed goal
// string -- same injection-safety reasoning as scriptRunCommand.

void scriptRecordEdit(size_t pos, const unsigned char *removed, size_t removed_len,
                      const unsigned char *inserted, size_t inserted_len,
                      size_t cur_before, size_t cur_after) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[5] = {
        prologMkInt(pl, (long)pos),
        prologMkCodeList(pl, (const char *)removed, removed_len),
        prologMkCodeList(pl, (const char *)inserted, inserted_len),
        prologMkInt(pl, (long)cur_before),
        prologMkInt(pl, (long)cur_after),
    };
    PlTerm *goal = prologMkCompound(pl, "record_edit", 5, cargs);
    prologSolve(pl, goal);
    prologReset(pl, mark);
}
bool scriptUndo(void) {
    if (!pl) return false;
    size_t mark = prologMark(pl);
    bool ok = prologSolve(pl, prologMkAtom(pl, "undo_step"));
    prologReset(pl, mark);
    return ok;
}
bool scriptRedo(void) {
    if (!pl) return false;
    size_t mark = prologMark(pl);
    bool ok = prologSolve(pl, prologMkAtom(pl, "redo_step"));
    prologReset(pl, mark);
    return ok;
}
bool scriptUndoStackEmpty(void) {
    if (!pl) return true;
    size_t mark = prologMark(pl);
    bool empty = prologSolve(pl, prologMkAtom(pl, "undo_stack_empty"));
    prologReset(pl, mark);
    return empty;
}
void scriptClearHistory(void) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    prologSolve(pl, prologMkAtom(pl, "clear_history"));
    prologReset(pl, mark);
}

// --- search/replace (src/search.pl) ---------------------------------------

static PlTerm *mkBool(Prolog *p, bool b) { return prologMkAtom(p, b ? "true" : "false"); }

void scriptStartSearch(bool is_regex, bool reverse, size_t origin) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[3] = { mkBool(pl, is_regex), mkBool(pl, reverse), prologMkInt(pl, (long)origin) };
    prologSolve(pl, prologMkCompound(pl, "start_search", 3, cargs));
    prologReset(pl, mark);
}
void scriptStartReplace(bool is_regex, bool all_mode, size_t origin) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[3] = { mkBool(pl, is_regex), mkBool(pl, all_mode), prologMkInt(pl, (long)origin) };
    prologSolve(pl, prologMkCompound(pl, "start_replace", 3, cargs));
    prologReset(pl, mark);
}
size_t scriptSearchOrigin(void) {
    if (!pl) return 0;
    size_t mark = prologMark(pl);
    PlTerm *originVar = prologMkVar(pl);
    PlTerm *cargs[1] = { originVar };
    size_t origin = 0;
    if (prologSolve(pl, prologMkCompound(pl, "search_origin", 1, cargs))) {
        long v;
        if (prologGetInt(pl, originVar, &v) && v >= 0) origin = (size_t)v;
    }
    prologReset(pl, mark);
    return origin;
}
bool scriptSearchReverse(void) {
    if (!pl) return false;
    size_t mark = prologMark(pl);
    PlTerm *revVar = prologMkVar(pl);
    PlTerm *cargs[1] = { revVar };
    bool reverse = false;
    if (prologSolve(pl, prologMkCompound(pl, "search_reverse", 1, cargs))) {
        const char *fn = prologFunctorName(pl, revVar);
        reverse = fn && strcmp(fn, "true") == 0;
    }
    prologReset(pl, mark);
    return reverse;
}
void scriptSearchUpdate(const unsigned char *query, size_t len) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[1] = { prologMkCodeList(pl, (const char *)query, len) };
    prologSolve(pl, prologMkCompound(pl, "search_update", 1, cargs));
    prologReset(pl, mark);
}
void scriptSearchStep(const unsigned char *query, size_t len, bool forward) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[2] = { prologMkCodeList(pl, (const char *)query, len), mkBool(pl, forward) };
    prologSolve(pl, prologMkCompound(pl, "search_step", 2, cargs));
    prologReset(pl, mark);
}
bool scriptEnterReplaceQuery(const unsigned char *pattern, size_t pattern_len,
                             const unsigned char *replacement, size_t replacement_len,
                             bool is_regex) {
    if (!pl) return false;
    size_t mark = prologMark(pl);
    PlTerm *cargs[3] = {
        prologMkCodeList(pl, (const char *)pattern, pattern_len),
        prologMkCodeList(pl, (const char *)replacement, replacement_len),
        mkBool(pl, is_regex),
    };
    bool ok = prologSolve(pl, prologMkCompound(pl, "enter_replace_query", 3, cargs));
    prologReset(pl, mark);
    return ok;
}
void scriptReplaceStep(bool forward) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[1] = { mkBool(pl, forward) };
    prologSolve(pl, prologMkCompound(pl, "replace_step", 1, cargs));
    prologReset(pl, mark);
}
ReplaceStepResult scriptReplaceCurrentMatch(void) {
    if (!pl) return REPLACE_STEP_DONE;
    size_t mark = prologMark(pl);
    PlTerm *resultVar = prologMkVar(pl);
    PlTerm *cargs[1] = { resultVar };
    ReplaceStepResult result = REPLACE_STEP_DONE;
    if (prologSolve(pl, prologMkCompound(pl, "replace_current_match", 1, cargs))) {
        const char *fn = prologFunctorName(pl, resultVar);
        if (fn && strcmp(fn, "ok") == 0) result = REPLACE_STEP_OK;
        else if (fn && strcmp(fn, "failed") == 0) result = REPLACE_STEP_FAILED;
    }
    prologReset(pl, mark);
    return result;
}
size_t scriptReplaceAll(const unsigned char *pattern, size_t pattern_len,
                        const unsigned char *replacement, size_t replacement_len,
                        bool is_regex) {
    if (!pl) return 0;
    size_t mark = prologMark(pl);
    PlTerm *countVar = prologMkVar(pl);
    PlTerm *cargs[4] = {
        prologMkCodeList(pl, (const char *)pattern, pattern_len),
        prologMkCodeList(pl, (const char *)replacement, replacement_len),
        mkBool(pl, is_regex),
        countVar,
    };
    size_t count = 0;
    if (prologSolve(pl, prologMkCompound(pl, "replace_all", 4, cargs))) {
        long v;
        if (prologGetInt(pl, countVar, &v) && v >= 0) count = (size_t)v;
    }
    prologReset(pl, mark);
    return count;
}
void scriptSearchStatus(size_t *index, size_t *count, bool *truncated, bool *bad_regex) {
    *index = 0; *count = 0; *truncated = false; *bad_regex = false;
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *idxVar = prologMkVar(pl);
    PlTerm *countVar = prologMkVar(pl);
    PlTerm *truncVar = prologMkVar(pl);
    PlTerm *badVar = prologMkVar(pl);
    PlTerm *cargs[4] = { idxVar, countVar, truncVar, badVar };
    if (prologSolve(pl, prologMkCompound(pl, "search_status", 4, cargs))) {
        long v;
        if (prologGetInt(pl, idxVar, &v) && v >= 0) *index = (size_t)v;
        if (prologGetInt(pl, countVar, &v) && v >= 0) *count = (size_t)v;
        const char *tn = prologFunctorName(pl, truncVar);
        *truncated = tn && strcmp(tn, "true") == 0;
        const char *bn = prologFunctorName(pl, badVar);
        *bad_regex = bn && strcmp(bn, "true") == 0;
    }
    prologReset(pl, mark);
}
void scriptClearSearch(void) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    prologSolve(pl, prologMkAtom(pl, "clear_search"));
    prologReset(pl, mark);
}

// --- cursor movement (src/movement.pl) ------------------------------------

static void solveAtom(const char *name) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    prologSolve(pl, prologMkAtom(pl, name));
    prologReset(pl, mark);
}
void scriptClearGoalColumn(void) { solveAtom("clear_goal_col"); }
void scriptMoveLeft(void) { solveAtom("move_left"); }
void scriptMoveRight(void) { solveAtom("move_right"); }
void scriptMoveWordStartLeft(void) { solveAtom("move_word_start_left"); }
void scriptMoveWordStartRight(void) { solveAtom("move_word_start_right"); }
void scriptMoveWordEndRight(void) { solveAtom("move_word_end_right"); }
void scriptMoveHome(void) { solveAtom("move_home"); }
void scriptMoveEnd(void) { solveAtom("move_end"); }
void scriptMoveBufferStart(void) { solveAtom("move_buffer_start"); }
void scriptMoveBufferEnd(void) { solveAtom("move_buffer_end"); }
void scriptSelectAll(void) { solveAtom("select_all"); }
void scriptMoveVertical(int delta, bool wrap, size_t cols) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[3] = { prologMkInt(pl, (long)delta), mkBool(pl, wrap), prologMkInt(pl, (long)cols) };
    prologSolve(pl, prologMkCompound(pl, "move_vertical", 3, cargs));
    prologReset(pl, mark);
}
void scriptPageUp(bool wrap, size_t cols, size_t lines) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[3] = { mkBool(pl, wrap), prologMkInt(pl, (long)cols), prologMkInt(pl, (long)lines) };
    prologSolve(pl, prologMkCompound(pl, "page_up", 3, cargs));
    prologReset(pl, mark);
}
void scriptPageDown(bool wrap, size_t cols, size_t lines) {
    if (!pl) return;
    size_t mark = prologMark(pl);
    PlTerm *cargs[3] = { mkBool(pl, wrap), prologMkInt(pl, (long)cols), prologMkInt(pl, (long)lines) };
    prologSolve(pl, prologMkCompound(pl, "page_down", 3, cargs));
    prologReset(pl, mark);
}

// --- editing (src/editing.pl) -----------------------------------------------
void scriptNewline(void) { solveAtom("newline"); }
void scriptOpenLineBelow(void) { solveAtom("open_line_below"); }
void scriptOpenLineAbove(void) { solveAtom("open_line_above"); }
void scriptIndent(void) { solveAtom("indent"); }
void scriptDeleteBack(void) { solveAtom("delete_back"); }
void scriptDeleteForward(void) { solveAtom("delete_forward"); }
void scriptCopy(void) { solveAtom("copy_selection"); }
void scriptCut(void) { solveAtom("cut_selection"); }
void scriptPaste(void) { solveAtom("paste_clipboard"); }
void scriptMoveLineLeft(void) { solveAtom("move_line_left"); }
void scriptMoveLineRight(void) { solveAtom("move_line_right"); }
void scriptMoveLineUp(void) { solveAtom("move_line_up"); }
void scriptMoveLineDown(void) { solveAtom("move_line_down"); }
void scriptCutLine(void) { solveAtom("cut_line"); }
void scriptCopyLine(void) { solveAtom("copy_line"); }
void scriptPasteLine(void) { solveAtom("paste_line"); }
void scriptSelectLine(void) { solveAtom("select_line"); }

// Prolog integration -- see script.h. Registers `te_action/1`, `te_echo/1`,
// `te_insert/1`, `te_text/1`, `te_cursor/1`, `te_set_cursor/1` as native
// predicates, and loads an optional user init.pl at startup. Bindings/hooks
// are ordinary facts and rules (key_binding/3, leader_binding/3, hook/2),
// re-solved fresh every time they're checked rather than cached from a
// one-time registration -- see prolog.h's engine and docs/init.pl.example.
#include "script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "binding.h"
#include "editor.h"
#include "prolog.h"

static Prolog *pl = NULL;

// Set right before solving a matched key_binding/leader_binding/hook
// Handler, so te_action/1 knows whether to run the action repeat-count
// aware (a top-level key, like a typed key) or exactly once (a leader
// chord or a lifecycle hook) -- mirrors editorRunAction vs editorApplyAction
// in the old Lua integration.
static bool currentAllowsRepeat = false;

static void reportError(const char *msg, void *ctx) {
    (void)ctx;
    char buf[560];
    snprintf(buf, sizeof buf, "prolog error: %s", msg);
    editorEcho(buf);
}

// Single-char names map directly: raylib's KEY_A..KEY_Z and KEY_ZERO..
// KEY_NINE constants equal the ASCII codes of the uppercase letter/digit.
static const struct { const char *name; int key; } NAMED_KEYS[] = {
    { "space", KEY_SPACE }, { "enter", KEY_ENTER }, { "tab", KEY_TAB },
    { "backspace", KEY_BACKSPACE }, { "delete", KEY_DELETE }, { "escape", KEY_ESCAPE },
};
static const size_t NAMED_KEYS_COUNT = sizeof(NAMED_KEYS) / sizeof(NAMED_KEYS[0]);

static int parseKeyName(const char *name) {
    if (strlen(name) == 1) return toupper((unsigned char)name[0]);
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
static bool nTeSetCursor(Prolog *p, PlTerm *args[], int arity, void *ctx) {
    (void)arity; (void)ctx;
    long pos;
    if (!prologGetInt(p, args[0], &pos)) return false;
    if (pos < 0) pos = 0;
    editorSetCursor((size_t)pos);
    return true;
}

// --- lifecycle --------------------------------------------------------

static void scriptSetup(void) {
    pl = prologCreate();
    prologSetErrorHandler(pl, reportError, NULL);
    prologRegisterNative(pl, "te_action", 1, nTeAction, NULL);
    prologRegisterNative(pl, "te_echo", 1, nTeEcho, NULL);
    prologRegisterNative(pl, "te_insert", 1, nTeInsert, NULL);
    prologRegisterNative(pl, "te_text", 1, nTeText, NULL);
    prologRegisterNative(pl, "te_cursor", 1, nTeCursor, NULL);
    prologRegisterNative(pl, "te_set_cursor", 1, nTeSetCursor, NULL);
}

void scriptInit(void) {
    scriptSetup();
    char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(path, sizeof path, "%s/te/init.pl", xdg);
    else if (home && home[0]) snprintf(path, sizeof path, "%s/.config/te/init.pl", home);
    else return;
    prologConsultFile(pl, path);
}
void scriptInitFromFile(const char *path) {
    scriptSetup();
    prologConsultFile(pl, path);
}
void scriptShutdown(void) {
    if (pl) { prologDestroy(pl); pl = NULL; }
}

// --- key/hook dispatch --------------------------------------------------
// key_binding/3 and leader_binding/3 are solved fresh every call (via
// findall) rather than cached, so a Handler rule can consult live editor
// state (te_text/1 etc.) -- see prolog.h's design note on this.

static bool matchAndRun(const char *predName, bool (*modMatches)(Mod, bool, bool),
                         bool cmd, bool shift, bool repeatable, bool (*keyPressed)(int key)) {
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
            currentAllowsRepeat = repeatable;
            prologSolve(pl, handler);
            matched = true;
            break;
        }
    }
    prologReset(pl, mark);
    return matched;
}
static bool topKeyPressed(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }
static bool prefixKeyPressed(int key) { return IsKeyPressed(key); }
// Adapts modMatchesChord's (Mod,bool shift) signature to matchAndRun's
// (Mod,bool cmd,bool shift) callback shape -- Ctrl is irrelevant/optional
// for a chord, only shift narrows it (see binding.h).
static bool modMatchesChordAdapter(Mod mod, bool cmd, bool shift) { (void)cmd; return modMatchesChord(mod, shift); }

bool scriptHandleKey(bool cmd, bool shift) {
    return matchAndRun("key_binding", modMatchesKey, cmd, shift, /*repeatable=*/true, topKeyPressed);
}
bool scriptHandlePrefixKey(bool shift) {
    return matchAndRun("leader_binding", modMatchesChordAdapter, /*cmd=*/false, shift, /*repeatable=*/false, prefixKeyPressed);
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

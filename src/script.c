// Lua integration -- see script.h. Registers a `te` table (te.action,
// te.echo, te.insert, te.text, te.cursor, te.set_cursor, te.bind) and loads
// it into an optional user init.lua at startup.
#include "script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "raylib.h"
#include "binding.h"
#include "editor.h"

static lua_State *L = NULL;

// A key bound via te.bind(): either an existing named action (looked up in
// COMMANDS, same as a typed command) or a Lua function held in the registry.
typedef enum { SB_ACTION, SB_FUNCTION } ScriptBindingKind;
typedef struct {
    int key;
    Mod mod;
    ScriptBindingKind kind;
    Action action;   // when kind == SB_ACTION
    int fn_ref;      // when kind == SB_FUNCTION (LUA_REGISTRYINDEX ref)
} ScriptBinding;

static ScriptBinding *bindings = NULL;
static size_t bindings_count = 0;
static size_t bindings_cap = 0;

static void reportError(const char *prefix) {
    const char *msg = lua_tostring(L, -1);
    char buf[512];
    snprintf(buf, sizeof buf, "%s: %s", prefix, msg ? msg : "unknown error");
    editorEcho(buf);
    lua_pop(L, 1);
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
    for (size_t i = 0; i < NAMED_KEYS_COUNT; i++) {
        if (strcmp(name, NAMED_KEYS[i].name) == 0) return NAMED_KEYS[i].key;
    }
    return -1;
}

static bool parseModName(const char *name, Mod *out) {
    if (strcmp(name, "any") == 0) { *out = MOD_ANY; return true; }
    if (strcmp(name, "ctrl") == 0) { *out = MOD_CTRL; return true; }
    if (strcmp(name, "ctrl-shift") == 0) { *out = MOD_CTRL_SHIFT; return true; }
    return false;
}

static bool lookupAction(const char *name, Action *out) {
    for (size_t i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) {
            *out = COMMANDS[i].action;
            return true;
        }
    }
    return false;
}

// --- te.* API ---------------------------------------------------------

static int l_action(lua_State *l) {
    const char *name = luaL_checkstring(l, 1);
    Action action;
    if (!lookupAction(name, &action)) return luaL_error(l, "unknown action '%s'", name);
    editorRunAction(action);
    return 0;
}

static int l_echo(lua_State *l) {
    editorEcho(luaL_checkstring(l, 1));
    return 0;
}

static int l_insert(lua_State *l) {
    size_t n;
    const char *s = luaL_checklstring(l, 1, &n);
    editorInsertText((const unsigned char *)s, n);
    return 0;
}

static int l_text(lua_State *l) {
    size_t n;
    const unsigned char *s = editorGetText(&n);
    lua_pushlstring(l, (const char *)s, n);
    return 1;
}

static int l_cursor(lua_State *l) {
    lua_pushinteger(l, (lua_Integer)editorGetCursor());
    return 1;
}

static int l_set_cursor(lua_State *l) {
    lua_Integer pos = luaL_checkinteger(l, 1);
    if (pos < 0) pos = 0;
    editorSetCursor((size_t)pos);
    return 0;
}

static int l_bind(lua_State *l) {
    const char *key_name = luaL_checkstring(l, 1);
    const char *mod_name = luaL_checkstring(l, 2);
    luaL_checkany(l, 3);

    int key = parseKeyName(key_name);
    if (key < 0) return luaL_error(l, "te.bind: unknown key '%s'", key_name);
    Mod mod;
    if (!parseModName(mod_name, &mod)) return luaL_error(l, "te.bind: unknown mods '%s'", mod_name);

    ScriptBinding sb = { .key = key, .mod = mod };
    if (lua_isstring(l, 3)) {
        if (!lookupAction(lua_tostring(l, 3), &sb.action)) {
            return luaL_error(l, "te.bind: unknown action '%s'", lua_tostring(l, 3));
        }
        sb.kind = SB_ACTION;
    } else if (lua_isfunction(l, 3)) {
        lua_pushvalue(l, 3);
        sb.kind = SB_FUNCTION;
        sb.fn_ref = luaL_ref(l, LUA_REGISTRYINDEX);
    } else {
        return luaL_error(l, "te.bind: handler must be an action name or a function");
    }

    if (bindings_count == bindings_cap) {
        bindings_cap = bindings_cap ? bindings_cap * 2 : 8;
        bindings = realloc(bindings, bindings_cap * sizeof(ScriptBinding));
    }
    bindings[bindings_count++] = sb;
    return 0;
}

static const luaL_Reg TE_FUNCS[] = {
    { "action", l_action },
    { "echo", l_echo },
    { "insert", l_insert },
    { "text", l_text },
    { "cursor", l_cursor },
    { "set_cursor", l_set_cursor },
    { "bind", l_bind },
    { NULL, NULL },
};

// --- lifecycle ----------------------------------------------------------

static void scriptSetup(void) {
    L = luaL_newstate();
    if (!L) return;
    luaL_openlibs(L);
    luaL_newlib(L, TE_FUNCS);
    lua_setglobal(L, "te");
}

static void scriptLoadFile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return; // no init file -- not an error, same as a missing .emacs
    fclose(f);
    if (luaL_dofile(L, path) != LUA_OK) reportError("lua error");
}

void scriptInit(void) {
    scriptSetup();
    if (!L) return;

    char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(path, sizeof path, "%s/te/init.lua", xdg);
    else if (home && home[0]) snprintf(path, sizeof path, "%s/.config/te/init.lua", home);
    else return;

    scriptLoadFile(path);
}

void scriptInitFromFile(const char *path) {
    scriptSetup();
    if (!L) return;
    scriptLoadFile(path);
}

void scriptShutdown(void) {
    if (L) {
        lua_close(L);
        L = NULL;
    }
    free(bindings);
    bindings = NULL;
    bindings_count = 0;
    bindings_cap = 0;
}

bool scriptHandleKey(bool cmd, bool shift) {
    if (!L) return false;
    for (size_t i = 0; i < bindings_count; i++) {
        ScriptBinding *b = &bindings[i];
        bool mod_ok;
        switch (b->mod) {
            case MOD_ANY: mod_ok = true; break;
            case MOD_CTRL: mod_ok = cmd && !shift; break;
            case MOD_CTRL_SHIFT: mod_ok = cmd && shift; break;
            default: mod_ok = false; break;
        }
        if (!mod_ok) continue;
        if (!(IsKeyPressed(b->key) || IsKeyPressedRepeat(b->key))) continue;

        if (b->kind == SB_ACTION) {
            editorRunAction(b->action);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, b->fn_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) reportError("lua error");
        }
        return true;
    }
    return false;
}

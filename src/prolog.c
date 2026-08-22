// A from-scratch Prolog engine -- see prolog.h. Facts/rules, unification,
// backtracking, cut, if-then-else, arithmetic, catch/throw, assert/retract,
// findall, and a practical-subset parser with a fixed infix operator table
// (no user-defined op/3). Not ISO-complete by design (see prolog.h).
//
// Two memory areas: `program` (permanent -- clause storage, freed only at
// prologDestroy) and `query` (transient -- everything a query builds, reset
// back to a mark by the caller once it's done with the results). Predicate
// and native-predicate tables are simple linear-scan arrays: config-scale
// predicate counts make that plenty fast and it keeps the code small.
#include "prolog.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *dupStr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    memcpy(p, s, n);
    return p;
}
static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

// --- Terms ------------------------------------------------------------

typedef int Atom;
// No distinct string type: per ISO, a "..." literal is a list of character
// codes (see mkCodeList/getTextFlexible), not its own term type.
typedef enum { T_VAR, T_ATOM, T_INT, T_FLT, T_CMP } Tag;

struct Term {
    Tag tag;
    union {
        struct Term *ref;   // T_VAR: self if unbound, else the bound value
        Atom atom;           // T_ATOM
        long i;              // T_INT
        double f;            // T_FLT
        struct { Atom functor; int arity; struct Term **args; } cmp; // T_CMP
    } u;
};
typedef struct Term Term;

// --- Arena (chunked bump allocator, mark/reset, never realloc'd) ------

#define ARENA_BLOCK_SIZE (64 * 1024)
// How many retract/1 calls accumulate before compactProgram (below) runs --
// config-scale, matching this file's own stated design philosophy (see the
// header comment).
#define PROLOG_COMPACT_RETRACT_INTERVAL 2000

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used, size;
    unsigned char data[];
} ArenaBlock;

typedef struct { ArenaBlock *first, *current; } Arena;
typedef struct { ArenaBlock *block; size_t used; } ArenaMark;

static ArenaBlock *arenaNewBlock(size_t minSize) {
    size_t sz = minSize > ARENA_BLOCK_SIZE ? minSize : ARENA_BLOCK_SIZE;
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + sz);
    b->next = NULL;
    b->used = 0;
    b->size = sz;
    return b;
}

static void *arenaAlloc(Arena *a, size_t n) {
    n = (n + 7u) & ~(size_t)7u;
    if (!a->current) a->first = a->current = arenaNewBlock(n);
    if (a->current->used + n > a->current->size) {
        if (a->current->next && a->current->next->size >= n) {
            a->current = a->current->next;
            a->current->used = 0;
        } else {
            ArenaBlock *nb = arenaNewBlock(n);
            nb->next = a->current->next;
            a->current->next = nb;
            a->current = nb;
        }
    }
    void *p = a->current->data + a->current->used;
    a->current->used += n;
    return p;
}

static ArenaMark arenaMark(Arena *a) {
    if (!a->current) return (ArenaMark){ NULL, 0 };
    return (ArenaMark){ a->current, a->current->used };
}
static void arenaReset(Arena *a, ArenaMark m) {
    if (!m.block) {
        for (ArenaBlock *b = a->first; b; b = b->next) b->used = 0;
        a->current = a->first;
        return;
    }
    m.block->used = m.used;
    for (ArenaBlock *b = m.block->next; b; b = b->next) b->used = 0;
    a->current = m.block;
}
static void arenaFreeAll(Arena *a) {
    ArenaBlock *b = a->first;
    while (b) { ArenaBlock *n = b->next; free(b); b = n; }
    a->first = a->current = NULL;
}

// --- Atom table ---------------------------------------------------------

typedef struct { char **names; size_t count, cap; } AtomTable;

// --- Clause / predicate database -----------------------------------------

typedef struct Clause {
    Term *head, *body; // body == NULL means a fact (equivalent to `true`)
    struct Clause *next;
} Clause;

typedef struct Predicate {
    Atom functor; int arity;
    Clause *first, *last;
} Predicate;

typedef struct { Predicate **items; size_t count, cap; } PredTable;

// --- Native predicates ----------------------------------------------------

typedef struct { Atom functor; int arity; PrologNative fn; void *ctx; } NativeEntry;
typedef struct { NativeEntry *items; size_t count, cap; } NativeTable;

// --- Trail (undo variable bindings on backtrack) --------------------------

typedef struct { Term **items; size_t top, cap; } Trail;

// --- catch/3 / throw/1 ----------------------------------------------------

typedef struct CatchFrame {
    jmp_buf jb;
    struct CatchFrame *prev;
    size_t trailMark;
    Term *ball;
} CatchFrame;

// --- Engine ---------------------------------------------------------------

struct Prolog {
    Arena program;
    Arena query;
    AtomTable atoms;
    PredTable preds;
    NativeTable natives;
    Trail trail;
    long nextBarrier;
    long cutSignal;
    CatchFrame *catchTop;
    PrologErrorFn errorFn;
    void *errorCtx;
    ArenaMark *markStack;
    size_t markStackTop, markStackCap;
    size_t retractsSinceCompact; // see compactProgram
    Atom atomComma, atomSemi, atomArrow, atomCut, atomTrue, atomFail, atomFalse,
         atomNaf, atomCall, atomCatch, atomThrow, atomColonDash, atomNil, atomDot, atomError;
};

static Atom internAtom(Prolog *pl, const char *name) {
    for (size_t i = 0; i < pl->atoms.count; i++)
        if (streq(pl->atoms.names[i], name)) return (Atom)i;
    if (pl->atoms.count == pl->atoms.cap) {
        pl->atoms.cap = pl->atoms.cap ? pl->atoms.cap * 2 : 64;
        pl->atoms.names = realloc(pl->atoms.names, pl->atoms.cap * sizeof(char *));
    }
    pl->atoms.names[pl->atoms.count] = dupStr(name);
    return (Atom)(pl->atoms.count++);
}
static const char *atomName(Prolog *pl, Atom a) { return pl->atoms.names[a]; }

// --- basic term constructors (arena-parameterized) ------------------------

static Term *newVar(Arena *a) {
    Term *t = arenaAlloc(a, sizeof(Term));
    t->tag = T_VAR;
    t->u.ref = t;
    return t;
}
static Term *mkAtomRaw(Arena *a, Atom id) {
    Term *t = arenaAlloc(a, sizeof(Term)); t->tag = T_ATOM; t->u.atom = id; return t;
}
static Term *mkIntRaw(Arena *a, long v) {
    Term *t = arenaAlloc(a, sizeof(Term)); t->tag = T_INT; t->u.i = v; return t;
}
static Term *mkFloatRaw(Arena *a, double v) {
    Term *t = arenaAlloc(a, sizeof(Term)); t->tag = T_FLT; t->u.f = v; return t;
}
static Term **allocArgs(Arena *a, int n) { return arenaAlloc(a, sizeof(Term *) * (size_t)n); }
static Term *mkCompoundRaw(Arena *a, Atom functor, int arity, Term **args) {
    Term *t = arenaAlloc(a, sizeof(Term));
    t->tag = T_CMP; t->u.cmp.functor = functor; t->u.cmp.arity = arity; t->u.cmp.args = args;
    return t;
}
static Term *mkCompound1(Arena *a, Atom functor, Term *arg) {
    Term **args = allocArgs(a, 1); args[0] = arg;
    return mkCompoundRaw(a, functor, 1, args);
}
// Builds a proper ISO list of character codes ('.'(Code,Tail).../[]) from
// raw bytes -- what a "..." literal parses to, and what te_text/1 etc.
// produce. "Codes" are bytes (0-255) here, not Unicode codepoints: te's
// buffer is raw UTF-8, and this keeps a multi-byte character decomposing
// the same way whether it came from the buffer or a source literal.
static Term *mkCodeList(Prolog *pl, const char *chars, size_t len) {
    Term *list = mkAtomRaw(&pl->query, pl->atomNil);
    for (size_t i = len; i > 0; i--) {
        Term **args = allocArgs(&pl->query, 2);
        args[0] = mkIntRaw(&pl->query, (unsigned char)chars[i - 1]);
        args[1] = list;
        list = mkCompoundRaw(&pl->query, pl->atomDot, 2, args);
    }
    return list;
}
// Builds a list of single-character atoms (atom_chars/2's shape) instead of
// codes.
static Term *mkCharList(Prolog *pl, const char *chars, size_t len) {
    Term *list = mkAtomRaw(&pl->query, pl->atomNil);
    for (size_t i = len; i > 0; i--) {
        char one[2] = { chars[i - 1], 0 };
        Term **args = allocArgs(&pl->query, 2);
        args[0] = mkAtomRaw(&pl->query, internAtom(pl, one));
        args[1] = list;
        list = mkCompoundRaw(&pl->query, pl->atomDot, 2, args);
    }
    return list;
}

static Term *deref(Term *t) {
    while (t->tag == T_VAR && t->u.ref != t) t = t->u.ref;
    return t;
}

// --- trail ------------------------------------------------------------

static void trailPush(Prolog *pl, Term *v) {
    if (pl->trail.top == pl->trail.cap) {
        pl->trail.cap = pl->trail.cap ? pl->trail.cap * 2 : 256;
        pl->trail.items = realloc(pl->trail.items, pl->trail.cap * sizeof(Term *));
    }
    pl->trail.items[pl->trail.top++] = v;
}
static void undoTrailTo(Prolog *pl, size_t mark) {
    while (pl->trail.top > mark) {
        pl->trail.top--;
        Term *v = pl->trail.items[pl->trail.top];
        v->u.ref = v;
    }
}
static void bindVar(Prolog *pl, Term *v, Term *val) {
    v->u.ref = val;
    trailPush(pl, v);
}

// --- unification / structural equality ------------------------------------

static bool unify(Prolog *pl, Term *a, Term *b) {
    a = deref(a); b = deref(b);
    if (a == b) return true;
    if (a->tag == T_VAR) { bindVar(pl, a, b); return true; }
    if (b->tag == T_VAR) { bindVar(pl, b, a); return true; }
    if (a->tag != b->tag) return false;
    switch (a->tag) {
    case T_ATOM: return a->u.atom == b->u.atom;
    case T_INT:  return a->u.i == b->u.i;
    case T_FLT:  return a->u.f == b->u.f;
    case T_CMP:
        if (a->u.cmp.functor != b->u.cmp.functor || a->u.cmp.arity != b->u.cmp.arity) return false;
        for (int i = 0; i < a->u.cmp.arity; i++)
            if (!unify(pl, a->u.cmp.args[i], b->u.cmp.args[i])) return false;
        return true;
    default: return false;
    }
}

static bool termEq(Term *a, Term *b) {
    a = deref(a); b = deref(b);
    if (a == b) return true;
    if (a->tag != b->tag) return false;
    switch (a->tag) {
    case T_ATOM: return a->u.atom == b->u.atom;
    case T_INT:  return a->u.i == b->u.i;
    case T_FLT:  return a->u.f == b->u.f;
    case T_CMP:
        if (a->u.cmp.functor != b->u.cmp.functor || a->u.cmp.arity != b->u.cmp.arity) return false;
        for (int i = 0; i < a->u.cmp.arity; i++)
            if (!termEq(a->u.cmp.args[i], b->u.cmp.args[i])) return false;
        return true;
    default: return false;
    }
}

// --- structure copying (clause renaming, assert, findall snapshots) ------
// Always allocates a fresh copy of every node in `dst` (never shares raw
// pointers across arena boundaries -- simple and safe, if not maximally
// fast); repeated occurrences of the same still-unbound variable within one
// copy share one fresh variable, via a small per-call association list.

typedef struct { Term *from, *to; } VarMapEntry;
typedef struct { VarMapEntry *items; size_t count, cap; } VarMap;

static void freeVarMap(VarMap *m) { free(m->items); m->items = NULL; m->count = m->cap = 0; }

static Term *copyTermRec(Arena *dst, Term *t, VarMap *map) {
    t = deref(t);
    switch (t->tag) {
    case T_VAR: {
        for (size_t i = 0; i < map->count; i++)
            if (map->items[i].from == t) return map->items[i].to;
        Term *fresh = newVar(dst);
        if (map->count == map->cap) {
            map->cap = map->cap ? map->cap * 2 : 8;
            map->items = realloc(map->items, map->cap * sizeof(VarMapEntry));
        }
        map->items[map->count++] = (VarMapEntry){ t, fresh };
        return fresh;
    }
    case T_ATOM: return mkAtomRaw(dst, t->u.atom);
    case T_INT:  return mkIntRaw(dst, t->u.i);
    case T_FLT:  return mkFloatRaw(dst, t->u.f);
    case T_CMP: {
        Term **newArgs = allocArgs(dst, t->u.cmp.arity);
        for (int i = 0; i < t->u.cmp.arity; i++)
            newArgs[i] = copyTermRec(dst, t->u.cmp.args[i], map);
        return mkCompoundRaw(dst, t->u.cmp.functor, t->u.cmp.arity, newArgs);
    }
    }
    return t;
}
static Term *copyTermFresh(Arena *dst, Term *t) {
    VarMap map = { 0 };
    Term *r = copyTermRec(dst, t, &map);
    freeVarMap(&map);
    return r;
}

// --- term -> text (error messages) ----------------------------------------

typedef struct { char *buf; size_t cap, pos; } WBuf;
static void wputn(WBuf *w, const char *s, size_t n) {
    size_t room = w->pos < w->cap ? w->cap - w->pos - 1 : 0;
    size_t c = n < room ? n : room;
    if (c) { memcpy(w->buf + w->pos, s, c); w->pos += c; }
    if (w->pos < w->cap) w->buf[w->pos] = 0;
}
static void wputs(WBuf *w, const char *s) { wputn(w, s, strlen(s)); }

static void writeTermBuf(Prolog *pl, Term *t, WBuf *w) {
    t = deref(t);
    char tmp[64];
    switch (t->tag) {
    case T_VAR: snprintf(tmp, sizeof tmp, "_G%p", (void *)t); wputs(w, tmp); break;
    case T_ATOM: wputs(w, atomName(pl, t->u.atom)); break;
    case T_INT: snprintf(tmp, sizeof tmp, "%ld", t->u.i); wputs(w, tmp); break;
    case T_FLT: snprintf(tmp, sizeof tmp, "%g", t->u.f); wputs(w, tmp); break;
    case T_CMP:
        if (t->u.cmp.functor == pl->atomDot && t->u.cmp.arity == 2) {
            wputs(w, "[");
            Term *cur = t; bool first = true;
            for (;;) {
                cur = deref(cur);
                if (cur->tag == T_CMP && cur->u.cmp.functor == pl->atomDot && cur->u.cmp.arity == 2) {
                    if (!first) wputs(w, ",");
                    writeTermBuf(pl, cur->u.cmp.args[0], w);
                    first = false;
                    cur = cur->u.cmp.args[1];
                } else if (cur->tag == T_ATOM && cur->u.atom == pl->atomNil) {
                    break;
                } else {
                    wputs(w, "|"); writeTermBuf(pl, cur, w); break;
                }
            }
            wputs(w, "]");
        } else {
            wputs(w, atomName(pl, t->u.cmp.functor));
            wputs(w, "(");
            for (int i = 0; i < t->u.cmp.arity; i++) {
                if (i) wputs(w, ",");
                writeTermBuf(pl, t->u.cmp.args[i], w);
            }
            wputs(w, ")");
        }
        break;
    }
}
static void writeTerm(Prolog *pl, Term *t, char *buf, size_t bufsz) {
    WBuf w = { buf, bufsz, 0 };
    if (bufsz) buf[0] = 0;
    writeTermBuf(pl, t, &w);
}

// Renders an ISO error(Formal, Context) ball into readable text for
// editorEcho -- not a certified-conformance message wording, just enough to
// be useful. Falls back to a generic term dump for anything else (a user's
// own throw(whatever), or an unrecognized Formal shape).
static void formatBallMessage(Prolog *pl, Term *ball, char *buf, size_t bufsz) {
    Term *b = deref(ball);
    if (!(b->tag == T_CMP && b->u.cmp.arity == 2 && b->u.cmp.functor == pl->atomError)) {
        writeTerm(pl, b, buf, bufsz);
        return;
    }
    Term *formal = deref(b->u.cmp.args[0]);
    const char *fn = formal->tag == T_ATOM ? atomName(pl, formal->u.atom)
                    : formal->tag == T_CMP ? atomName(pl, formal->u.cmp.functor) : NULL;
    if (fn && formal->tag == T_ATOM && streq(fn, "instantiation_error")) {
        snprintf(buf, bufsz, "instantiation error"); return;
    }
    if (fn && formal->tag == T_CMP && formal->u.cmp.arity == 2 && streq(fn, "type_error")) {
        char typeBuf[64], culpritBuf[200];
        writeTerm(pl, formal->u.cmp.args[0], typeBuf, sizeof typeBuf);
        writeTerm(pl, formal->u.cmp.args[1], culpritBuf, sizeof culpritBuf);
        snprintf(buf, bufsz, "type error: expected %s, got %s", typeBuf, culpritBuf);
        return;
    }
    if (fn && formal->tag == T_CMP && formal->u.cmp.arity == 2 && streq(fn, "domain_error")) {
        char domBuf[64], culpritBuf[200];
        writeTerm(pl, formal->u.cmp.args[0], domBuf, sizeof domBuf);
        writeTerm(pl, formal->u.cmp.args[1], culpritBuf, sizeof culpritBuf);
        snprintf(buf, bufsz, "domain error: expected %s, got %s", domBuf, culpritBuf);
        return;
    }
    if (fn && formal->tag == T_CMP && formal->u.cmp.arity == 2 && streq(fn, "existence_error")) {
        char typeBuf[64], culpritBuf[200];
        writeTerm(pl, formal->u.cmp.args[0], typeBuf, sizeof typeBuf);
        writeTerm(pl, formal->u.cmp.args[1], culpritBuf, sizeof culpritBuf);
        snprintf(buf, bufsz, "existence error: %s %s does not exist", typeBuf, culpritBuf);
        return;
    }
    if (fn && formal->tag == T_CMP && formal->u.cmp.arity == 3 && streq(fn, "permission_error")) {
        char opBuf[64], typeBuf[64], culpritBuf[200];
        writeTerm(pl, formal->u.cmp.args[0], opBuf, sizeof opBuf);
        writeTerm(pl, formal->u.cmp.args[1], typeBuf, sizeof typeBuf);
        writeTerm(pl, formal->u.cmp.args[2], culpritBuf, sizeof culpritBuf);
        snprintf(buf, bufsz, "permission error: no permission to %s %s %s", opBuf, typeBuf, culpritBuf);
        return;
    }
    if (fn && formal->tag == T_CMP && formal->u.cmp.arity == 1 && streq(fn, "evaluation_error")) {
        char whatBuf[64];
        writeTerm(pl, formal->u.cmp.args[0], whatBuf, sizeof whatBuf);
        snprintf(buf, bufsz, "evaluation error: %s", whatBuf);
        return;
    }
    if (fn && formal->tag == T_CMP && formal->u.cmp.arity == 1 && streq(fn, "te_error")) {
        Term *msg = deref(formal->u.cmp.args[0]);
        if (msg->tag == T_ATOM) { snprintf(buf, bufsz, "%s", atomName(pl, msg->u.atom)); return; }
    }
    writeTerm(pl, b, buf, bufsz);
}

static void reportPlainError(Prolog *pl, const char *msg) {
    if (pl->errorFn) pl->errorFn(msg, pl->errorCtx);
}

// --- throw / ISO structured errors -----------------------------------
// Every engine-raised error is a standard error(Formal, Context) ball
// (Context left an unbound var -- ISO doesn't mandate its content), built
// fresh from these typed constructors rather than a printf-style message,
// so a user's own catch/3 can pattern-match the Formal term.

static _Noreturn void throwBall(Prolog *pl, Term *ball) {
    if (!pl->catchTop) abort(); // invariant: solveTopLevel always installs a frame first
    CatchFrame *f = pl->catchTop;
    pl->catchTop = f->prev;
    f->ball = ball;
    longjmp(f->jb, 1);
}
static _Noreturn void throwFormal(Prolog *pl, Term *formal) {
    Term *ctx = newVar(&pl->query);
    Term **args = allocArgs(&pl->query, 2);
    args[0] = formal; args[1] = ctx;
    throwBall(pl, mkCompoundRaw(&pl->query, pl->atomError, 2, args));
}
static _Noreturn void throwInstantiationError(Prolog *pl) {
    throwFormal(pl, mkAtomRaw(&pl->query, internAtom(pl, "instantiation_error")));
}
static _Noreturn void throwTypeError(Prolog *pl, const char *validType, Term *culprit) {
    Term **a = allocArgs(&pl->query, 2);
    a[0] = mkAtomRaw(&pl->query, internAtom(pl, validType));
    a[1] = culprit;
    throwFormal(pl, mkCompoundRaw(&pl->query, internAtom(pl, "type_error"), 2, a));
}
static Term *mkSlash(Prolog *pl, Atom name, int arity) {
    Term **a = allocArgs(&pl->query, 2);
    a[0] = mkAtomRaw(&pl->query, name);
    a[1] = mkIntRaw(&pl->query, arity);
    return mkCompoundRaw(&pl->query, internAtom(pl, "/"), 2, a);
}
static _Noreturn void throwExistenceError(Prolog *pl, const char *objType, Term *culprit) {
    Term **a = allocArgs(&pl->query, 2);
    a[0] = mkAtomRaw(&pl->query, internAtom(pl, objType));
    a[1] = culprit;
    throwFormal(pl, mkCompoundRaw(&pl->query, internAtom(pl, "existence_error"), 2, a));
}
static _Noreturn void throwEvaluationError(Prolog *pl, const char *what) {
    throwFormal(pl, mkCompound1(&pl->query, internAtom(pl, "evaluation_error"),
                                 mkAtomRaw(&pl->query, internAtom(pl, what))));
}

// Not an ISO formal error, but follows the same error(Formal, Context)
// wrapping convention (common practice for implementation-specific errors).
// Used by native te_* predicates in script.c via the public API.
_Noreturn void prologThrowMsg(Prolog *pl, const char *message) {
    Term *msgAtom = mkAtomRaw(&pl->query, internAtom(pl, message));
    throwFormal(pl, mkCompound1(&pl->query, internAtom(pl, "te_error"), msgAtom));
}

// --- arithmetic ---------------------------------------------------------

typedef struct { bool isFloat; long i; double f; } Num;
static double numAsF(Num n) { return n.isFloat ? n.f : (double)n.i; }

static Num evalArith(Prolog *pl, Term *t) {
    t = deref(t);
    if (t->tag == T_INT) return (Num){ false, t->u.i, 0 };
    if (t->tag == T_FLT) return (Num){ true, 0, t->u.f };
    if (t->tag == T_VAR) throwInstantiationError(pl);
    if (t->tag == T_CMP) {
        const char *f = atomName(pl, t->u.cmp.functor);
        int ar = t->u.cmp.arity;
        if (ar == 1) {
            Num a = evalArith(pl, t->u.cmp.args[0]);
            if (streq(f, "-")) return a.isFloat ? (Num){ true, 0, -a.f } : (Num){ false, -a.i, 0 };
            if (streq(f, "+")) return a;
            if (streq(f, "abs")) return a.isFloat ? (Num){ true, 0, fabs(a.f) } : (Num){ false, labs(a.i), 0 };
        } else if (ar == 2) {
            Num a = evalArith(pl, t->u.cmp.args[0]);
            Num b = evalArith(pl, t->u.cmp.args[1]);
            bool fl = a.isFloat || b.isFloat;
            if (streq(f, "+")) return fl ? (Num){ true, 0, numAsF(a) + numAsF(b) } : (Num){ false, a.i + b.i, 0 };
            if (streq(f, "-")) return fl ? (Num){ true, 0, numAsF(a) - numAsF(b) } : (Num){ false, a.i - b.i, 0 };
            if (streq(f, "*")) return fl ? (Num){ true, 0, numAsF(a) * numAsF(b) } : (Num){ false, a.i * b.i, 0 };
            if (streq(f, "/")) return (Num){ true, 0, numAsF(a) / numAsF(b) };
            if (streq(f, "//")) {
                if (fl) throwTypeError(pl, "integer", mkFloatRaw(&pl->query, a.isFloat ? a.f : b.f));
                if (b.i == 0) throwEvaluationError(pl, "zero_divisor");
                return (Num){ false, a.i / b.i, 0 };
            }
            if (streq(f, "mod")) {
                if (fl) throwTypeError(pl, "integer", mkFloatRaw(&pl->query, a.isFloat ? a.f : b.f));
                if (b.i == 0) throwEvaluationError(pl, "zero_divisor");
                long m = a.i % b.i;
                if (m != 0 && ((m < 0) != (b.i < 0))) m += b.i;
                return (Num){ false, m, 0 };
            }
            if (streq(f, "min")) return numAsF(a) < numAsF(b) ? a : b;
            if (streq(f, "max")) return numAsF(a) > numAsF(b) ? a : b;
        }
    }
    if (t->tag == T_ATOM) throwTypeError(pl, "evaluable", mkSlash(pl, t->u.atom, 0));
    throwTypeError(pl, "evaluable", mkSlash(pl, t->u.cmp.functor, t->u.cmp.arity));
}

// --- getTextFlexible (atom or proper code list -> chars) -----------------
// Accepts either an atom (key/mod names, te_action, atom_concat, ...) or a
// proper ISO code list (what "..." now parses to) and returns a pointer to
// a NUL-terminated byte run: for an atom, straight into the atom table; for
// a code list, materialized into a fresh query-arena buffer (transient,
// reclaimed at the next mark/reset like everything else a query builds).
// Returns false if `t` is neither (a bare compound, an improper list, a
// list containing something other than an integer 0-255, ...).

static bool getTextFlexible(Prolog *pl, Term *t, const char **out, size_t *outLen) {
    t = deref(t);
    if (t->tag == T_ATOM && t->u.atom == pl->atomNil) { *out = ""; *outLen = 0; return true; }
    if (t->tag == T_ATOM) { const char *n = atomName(pl, t->u.atom); *out = n; *outLen = strlen(n); return true; }
    size_t count = 0;
    Term *cur = t;
    for (;;) {
        if (cur->tag == T_CMP && cur->u.cmp.functor == pl->atomDot && cur->u.cmp.arity == 2) {
            Term *head = deref(cur->u.cmp.args[0]);
            if (head->tag != T_INT || head->u.i < 0 || head->u.i > 255) return false;
            count++;
            cur = deref(cur->u.cmp.args[1]);
        } else break;
    }
    if (!(cur->tag == T_ATOM && cur->u.atom == pl->atomNil)) return false;
    char *buf = arenaAlloc(&pl->query, count + 1);
    size_t i = 0;
    cur = t;
    while (cur->tag == T_CMP && cur->u.cmp.functor == pl->atomDot && cur->u.cmp.arity == 2) {
        buf[i++] = (char)(unsigned char)deref(cur->u.cmp.args[0])->u.i;
        cur = deref(cur->u.cmp.args[1]);
    }
    buf[count] = 0;
    *out = buf; *outLen = count;
    return true;
}
// Walks a proper list of single-character atoms (atom_chars/2's input
// shape) into a caller-supplied buffer.
static bool getCharListText(Prolog *pl, Term *t, char *buf, size_t bufcap, size_t *outLen) {
    t = deref(t);
    size_t o = 0;
    while (t->tag == T_CMP && t->u.cmp.functor == pl->atomDot && t->u.cmp.arity == 2) {
        Term *head = deref(t->u.cmp.args[0]);
        if (head->tag != T_ATOM) return false;
        const char *n = atomName(pl, head->u.atom);
        if (strlen(n) != 1) return false;
        if (o < bufcap) buf[o] = n[0];
        o++;
        t = deref(t->u.cmp.args[1]);
    }
    if (!(t->tag == T_ATOM && t->u.atom == pl->atomNil)) return false;
    *outLen = o < bufcap ? o : bufcap;
    return true;
}
// Collects a proper list's elements into a fresh malloc'd C array (caller
// frees). Returns false if `list` isn't a proper list.
static bool collectList(Prolog *pl, Term *list, Term ***outItems, size_t *outCount) {
    Term **items = NULL; size_t count = 0, cap = 0;
    Term *cur = deref(list);
    while (cur->tag == T_CMP && cur->u.cmp.functor == pl->atomDot && cur->u.cmp.arity == 2) {
        if (count == cap) { cap = cap ? cap * 2 : 8; items = realloc(items, cap * sizeof(Term *)); }
        items[count++] = cur->u.cmp.args[0];
        cur = deref(cur->u.cmp.args[1]);
    }
    if (!(cur->tag == T_ATOM && cur->u.atom == pl->atomNil)) { free(items); return false; }
    *outItems = items; *outCount = count;
    return true;
}
static Term *buildList(Prolog *pl, Term **items, size_t count) {
    Term *list = mkAtomRaw(&pl->query, pl->atomNil);
    for (size_t i = count; i > 0; i--) {
        Term **args = allocArgs(&pl->query, 2);
        args[0] = items[i - 1]; args[1] = list;
        list = mkCompoundRaw(&pl->query, pl->atomDot, 2, args);
    }
    return list;
}

// --- predicate database ---------------------------------------------------

static Predicate *findPred(Prolog *pl, Atom functor, int arity) {
    for (size_t i = 0; i < pl->preds.count; i++)
        if (pl->preds.items[i]->functor == functor && pl->preds.items[i]->arity == arity)
            return pl->preds.items[i];
    return NULL;
}
static Predicate *findOrCreatePred(Prolog *pl, Atom functor, int arity) {
    Predicate *p = findPred(pl, functor, arity);
    if (p) return p;
    p = malloc(sizeof(Predicate));
    p->functor = functor; p->arity = arity; p->first = p->last = NULL;
    if (pl->preds.count == pl->preds.cap) {
        pl->preds.cap = pl->preds.cap ? pl->preds.cap * 2 : 16;
        pl->preds.items = realloc(pl->preds.items, pl->preds.cap * sizeof(Predicate *));
    }
    pl->preds.items[pl->preds.count++] = p;
    return p;
}
static void addClause(Prolog *pl, Term *head, Term *body, bool atEnd) {
    Atom functor; int arity;
    if (head->tag == T_ATOM) { functor = head->u.atom; arity = 0; }
    else if (head->tag == T_CMP) { functor = head->u.cmp.functor; arity = head->u.cmp.arity; }
    else throwTypeError(pl, "callable", head);
    Predicate *pred = findOrCreatePred(pl, functor, arity);
    Clause *c = arenaAlloc(&pl->program, sizeof(Clause));
    c->head = head; c->body = body; c->next = NULL;
    if (!pred->first) { pred->first = pred->last = c; }
    else if (atEnd) { pred->last->next = c; pred->last = c; }
    else { c->next = pred->first; pred->first = c; }
}

static NativeEntry *lookupNative(Prolog *pl, Atom functor, int arity) {
    for (size_t i = 0; i < pl->natives.count; i++)
        if (pl->natives.items[i].functor == functor && pl->natives.items[i].arity == arity)
            return &pl->natives.items[i];
    return NULL;
}
void prologRegisterNative(Prolog *pl, const char *name, int arity, PrologNative fn, void *ctx) {
    Atom a = internAtom(pl, name);
    NativeEntry *existing = lookupNative(pl, a, arity);
    if (existing) { existing->fn = fn; existing->ctx = ctx; return; }
    if (pl->natives.count == pl->natives.cap) {
        pl->natives.cap = pl->natives.cap ? pl->natives.cap * 2 : 16;
        pl->natives.items = realloc(pl->natives.items, pl->natives.cap * sizeof(NativeEntry));
    }
    pl->natives.items[pl->natives.count++] = (NativeEntry){ a, arity, fn, ctx };
}

// --- solver -----------------------------------------------------------
// CPS: solve(Goal, barrier, sc, ctx) calls sc() on every solution it finds;
// sc returning true means "accept, stop searching" (propagated back out
// through the whole call chain); false means "keep backtracking". `barrier`
// identifies the innermost enclosing user-predicate-call (or call/N, \+,
// catch/3 Goal) scope, for cut (!) to target -- see pl->cutSignal.

typedef bool (*SolveCont)(Prolog *pl, void *ctx);
static bool solve(Prolog *pl, Term *goal, long barrier, SolveCont sc, void *skctx);
static void consultBuffer(Prolog *pl, const char *buf, size_t len);

static bool scAcceptFirst(Prolog *pl, void *ctx) { (void)pl; (void)ctx; return true; }

// Runs `g` in a fresh cut scope (opaque to the caller's cut) via `sc`.
static bool solveOpaque(Prolog *pl, Term *g, SolveCont sc, void *ctx) {
    long b = pl->nextBarrier++;
    bool r = solve(pl, g, b, sc, ctx);
    if (pl->cutSignal == b) pl->cutSignal = 0;
    return r;
}
static bool solveOnce(Prolog *pl, Term *g) { return solveOpaque(pl, g, scAcceptFirst, NULL); }

typedef struct { Term *b; long barrier; SolveCont sc; void *skctx; } ConjCtx;
static bool conjCont(Prolog *pl, void *ctx) {
    ConjCtx *c = ctx;
    return solve(pl, c->b, c->barrier, c->sc, c->skctx);
}

static bool solve(Prolog *pl, Term *goal, long barrier, SolveCont sc, void *skctx) {
    goal = deref(goal);
    if (goal->tag == T_VAR) throwInstantiationError(pl);
    Atom functor; int arity; Term **args;
    if (goal->tag == T_ATOM) { functor = goal->u.atom; arity = 0; args = NULL; }
    else if (goal->tag == T_CMP) { functor = goal->u.cmp.functor; arity = goal->u.cmp.arity; args = goal->u.cmp.args; }
    else throwTypeError(pl, "callable", goal);

    if (functor == pl->atomComma && arity == 2) {
        ConjCtx cc = { args[1], barrier, sc, skctx };
        return solve(pl, args[0], barrier, conjCont, &cc);
    }
    if (functor == pl->atomSemi && arity == 2) {
        Term *A = deref(args[0]);
        if (A->tag == T_CMP && A->u.cmp.functor == pl->atomArrow && A->u.cmp.arity == 2) {
            Term *cond = A->u.cmp.args[0], *then = A->u.cmp.args[1], *elseG = args[1];
            if (solveOnce(pl, cond)) return solve(pl, then, barrier, sc, skctx);
            return solve(pl, elseG, barrier, sc, skctx);
        }
        size_t tmark = pl->trail.top;
        if (solve(pl, args[0], barrier, sc, skctx)) return true;
        if (pl->cutSignal) return false;
        undoTrailTo(pl, tmark);
        return solve(pl, args[1], barrier, sc, skctx);
    }
    if (functor == pl->atomArrow && arity == 2) {
        if (solveOnce(pl, args[0])) return solve(pl, args[1], barrier, sc, skctx);
        return false;
    }
    if (functor == pl->atomCut && arity == 0) {
        bool r = sc(pl, skctx);
        pl->cutSignal = barrier;
        return r;
    }
    if (functor == pl->atomTrue && arity == 0) return sc(pl, skctx);
    if ((functor == pl->atomFail || functor == pl->atomFalse) && arity == 0) return false;
    if (functor == pl->atomNaf && arity == 1) {
        size_t tmark = pl->trail.top;
        bool inner = solveOnce(pl, args[0]);
        undoTrailTo(pl, tmark);
        return inner ? false : sc(pl, skctx);
    }
    if (functor == pl->atomCall && arity >= 1 && arity <= 8) {
        Term *base = deref(args[0]);
        Term *effective;
        int extra = arity - 1;
        if (extra == 0) effective = base;
        else {
            Atom bf; int bar; Term **bargs = NULL;
            if (base->tag == T_ATOM) { bf = base->u.atom; bar = 0; }
            else if (base->tag == T_CMP) { bf = base->u.cmp.functor; bar = base->u.cmp.arity; bargs = base->u.cmp.args; }
            else { throwTypeError(pl, "callable", base); }
            int newArity = bar + extra;
            Term **newArgs = allocArgs(&pl->query, newArity);
            for (int i = 0; i < bar; i++) newArgs[i] = bargs[i];
            for (int i = 0; i < extra; i++) newArgs[bar + i] = args[1 + i];
            effective = mkCompoundRaw(&pl->query, bf, newArity, newArgs);
        }
        return solveOpaque(pl, effective, sc, skctx);
    }
    if (functor == pl->atomCatch && arity == 3) {
        Term *Goal = args[0], *Catcher = args[1], *Recovery = args[2];
        CatchFrame frame; frame.prev = pl->catchTop; frame.trailMark = pl->trail.top; frame.ball = NULL;
        pl->catchTop = &frame;
        if (setjmp(frame.jb) == 0) {
            bool r = solveOpaque(pl, Goal, sc, skctx);
            pl->catchTop = frame.prev;
            return r;
        } else {
            pl->catchTop = frame.prev;
            undoTrailTo(pl, frame.trailMark);
            Term *ball = frame.ball;
            size_t m2 = pl->trail.top;
            if (!unify(pl, Catcher, ball)) {
                undoTrailTo(pl, m2);
                throwBall(pl, ball);
            }
            return solve(pl, Recovery, barrier, sc, skctx);
        }
    }
    if (functor == pl->atomThrow && arity == 1) {
        Term *ball = copyTermFresh(&pl->query, args[0]);
        throwBall(pl, ball);
    }

    NativeEntry *ne = lookupNative(pl, functor, arity);
    if (ne) return ne->fn(pl, args, arity, ne->ctx) ? sc(pl, skctx) : false;

    Predicate *pred = findPred(pl, functor, arity);
    if (!pred) throwExistenceError(pl, "procedure", mkSlash(pl, functor, arity));

    long myBarrier = pl->nextBarrier++;
    for (Clause *c = pred->first; c; c = c->next) {
        size_t tmark = pl->trail.top;
        VarMap map = { 0 };
        Term *h = copyTermRec(&pl->query, c->head, &map);
        bool headOk = true;
        if (arity > 0)
            for (int i = 0; i < arity && headOk; i++) headOk = unify(pl, h->u.cmp.args[i], args[i]);
        if (!headOk) { freeVarMap(&map); undoTrailTo(pl, tmark); continue; }
        Term *b = c->body ? copyTermRec(&pl->query, c->body, &map) : NULL;
        freeVarMap(&map);
        bool r = b ? solve(pl, b, myBarrier, sc, skctx) : sc(pl, skctx);
        if (pl->cutSignal == myBarrier) { pl->cutSignal = 0; return r; }
        if (r) return true;
        if (pl->cutSignal) return false;
        undoTrailTo(pl, tmark);
    }
    return false;
}

static bool solveTopLevel(Prolog *pl, Term *goal) {
    CatchFrame frame; frame.prev = pl->catchTop; frame.trailMark = pl->trail.top; frame.ball = NULL;
    pl->catchTop = &frame;
    bool ok;
    if (setjmp(frame.jb) == 0) {
        ok = solveOpaque(pl, goal, scAcceptFirst, NULL);
        pl->catchTop = frame.prev;
    } else {
        pl->catchTop = frame.prev;
        undoTrailTo(pl, frame.trailMark);
        char buf[512];
        formatBallMessage(pl, frame.ball, buf, sizeof buf);
        reportPlainError(pl, buf);
        ok = false;
    }
    return ok;
}

bool prologSolve(Prolog *pl, PlTerm *goal) { return solveTopLevel(pl, goal); }

// --- standard library (native predicates registered at prologCreate) ------

static bool nativeUnify(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx; return unify(pl, a[0], a[1]);
}
static bool nativeNotUnify(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    size_t m = pl->trail.top;
    bool u = unify(pl, a[0], a[1]);
    undoTrailTo(pl, m);
    return !u;
}
static bool nativeEq(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)pl; (void)arity; (void)ctx; return termEq(a[0], a[1]);
}
static bool nativeNeq(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)pl; (void)arity; (void)ctx; return !termEq(a[0], a[1]);
}
static bool nativeIs(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Num n = evalArith(pl, a[1]);
    Term *result = n.isFloat ? mkFloatRaw(&pl->query, n.f) : mkIntRaw(&pl->query, n.i);
    return unify(pl, a[0], result);
}
static bool nativeArithCmp(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity;
    const char *op = (const char *)ctx;
    Num x = evalArith(pl, a[0]), y = evalArith(pl, a[1]);
    double xf = numAsF(x), yf = numAsF(y);
    if (streq(op, "<")) return xf < yf;
    if (streq(op, ">")) return xf > yf;
    if (streq(op, "=<")) return xf <= yf;
    if (streq(op, ">=")) return xf >= yf;
    if (streq(op, "=:=")) return xf == yf;
    return xf != yf; // "=\\="
}

static bool doAssert(Prolog *pl, Term *clauseTerm, bool atEnd) {
    Term *t = deref(clauseTerm);
    Term *head, *body = NULL;
    if (t->tag == T_CMP && t->u.cmp.arity == 2 && t->u.cmp.functor == pl->atomColonDash) {
        head = t->u.cmp.args[0]; body = t->u.cmp.args[1];
    } else head = t;
    VarMap map = { 0 };
    Term *h2 = copyTermRec(&pl->program, head, &map);
    Term *b2 = body ? copyTermRec(&pl->program, body, &map) : NULL;
    freeVarMap(&map);
    addClause(pl, h2, b2, atEnd);
    return true;
}
static bool nativeAssertz(Prolog *pl, Term **a, int arity, void *ctx) { (void)arity; (void)ctx; return doAssert(pl, a[0], true); }
static bool nativeAsserta(Prolog *pl, Term **a, int arity, void *ctx) { (void)arity; (void)ctx; return doAssert(pl, a[0], false); }

static bool nativeRetract(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *t = deref(a[0]);
    Term *headPat, *bodyPat;
    if (t->tag == T_CMP && t->u.cmp.arity == 2 && t->u.cmp.functor == pl->atomColonDash) {
        headPat = t->u.cmp.args[0]; bodyPat = t->u.cmp.args[1];
    } else { headPat = t; bodyPat = mkAtomRaw(&pl->query, pl->atomTrue); }
    Term *hd = deref(headPat);
    Atom functor; int ar;
    if (hd->tag == T_ATOM) { functor = hd->u.atom; ar = 0; }
    else if (hd->tag == T_CMP) { functor = hd->u.cmp.functor; ar = hd->u.cmp.arity; }
    else return false;
    Predicate *pred = findPred(pl, functor, ar);
    if (!pred) return false;
    Clause *prev = NULL;
    for (Clause *c = pred->first; c; prev = c, c = c->next) {
        size_t tmark = pl->trail.top;
        VarMap map = { 0 };
        Term *ch = copyTermRec(&pl->query, c->head, &map);
        Term *cb = c->body ? copyTermRec(&pl->query, c->body, &map) : mkAtomRaw(&pl->query, pl->atomTrue);
        freeVarMap(&map);
        if (unify(pl, headPat, ch) && unify(pl, bodyPat, cb)) {
            if (prev) prev->next = c->next; else pred->first = c->next;
            if (pred->last == c) pred->last = prev;
            pl->retractsSinceCompact++;
            return true;
        }
        undoTrailTo(pl, tmark);
    }
    return false;
}

// retract() above only unlinks a Clause node from its predicate's list --
// `program` is a bump allocator with no free-list, so the node's arena bytes
// stay allocated. Every "retract this singleton fact, assertz its
// replacement" update (src/*.pl's mutable-state idiom: undo history,
// goal-column tracking, search state, buffer-local variables) would
// otherwise leak permanently over a long session. This is a copying
// compaction: walk every predicate's still-live clauses and copy them into
// a fresh arena, then discard the old one. Only called from prologReset,
// and only when markStackTop == 0 (no query in flight) -- solve()'s clause
// loop holds a live `Clause *c` into `program` for the entire nested C call
// stack of whatever it's currently solving, so compacting mid-query would
// leave that dangling; between queries, by the mark/reset contract, nothing
// anywhere holds a pointer into `program`'s old blocks.
static void compactProgram(Prolog *pl) {
    Arena fresh = { 0 };
    for (size_t i = 0; i < pl->preds.count; i++) {
        Predicate *pred = pl->preds.items[i];
        Clause *newFirst = NULL, *newLast = NULL;
        for (Clause *c = pred->first; c; c = c->next) {
            VarMap map = { 0 };
            Term *h2 = copyTermRec(&fresh, c->head, &map);
            Term *b2 = c->body ? copyTermRec(&fresh, c->body, &map) : NULL;
            freeVarMap(&map);
            Clause *nc = arenaAlloc(&fresh, sizeof(Clause));
            nc->head = h2; nc->body = b2; nc->next = NULL;
            if (!newFirst) newFirst = newLast = nc;
            else { newLast->next = nc; newLast = nc; }
        }
        pred->first = newFirst;
        pred->last = newLast;
    }
    arenaFreeAll(&pl->program);
    pl->program = fresh;
    pl->retractsSinceCompact = 0;
}

typedef struct { Term *templateTerm; Term **items; size_t count, cap; } FindallCtx;
static bool findallCollect(Prolog *pl, void *ctx) {
    FindallCtx *fc = ctx;
    if (fc->count == fc->cap) { fc->cap = fc->cap ? fc->cap * 2 : 8; fc->items = realloc(fc->items, fc->cap * sizeof(Term *)); }
    fc->items[fc->count++] = copyTermFresh(&pl->query, fc->templateTerm);
    return false; // keep searching -- findall wants every solution
}
static bool nativeFindall(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    FindallCtx fc = { a[0], NULL, 0, 0 };
    solveOpaque(pl, a[1], findallCollect, &fc);
    Term *list = mkAtomRaw(&pl->query, pl->atomNil);
    for (size_t i = fc.count; i > 0; i--) {
        Term **args2 = allocArgs(&pl->query, 2);
        args2[0] = fc.items[i - 1]; args2[1] = list;
        list = mkCompoundRaw(&pl->query, pl->atomDot, 2, args2);
    }
    free(fc.items);
    return unify(pl, a[2], list);
}

// atom_concat/3: construct-only (both inputs bound) -- ISO also allows a
// nondeterministic decompose mode (Atom3 bound, Atom1/Atom2 unbound,
// backtracking over every split); not supported here, noted as a deviation.
static bool nativeAtomConcat(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *x, *y; size_t xl, yl;
    Term *A0 = deref(a[0]), *A1 = deref(a[1]);
    if (A0->tag == T_VAR || A1->tag == T_VAR) throwInstantiationError(pl);
    if (!getTextFlexible(pl, A0, &x, &xl)) throwTypeError(pl, "atomic", A0);
    if (!getTextFlexible(pl, A1, &y, &yl)) throwTypeError(pl, "atomic", A1);
    char *buf = malloc(xl + yl + 1);
    memcpy(buf, x, xl); memcpy(buf + xl, y, yl); buf[xl + yl] = 0;
    Term *r = mkAtomRaw(&pl->query, internAtom(pl, buf));
    free(buf);
    return unify(pl, a[2], r);
}
static bool nativeAtomLength(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *A = deref(a[0]);
    if (A->tag == T_VAR) throwInstantiationError(pl);
    const char *s; size_t len;
    if (!getTextFlexible(pl, A, &s, &len)) throwTypeError(pl, "atomic", A);
    return unify(pl, a[1], mkIntRaw(&pl->query, (long)len));
}
static bool parseNum(const char *txt, size_t len, Num *out) {
    char buf[64]; size_t n = len < 63 ? len : 63;
    memcpy(buf, txt, n); buf[n] = 0;
    char *end;
    long iv = strtol(buf, &end, 10);
    if (*end == 0 && end != buf) { out->isFloat = false; out->i = iv; return true; }
    double fv = strtod(buf, &end);
    if (*end == 0 && end != buf) { out->isFloat = true; out->f = fv; return true; }
    return false;
}
static bool nativeAtomNumber(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *A = deref(a[0]);
    if (A->tag != T_VAR) {
        const char *txt; size_t len;
        if (!getTextFlexible(pl, A, &txt, &len)) throwTypeError(pl, "atomic", A);
        Num n;
        if (!parseNum(txt, len, &n)) return false;
        return unify(pl, a[1], n.isFloat ? mkFloatRaw(&pl->query, n.f) : mkIntRaw(&pl->query, n.i));
    }
    Term *N = deref(a[1]);
    char buf[64];
    if (N->tag == T_INT) snprintf(buf, sizeof buf, "%ld", N->u.i);
    else if (N->tag == T_FLT) snprintf(buf, sizeof buf, "%g", N->u.f);
    else throwInstantiationError(pl);
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, buf)));
}
static bool nativeAtomCodes(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *A = deref(a[0]);
    if (A->tag != T_VAR) {
        const char *s; size_t len;
        if (!getTextFlexible(pl, A, &s, &len)) throwTypeError(pl, "atomic", A);
        return unify(pl, a[1], mkCodeList(pl, s, len));
    }
    const char *s; size_t len;
    if (!getTextFlexible(pl, a[1], &s, &len)) throwInstantiationError(pl);
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, s)));
}
static bool nativeAtomChars(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *A = deref(a[0]);
    if (A->tag != T_VAR) {
        const char *s; size_t len;
        if (!getTextFlexible(pl, A, &s, &len)) throwTypeError(pl, "atomic", A);
        return unify(pl, a[1], mkCharList(pl, s, len));
    }
    char buf[256]; size_t len;
    if (!getCharListText(pl, a[1], buf, sizeof buf, &len)) throwInstantiationError(pl);
    char nbuf[257]; memcpy(nbuf, buf, len); nbuf[len] = 0;
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, nbuf)));
}
static bool nativeCharCode(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *C = deref(a[0]);
    if (C->tag == T_ATOM) {
        const char *n = atomName(pl, C->u.atom);
        if (strlen(n) != 1) throwTypeError(pl, "character", C);
        return unify(pl, a[1], mkIntRaw(&pl->query, (unsigned char)n[0]));
    }
    Term *Code = deref(a[1]);
    if (Code->tag != T_INT) throwInstantiationError(pl);
    char buf[2] = { (char)Code->u.i, 0 };
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, buf)));
}
static bool nativeNumberCodes(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *N = deref(a[0]);
    if (N->tag == T_INT || N->tag == T_FLT) {
        char buf[64];
        if (N->tag == T_INT) snprintf(buf, sizeof buf, "%ld", N->u.i);
        else snprintf(buf, sizeof buf, "%g", N->u.f);
        return unify(pl, a[1], mkCodeList(pl, buf, strlen(buf)));
    }
    const char *s; size_t len;
    if (!getTextFlexible(pl, a[1], &s, &len)) throwInstantiationError(pl);
    Num n;
    if (!parseNum(s, len, &n)) throwTypeError(pl, "number", a[1]);
    return unify(pl, a[0], n.isFloat ? mkFloatRaw(&pl->query, n.f) : mkIntRaw(&pl->query, n.i));
}
static bool nativeNumberChars(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *N = deref(a[0]);
    if (N->tag == T_INT || N->tag == T_FLT) {
        char buf[64];
        if (N->tag == T_INT) snprintf(buf, sizeof buf, "%ld", N->u.i);
        else snprintf(buf, sizeof buf, "%g", N->u.f);
        return unify(pl, a[1], mkCharList(pl, buf, strlen(buf)));
    }
    char buf[64]; size_t len;
    if (!getCharListText(pl, a[1], buf, sizeof buf, &len)) throwInstantiationError(pl);
    Num n;
    if (!parseNum(buf, len, &n)) throwTypeError(pl, "number", a[1]);
    return unify(pl, a[0], n.isFloat ? mkFloatRaw(&pl->query, n.f) : mkIntRaw(&pl->query, n.i));
}
static bool nativeLength(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *L = deref(a[0]);
    if (L->tag != T_VAR) {
        Term **items; size_t count;
        if (!collectList(pl, L, &items, &count)) throwTypeError(pl, "list", L);
        free(items);
        return unify(pl, a[1], mkIntRaw(&pl->query, (long)count));
    }
    Term *N = deref(a[1]);
    if (N->tag != T_INT || N->u.i < 0) throwTypeError(pl, "integer", N);
    Term *list = mkAtomRaw(&pl->query, pl->atomNil);
    for (long i = 0; i < N->u.i; i++) {
        Term **args = allocArgs(&pl->query, 2);
        args[0] = newVar(&pl->query); args[1] = list;
        list = mkCompoundRaw(&pl->query, pl->atomDot, 2, args);
    }
    return unify(pl, a[0], list);
}
static bool nativeFunctor(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *T = deref(a[0]);
    if (T->tag != T_VAR) {
        Term *name; long ar;
        if (T->tag == T_CMP) { name = mkAtomRaw(&pl->query, T->u.cmp.functor); ar = T->u.cmp.arity; }
        else { name = T; ar = 0; }
        return unify(pl, a[1], name) && unify(pl, a[2], mkIntRaw(&pl->query, ar));
    }
    Term *Name = deref(a[1]), *ArT = deref(a[2]);
    if (Name->tag == T_VAR || ArT->tag == T_VAR) throwInstantiationError(pl);
    if (ArT->tag != T_INT) throwTypeError(pl, "integer", ArT);
    if (ArT->u.i == 0) return unify(pl, a[0], Name);
    if (Name->tag != T_ATOM) throwTypeError(pl, "atom", Name);
    Term **args = allocArgs(&pl->query, (int)ArT->u.i);
    for (long i = 0; i < ArT->u.i; i++) args[i] = newVar(&pl->query);
    return unify(pl, a[0], mkCompoundRaw(&pl->query, Name->u.atom, (int)ArT->u.i, args));
}
static bool nativeUniv(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *T = deref(a[0]);
    if (T->tag != T_VAR) {
        Term *list;
        if (T->tag == T_CMP) {
            list = mkAtomRaw(&pl->query, pl->atomNil);
            for (int i = T->u.cmp.arity; i >= 1; i--) {
                Term **cargs = allocArgs(&pl->query, 2);
                cargs[0] = T->u.cmp.args[i - 1]; cargs[1] = list;
                list = mkCompoundRaw(&pl->query, pl->atomDot, 2, cargs);
            }
            Term **cargs = allocArgs(&pl->query, 2);
            cargs[0] = mkAtomRaw(&pl->query, T->u.cmp.functor); cargs[1] = list;
            list = mkCompoundRaw(&pl->query, pl->atomDot, 2, cargs);
        } else {
            Term **cargs = allocArgs(&pl->query, 2);
            cargs[0] = T; cargs[1] = mkAtomRaw(&pl->query, pl->atomNil);
            list = mkCompoundRaw(&pl->query, pl->atomDot, 2, cargs);
        }
        return unify(pl, a[1], list);
    }
    Term *L = deref(a[1]);
    if (!(L->tag == T_CMP && L->u.cmp.functor == pl->atomDot && L->u.cmp.arity == 2)) throwInstantiationError(pl);
    Term *head = deref(L->u.cmp.args[0]);
    Term *rest = deref(L->u.cmp.args[1]);
    if (rest->tag == T_ATOM && rest->u.atom == pl->atomNil) return unify(pl, a[0], head);
    if (head->tag != T_ATOM) throwTypeError(pl, "atom", head);
    Term **items; size_t count;
    if (!collectList(pl, rest, &items, &count)) throwTypeError(pl, "list", rest);
    Term **cargs = allocArgs(&pl->query, (int)count);
    memcpy(cargs, items, count * sizeof(Term *));
    free(items);
    return unify(pl, a[0], mkCompoundRaw(&pl->query, head->u.atom, (int)count, cargs));
}
static bool nativeArg(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *N = deref(a[0]), *T = deref(a[1]);
    if (N->tag == T_VAR || T->tag == T_VAR) throwInstantiationError(pl);
    if (N->tag != T_INT) throwTypeError(pl, "integer", N);
    if (T->tag != T_CMP) throwTypeError(pl, "compound", T);
    if (N->u.i < 1 || N->u.i > T->u.cmp.arity) return false;
    return unify(pl, a[2], T->u.cmp.args[N->u.i - 1]);
}
static bool nativeCopyTerm(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    return unify(pl, a[1], copyTermFresh(&pl->query, a[0]));
}

// --- ISO type-checking predicates -----------------------------------------
// Building blocks for Prolog-level library definitions (see src/bootstrap.pl)
// as much as user-facing predicates -- e.g. succ/2 is written in Prolog on
// top of var/1 and integer/1 rather than as a native.
static bool nativeVar(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return deref(a[0])->tag == T_VAR; }
static bool nativeNonvar(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return deref(a[0])->tag != T_VAR; }
static bool nativeAtomCheck(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return deref(a[0])->tag == T_ATOM; }
static bool nativeAtomicCheck(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)pl; (void)arity; (void)ctx;
    Tag t = deref(a[0])->tag;
    return t == T_ATOM || t == T_INT || t == T_FLT;
}
static bool nativeNumberCheck(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)pl; (void)arity; (void)ctx;
    Tag t = deref(a[0])->tag;
    return t == T_INT || t == T_FLT;
}
static bool nativeIntegerCheck(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return deref(a[0])->tag == T_INT; }
static bool nativeFloatCheck(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return deref(a[0])->tag == T_FLT; }
static bool nativeCompoundCheck(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return deref(a[0])->tag == T_CMP; }
static bool nativeCallableCheck(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)pl; (void)arity; (void)ctx;
    Tag t = deref(a[0])->tag;
    return t == T_ATOM || t == T_CMP;
}
static bool isProperList(Prolog *pl, Term *t) {
    t = deref(t);
    while (t->tag == T_CMP && t->u.cmp.functor == pl->atomDot && t->u.cmp.arity == 2) t = deref(t->u.cmp.args[1]);
    return t->tag == T_ATOM && t->u.atom == pl->atomNil;
}
static bool nativeIsList(Prolog *pl, Term **a, int arity, void *ctx) { (void)arity; (void)ctx; return isProperList(pl, a[0]); }
static bool isGround(Term *t) {
    t = deref(t);
    if (t->tag == T_VAR) return false;
    if (t->tag == T_CMP) for (int i = 0; i < t->u.cmp.arity; i++) if (!isGround(t->u.cmp.args[i])) return false;
    return true;
}
static bool nativeGround(Prolog *pl, Term **a, int arity, void *ctx) { (void)pl; (void)arity; (void)ctx; return isGround(a[0]); }

// --- standard order of terms: Var @< Number @< Atom @< Compound ----------

static int termOrder(Prolog *pl, Term *a, Term *b) {
    a = deref(a); b = deref(b);
    int ra = a->tag == T_VAR ? 0 : (a->tag == T_INT || a->tag == T_FLT) ? 1 : a->tag == T_ATOM ? 2 : 3;
    int rb = b->tag == T_VAR ? 0 : (b->tag == T_INT || b->tag == T_FLT) ? 1 : b->tag == T_ATOM ? 2 : 3;
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (ra) {
    case 0: return a == b ? 0 : (a < b ? -1 : 1); // distinct vars: stable pointer order
    case 1: {
        double av = a->tag == T_FLT ? a->u.f : (double)a->u.i;
        double bv = b->tag == T_FLT ? b->u.f : (double)b->u.i;
        if (av < bv) return -1;
        if (av > bv) return 1;
        bool af = a->tag == T_FLT, bf = b->tag == T_FLT;
        return af == bf ? 0 : (af ? -1 : 1); // equal value: float @< int, per ISO
    }
    case 2: {
        int c = strcmp(atomName(pl, a->u.atom), atomName(pl, b->u.atom));
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    default: {
        if (a->u.cmp.arity != b->u.cmp.arity) return a->u.cmp.arity < b->u.cmp.arity ? -1 : 1;
        int c = strcmp(atomName(pl, a->u.cmp.functor), atomName(pl, b->u.cmp.functor));
        if (c != 0) return c < 0 ? -1 : 1;
        for (int i = 0; i < a->u.cmp.arity; i++) {
            int r = termOrder(pl, a->u.cmp.args[i], b->u.cmp.args[i]);
            if (r != 0) return r;
        }
        return 0;
    }
    }
}
static bool nativeCompare(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    int c = termOrder(pl, a[1], a[2]);
    const char *sym = c < 0 ? "<" : c > 0 ? ">" : "=";
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, sym)));
}
static bool nativeOrderCmp(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity;
    const char *op = (const char *)ctx;
    int c = termOrder(pl, a[0], a[1]);
    if (streq(op, "@<")) return c < 0;
    if (streq(op, "@=<")) return c <= 0;
    if (streq(op, "@>")) return c > 0;
    return c >= 0; // "@>="
}
// Insertion sort (config-scale lists -- O(n^2) is plenty fast here, and it
// keeps termOrder's Prolog* in scope without qsort's non-reentrant
// comparator signature).
static void sortTerms(Prolog *pl, Term **items, size_t n) {
    for (size_t i = 1; i < n; i++) {
        Term *key = items[i];
        size_t j = i;
        while (j > 0 && termOrder(pl, items[j - 1], key) > 0) { items[j] = items[j - 1]; j--; }
        items[j] = key;
    }
}
static bool nativeMsort(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term **items; size_t count;
    if (!collectList(pl, a[0], &items, &count)) throwTypeError(pl, "list", a[0]);
    sortTerms(pl, items, count);
    Term *r = buildList(pl, items, count);
    free(items);
    return unify(pl, a[1], r);
}
static bool nativeSort(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term **items; size_t count;
    if (!collectList(pl, a[0], &items, &count)) throwTypeError(pl, "list", a[0]);
    sortTerms(pl, items, count);
    size_t w = 0;
    for (size_t i = 0; i < count; i++)
        if (w == 0 || termOrder(pl, items[w - 1], items[i]) != 0) items[w++] = items[i];
    Term *r = buildList(pl, items, w);
    free(items);
    return unify(pl, a[1], r);
}

static bool nativeGetTime(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    return unify(pl, a[0], mkFloatRaw(&pl->query, (double)time(NULL)));
}
// Not ISO -- no standard predicate for wall-clock time exists -- but kept as
// a clearly-flagged, necessary extension (see get_time/1): dropping
// date-stamping entirely would gut docs/init.pl.example's usefulness.
static bool nativeFormatTime(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *fmt; size_t fmtLen;
    if (!getTextFlexible(pl, a[1], &fmt, &fmtLen)) throwInstantiationError(pl);
    Term *ts = deref(a[2]);
    time_t tt;
    if (ts->tag == T_INT) tt = (time_t)ts->u.i;
    else if (ts->tag == T_FLT) tt = (time_t)ts->u.f;
    else throwTypeError(pl, "number", ts);
    char fmtBuf[128]; size_t fl = fmtLen < 127 ? fmtLen : 127;
    memcpy(fmtBuf, fmt, fl); fmtBuf[fl] = 0;
    struct tm tmv = *localtime(&tt); // te is single-threaded; localtime's static buffer is fine here
    char out[256];
    size_t n = strftime(out, sizeof out, fmtBuf, &tmv);
    return unify(pl, a[0], mkCodeList(pl, out, n));
}

static void registerStdlib(Prolog *pl) {
    prologRegisterNative(pl, "=", 2, nativeUnify, NULL);
    prologRegisterNative(pl, "\\=", 2, nativeNotUnify, NULL);
    prologRegisterNative(pl, "==", 2, nativeEq, NULL);
    prologRegisterNative(pl, "\\==", 2, nativeNeq, NULL);
    prologRegisterNative(pl, "is", 2, nativeIs, NULL);
    prologRegisterNative(pl, "<", 2, nativeArithCmp, "<");
    prologRegisterNative(pl, ">", 2, nativeArithCmp, ">");
    prologRegisterNative(pl, "=<", 2, nativeArithCmp, "=<");
    prologRegisterNative(pl, ">=", 2, nativeArithCmp, ">=");
    prologRegisterNative(pl, "=:=", 2, nativeArithCmp, "=:=");
    prologRegisterNative(pl, "=\\=", 2, nativeArithCmp, "=\\=");
    prologRegisterNative(pl, "assertz", 1, nativeAssertz, NULL);
    prologRegisterNative(pl, "asserta", 1, nativeAsserta, NULL);
    prologRegisterNative(pl, "retract", 1, nativeRetract, NULL);
    prologRegisterNative(pl, "findall", 3, nativeFindall, NULL);
    prologRegisterNative(pl, "atom_concat", 3, nativeAtomConcat, NULL);
    prologRegisterNative(pl, "atom_length", 2, nativeAtomLength, NULL);
    prologRegisterNative(pl, "atom_number", 2, nativeAtomNumber, NULL);
    prologRegisterNative(pl, "atom_codes", 2, nativeAtomCodes, NULL);
    prologRegisterNative(pl, "atom_chars", 2, nativeAtomChars, NULL);
    prologRegisterNative(pl, "char_code", 2, nativeCharCode, NULL);
    prologRegisterNative(pl, "number_codes", 2, nativeNumberCodes, NULL);
    prologRegisterNative(pl, "number_chars", 2, nativeNumberChars, NULL);
    prologRegisterNative(pl, "length", 2, nativeLength, NULL);
    prologRegisterNative(pl, "functor", 3, nativeFunctor, NULL);
    prologRegisterNative(pl, "=..", 2, nativeUniv, NULL);
    prologRegisterNative(pl, "arg", 3, nativeArg, NULL);
    prologRegisterNative(pl, "copy_term", 2, nativeCopyTerm, NULL);
    prologRegisterNative(pl, "var", 1, nativeVar, NULL);
    prologRegisterNative(pl, "nonvar", 1, nativeNonvar, NULL);
    prologRegisterNative(pl, "atom", 1, nativeAtomCheck, NULL);
    prologRegisterNative(pl, "atomic", 1, nativeAtomicCheck, NULL);
    prologRegisterNative(pl, "number", 1, nativeNumberCheck, NULL);
    prologRegisterNative(pl, "integer", 1, nativeIntegerCheck, NULL);
    prologRegisterNative(pl, "float", 1, nativeFloatCheck, NULL);
    prologRegisterNative(pl, "compound", 1, nativeCompoundCheck, NULL);
    prologRegisterNative(pl, "callable", 1, nativeCallableCheck, NULL);
    prologRegisterNative(pl, "is_list", 1, nativeIsList, NULL);
    prologRegisterNative(pl, "ground", 1, nativeGround, NULL);
    prologRegisterNative(pl, "compare", 3, nativeCompare, NULL);
    prologRegisterNative(pl, "@<", 2, nativeOrderCmp, "@<");
    prologRegisterNative(pl, "@=<", 2, nativeOrderCmp, "@=<");
    prologRegisterNative(pl, "@>", 2, nativeOrderCmp, "@>");
    prologRegisterNative(pl, "@>=", 2, nativeOrderCmp, "@>=");
    prologRegisterNative(pl, "sort", 2, nativeSort, NULL);
    prologRegisterNative(pl, "msort", 2, nativeMsort, NULL);
    prologRegisterNative(pl, "get_time", 1, nativeGetTime, NULL);
    prologRegisterNative(pl, "format_time", 3, nativeFormatTime, NULL);
}

// --- tokenizer --------------------------------------------------------

typedef enum { TK_EOF, TK_ATOM, TK_VAR, TK_INT, TK_FLOAT, TK_STRING, TK_PUNCT } TokKind;
typedef struct {
    TokKind kind;
    char text[512];
    size_t textLen;
    long ival;
    double fval;
    bool precededBySpace;
} Token;

typedef struct { char *name; Term *term; } VarBinding;

typedef struct {
    Prolog *pl;
    const char *src; size_t pos, len;
    Token cur;
    Arena *target;
    VarBinding *vars; size_t varCount, varCap;
    bool hadError;
} Parser;

static void initParser(Parser *p, Prolog *pl, const char *src, size_t len, Arena *target) {
    memset(p, 0, sizeof *p);
    p->pl = pl; p->src = src; p->len = len; p->target = target;
}
static void resetVarScope(Parser *p) {
    for (size_t i = 0; i < p->varCount; i++) free(p->vars[i].name);
    p->varCount = 0;
}
static void destroyParser(Parser *p) {
    resetVarScope(p);
    free(p->vars);
}

static bool skipTriviaMarkSpace(Parser *p) {
    size_t start = p->pos;
    for (;;) {
        while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
        if (p->pos < p->len && p->src[p->pos] == '%') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
            continue;
        }
        if (p->pos + 1 < p->len && p->src[p->pos] == '/' && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) p->pos++;
            if (p->pos + 1 < p->len) p->pos += 2; else p->pos = p->len;
            continue;
        }
        break;
    }
    return p->pos != start;
}

static void parseNumber(Parser *p, Token *tok) {
    size_t start = p->pos;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    bool isFloat = false;
    if (p->pos + 1 < p->len && p->src[p->pos] == '.' && isdigit((unsigned char)p->src[p->pos + 1])) {
        isFloat = true; p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        size_t save = p->pos, q = p->pos + 1;
        if (q < p->len && (p->src[q] == '+' || p->src[q] == '-')) q++;
        if (q < p->len && isdigit((unsigned char)p->src[q])) {
            isFloat = true; p->pos = q;
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        } else p->pos = save;
    }
    size_t len = p->pos - start;
    char buf[64]; size_t n = len < 63 ? len : 63;
    memcpy(buf, p->src + start, n); buf[n] = 0;
    if (isFloat) { tok->kind = TK_FLOAT; tok->fval = strtod(buf, NULL); }
    else { tok->kind = TK_INT; tok->ival = strtol(buf, NULL, 10); }
}
static void parseName(Parser *p, Token *tok) {
    size_t start = p->pos;
    while (p->pos < p->len && (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_')) p->pos++;
    size_t len = p->pos - start;
    size_t n = len < sizeof tok->text - 1 ? len : sizeof tok->text - 1;
    memcpy(tok->text, p->src + start, n); tok->text[n] = 0; tok->textLen = n;
}
static void parseQuoted(Parser *p, Token *tok, char q) {
    p->pos++; // opening quote
    size_t o = 0;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == q) {
            if (p->pos + 1 < p->len && p->src[p->pos + 1] == q) {
                if (o < sizeof tok->text - 1) tok->text[o++] = q;
                p->pos += 2; continue;
            }
            p->pos++; break;
        }
        if (c == '\\' && p->pos + 1 < p->len) {
            char e = p->src[p->pos + 1];
            char actual = e == 'n' ? '\n' : e == 't' ? '\t' : e == '\\' ? '\\' : e == '\'' ? '\'' : e == '"' ? '"' : e;
            if (o < sizeof tok->text - 1) tok->text[o++] = actual;
            p->pos += 2; continue;
        }
        if (o < sizeof tok->text - 1) tok->text[o++] = c;
        p->pos++;
    }
    tok->text[o] = 0; tok->textLen = o;
}
static const char *const MULTI_PUNCT[] = {
    ":-", "->", "\\+", "\\==", "\\=", "==", "=<", ">=", "=:=", "=\\=", "//", "=..",
    "@=<", "@>=", "@<", "@>", NULL
};
static void parsePunctOrSymbolic(Parser *p, Token *tok) {
    for (int i = 0; MULTI_PUNCT[i]; i++) {
        size_t l = strlen(MULTI_PUNCT[i]);
        if (p->pos + l <= p->len && memcmp(p->src + p->pos, MULTI_PUNCT[i], l) == 0) {
            memcpy(tok->text, MULTI_PUNCT[i], l + 1); tok->textLen = l;
            p->pos += l; tok->kind = TK_PUNCT; return;
        }
    }
    tok->text[0] = p->src[p->pos]; tok->text[1] = 0; tok->textLen = 1;
    p->pos++;
    tok->kind = TK_PUNCT;
}
static void advance(Parser *p) {
    bool space = skipTriviaMarkSpace(p);
    Token tok; memset(&tok, 0, sizeof tok);
    tok.precededBySpace = space;
    if (p->pos >= p->len) { tok.kind = TK_EOF; p->cur = tok; return; }
    unsigned char c = (unsigned char)p->src[p->pos];
    if (c == '\'') { parseQuoted(p, &tok, '\''); tok.kind = TK_ATOM; p->cur = tok; return; }
    if (c == '"') { parseQuoted(p, &tok, '"'); tok.kind = TK_STRING; p->cur = tok; return; }
    if (isdigit(c)) { parseNumber(p, &tok); p->cur = tok; return; }
    if (c == '_' || isupper(c)) { parseName(p, &tok); tok.kind = TK_VAR; p->cur = tok; return; }
    if (islower(c)) { parseName(p, &tok); tok.kind = TK_ATOM; p->cur = tok; return; }
    parsePunctOrSymbolic(p, &tok);
    p->cur = tok;
}

static bool curIs(Parser *p, TokKind k, const char *text) {
    return p->cur.kind == k && streq(p->cur.text, text);
}
static void reportPlainErrorParser(Parser *p, const char *msg) {
    reportPlainError(p->pl, msg);
    p->hadError = true;
}

static Term *lookupOrCreateVar(Parser *p, const char *name, size_t len) {
    if (len == 1 && name[0] == '_') return newVar(p->target);
    for (size_t i = 0; i < p->varCount; i++)
        if (strlen(p->vars[i].name) == len && memcmp(p->vars[i].name, name, len) == 0) return p->vars[i].term;
    if (p->varCount == p->varCap) {
        p->varCap = p->varCap ? p->varCap * 2 : 8;
        p->vars = realloc(p->vars, p->varCap * sizeof(VarBinding));
    }
    char *ncopy = malloc(len + 1); memcpy(ncopy, name, len); ncopy[len] = 0;
    Term *v = newVar(p->target);
    p->vars[p->varCount++] = (VarBinding){ ncopy, v };
    return v;
}

// --- operator-precedence parser (practical subset, fixed operator table) -

typedef struct { const char *name; int prec; bool rightAssoc; } OpDef;
static const OpDef INFIX_OPS[] = {
    { ":-", 1200, false },
    { ";", 1100, true },
    { "->", 1050, true },
    { ",", 1000, true },
    { "=", 700, false }, { "\\=", 700, false }, { "==", 700, false }, { "\\==", 700, false },
    { "is", 700, false }, { "<", 700, false }, { ">", 700, false }, { "=<", 700, false }, { ">=", 700, false },
    { "=:=", 700, false }, { "=\\=", 700, false }, { "=..", 700, false },
    { "@<", 700, false }, { "@=<", 700, false }, { "@>", 700, false }, { "@>=", 700, false },
    { "+", 500, false }, { "-", 500, false },
    { "*", 400, false }, { "/", 400, false }, { "//", 400, false }, { "mod", 400, false },
};
static const OpDef *peekInfixOp(Parser *p) {
    if (p->cur.kind != TK_PUNCT && p->cur.kind != TK_ATOM) return NULL;
    for (size_t i = 0; i < sizeof(INFIX_OPS) / sizeof(INFIX_OPS[0]); i++)
        if (streq(p->cur.text, INFIX_OPS[i].name)) return &INFIX_OPS[i];
    return NULL;
}

static Term *parseExpr(Parser *p, int maxPrec);

static Term *parseList(Parser *p) {
    if (curIs(p, TK_PUNCT, "]")) { advance(p); return mkAtomRaw(p->target, p->pl->atomNil); }
    Term **items = NULL; size_t count = 0, cap = 0;
    for (;;) {
        Term *e = parseExpr(p, 999);
        if (count == cap) { cap = cap ? cap * 2 : 8; items = realloc(items, cap * sizeof(Term *)); }
        items[count++] = e;
        if (curIs(p, TK_PUNCT, ",")) { advance(p); continue; }
        break;
    }
    Term *tail;
    if (curIs(p, TK_PUNCT, "|")) { advance(p); tail = parseExpr(p, 999); }
    else tail = mkAtomRaw(p->target, p->pl->atomNil);
    if (!curIs(p, TK_PUNCT, "]")) reportPlainErrorParser(p, "expected ']'");
    else advance(p);
    Term *list = tail;
    for (size_t i = count; i > 0; i--) {
        Term **cargs = allocArgs(p->target, 2);
        cargs[0] = items[i - 1]; cargs[1] = list;
        list = mkCompoundRaw(p->target, p->pl->atomDot, 2, cargs);
    }
    free(items);
    return list;
}

// Symbolic operator atoms usable standalone as a term (not just infix) --
// e.g. compare/3's '<'/'='/'>' results, or `X = (+)`. Structural delimiters
// (, ; | . ( ) [ ]) are deliberately excluded -- still reserved punctuation.
static bool isBareableSymbol(const char *s) {
    static const char *const SYMS[] = {
        ":-", "->", "\\+", "\\==", "\\=", "==", "=<", ">=", "=:=", "=\\=", "//", "=..",
        "@=<", "@>=", "@<", "@>", "=", "<", ">", "+", "-", "*", "/", NULL
    };
    for (int i = 0; SYMS[i]; i++) if (streq(s, SYMS[i])) return true;
    return false;
}
static Term *parsePrimary(Parser *p) {
    Token t = p->cur;
    switch (t.kind) {
    case TK_INT: advance(p); return mkIntRaw(p->target, t.ival);
    case TK_FLOAT: advance(p); return mkFloatRaw(p->target, t.fval);
    case TK_STRING: advance(p); return mkCodeList(p->pl, t.text, t.textLen);
    case TK_VAR: advance(p); return lookupOrCreateVar(p, t.text, t.textLen);
    case TK_ATOM: {
        char name[512]; memcpy(name, t.text, t.textLen + 1);
        advance(p);
        if (p->cur.kind == TK_PUNCT && streq(p->cur.text, "(") && !p->cur.precededBySpace) {
            advance(p); // consume '('
            Term **args = NULL; size_t count = 0, cap = 0;
            for (;;) {
                Term *a = parseExpr(p, 999);
                if (count == cap) { cap = cap ? cap * 2 : 8; args = realloc(args, cap * sizeof(Term *)); }
                args[count++] = a;
                if (curIs(p, TK_PUNCT, ",")) { advance(p); continue; }
                break;
            }
            if (!curIs(p, TK_PUNCT, ")")) reportPlainErrorParser(p, "expected ')'");
            else advance(p);
            Term **cargs = allocArgs(p->target, (int)count);
            memcpy(cargs, args, count * sizeof(Term *));
            free(args);
            return mkCompoundRaw(p->target, internAtom(p->pl, name), (int)count, cargs);
        }
        return mkAtomRaw(p->target, internAtom(p->pl, name));
    }
    case TK_PUNCT:
        if (streq(t.text, "(")) {
            advance(p);
            Term *inner = parseExpr(p, 1200);
            if (!curIs(p, TK_PUNCT, ")")) reportPlainErrorParser(p, "expected ')'");
            else advance(p);
            return inner;
        }
        if (streq(t.text, "[")) { advance(p); return parseList(p); }
        if (streq(t.text, "!")) { advance(p); return mkAtomRaw(p->target, p->pl->atomCut); }
        if (isBareableSymbol(t.text)) { advance(p); return mkAtomRaw(p->target, internAtom(p->pl, t.text)); }
        reportPlainErrorParser(p, "unexpected token");
        advance(p);
        return mkAtomRaw(p->target, p->pl->atomFail);
    case TK_EOF:
    default:
        reportPlainErrorParser(p, "unexpected end of input");
        return mkAtomRaw(p->target, p->pl->atomFail);
    }
}
static Term *parsePrefixOrPrimary(Parser *p, int maxPrec) {
    if (curIs(p, TK_PUNCT, "\\+") && maxPrec >= 900) {
        advance(p);
        Term *arg = parseExpr(p, 900);
        return mkCompound1(p->target, p->pl->atomNaf, arg);
    }
    if (curIs(p, TK_PUNCT, "-") && maxPrec >= 200) {
        advance(p);
        Term *arg = parsePrefixOrPrimary(p, 200);
        if (arg->tag == T_INT) return mkIntRaw(p->target, -arg->u.i);
        if (arg->tag == T_FLT) return mkFloatRaw(p->target, -arg->u.f);
        return mkCompound1(p->target, internAtom(p->pl, "-"), arg);
    }
    return parsePrimary(p);
}
static Term *parseInfixLoop(Parser *p, Term *left, int maxPrec) {
    for (;;) {
        const OpDef *op = peekInfixOp(p);
        if (!op || op->prec > maxPrec) break;
        char opname[16]; size_t n = strlen(op->name); if (n > 15) n = 15;
        memcpy(opname, op->name, n); opname[n] = 0;
        advance(p);
        int rightMax = op->rightAssoc ? op->prec : op->prec - 1;
        Term *right = parseExpr(p, rightMax);
        Term **cargs = allocArgs(p->target, 2);
        cargs[0] = left; cargs[1] = right;
        left = mkCompoundRaw(p->target, internAtom(p->pl, opname), 2, cargs);
    }
    return left;
}
static Term *parseExpr(Parser *p, int maxPrec) {
    Term *left = parsePrefixOrPrimary(p, maxPrec);
    return parseInfixLoop(p, left, maxPrec);
}

// --- clause-level parsing (facts/rules/directives) ------------------------

typedef enum { CL_EOF, CL_FACT, CL_RULE, CL_DIRECTIVE, CL_ERROR } ClauseKind;
typedef struct { ClauseKind kind; Term *head, *body; bool atDotBoundary; } ParsedClause;

// Returns true if it found and consumed the clause-terminating '.' (even if
// hadError was already set by something earlier in the clause) -- when it
// didn't, the cursor is lost mid-expression and needs skipToNextClauseEnd;
// when it did, the cursor already sits at the next clause's first token and
// skipping further would wrongly eat that next (valid) clause too.
static bool expectEndOfClause(Parser *p) {
    if (!curIs(p, TK_PUNCT, ".")) { reportPlainErrorParser(p, "expected '.' at end of clause"); return false; }
    advance(p);
    return true;
}
static void skipToNextClauseEnd(Parser *p) {
    while (p->cur.kind != TK_EOF) {
        if (p->cur.kind == TK_PUNCT && streq(p->cur.text, ".")) { advance(p); return; }
        advance(p);
    }
}
static ParsedClause parseOneClause(Parser *p) {
    if (p->cur.kind == TK_EOF) return (ParsedClause){ .kind = CL_EOF };
    if (curIs(p, TK_PUNCT, ":-")) {
        advance(p);
        Term *goal = parseExpr(p, 1199);
        bool atDot = expectEndOfClause(p);
        if (p->hadError) return (ParsedClause){ .kind = CL_ERROR, .atDotBoundary = atDot };
        return (ParsedClause){ .kind = CL_DIRECTIVE, .head = goal };
    }
    Term *t = parseExpr(p, 1200);
    bool atDot = expectEndOfClause(p);
    if (p->hadError) return (ParsedClause){ .kind = CL_ERROR, .atDotBoundary = atDot };
    if (t->tag == T_CMP && t->u.cmp.functor == p->pl->atomColonDash && t->u.cmp.arity == 2)
        return (ParsedClause){ .kind = CL_RULE, .head = t->u.cmp.args[0], .body = t->u.cmp.args[1] };
    return (ParsedClause){ .kind = CL_FACT, .head = t };
}

// --- public API: lifecycle, consult, mark/reset, term inspection/build ---

Prolog *prologCreate(void) {
    Prolog *pl = calloc(1, sizeof(Prolog));
    pl->atomComma = internAtom(pl, ",");
    pl->atomSemi = internAtom(pl, ";");
    pl->atomArrow = internAtom(pl, "->");
    pl->atomCut = internAtom(pl, "!");
    pl->atomTrue = internAtom(pl, "true");
    pl->atomFail = internAtom(pl, "fail");
    pl->atomFalse = internAtom(pl, "false");
    pl->atomNaf = internAtom(pl, "\\+");
    pl->atomCall = internAtom(pl, "call");
    pl->atomCatch = internAtom(pl, "catch");
    pl->atomThrow = internAtom(pl, "throw");
    pl->atomColonDash = internAtom(pl, ":-");
    pl->atomNil = internAtom(pl, "[]");
    pl->atomDot = internAtom(pl, ".");
    pl->atomError = internAtom(pl, "error");
    registerStdlib(pl);
    // The engine's own standard library (between/3, member/2, maplist/2-4,
    // ...) is written in Prolog, not C -- see src/bootstrap.pl for why and
    // what. Loading it is the caller's job now (script.c's scriptSetup finds
    // and consults src/bootstrap.pl on disk, next to the running executable)
    // -- prologCreate no longer bundles it, so a bare engine with none of
    // that library loaded is a valid (if not very useful) starting point.
    return pl;
}
void prologDestroy(Prolog *pl) {
    if (!pl) return;
    arenaFreeAll(&pl->program);
    arenaFreeAll(&pl->query);
    for (size_t i = 0; i < pl->atoms.count; i++) free(pl->atoms.names[i]);
    free(pl->atoms.names);
    for (size_t i = 0; i < pl->preds.count; i++) free(pl->preds.items[i]);
    free(pl->preds.items);
    free(pl->natives.items);
    free(pl->trail.items);
    free(pl->markStack);
    free(pl);
}
void prologSetErrorHandler(Prolog *pl, PrologErrorFn fn, void *ctx) { pl->errorFn = fn; pl->errorCtx = ctx; }

// Parses and loads clauses/directives from a buffer (used by both
// prologConsultFile and prologCreate's src/bootstrap.pl library load).
static void consultBuffer(Prolog *pl, const char *buf, size_t len) {
    Parser parser;
    initParser(&parser, pl, buf, len, &pl->query);
    advance(&parser);
    for (;;) {
        ArenaMark mark = arenaMark(&pl->query);
        resetVarScope(&parser);
        parser.hadError = false;
        ParsedClause pc = parseOneClause(&parser);
        if (pc.kind == CL_EOF) { arenaReset(&pl->query, mark); break; }
        if (pc.kind == CL_ERROR) {
            if (!pc.atDotBoundary) skipToNextClauseEnd(&parser);
            arenaReset(&pl->query, mark);
            continue;
        }
        if (pc.kind == CL_DIRECTIVE) {
            solveTopLevel(pl, pc.head);
            arenaReset(&pl->query, mark);
        } else {
            VarMap map = { 0 };
            Term *h = copyTermRec(&pl->program, pc.head, &map);
            Term *b = pc.body ? copyTermRec(&pl->program, pc.body, &map) : NULL;
            freeVarMap(&map);
            addClause(pl, h, b, true);
            arenaReset(&pl->query, mark);
        }
    }
    destroyParser(&parser);
}
void prologConsultFile(Prolog *pl, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return; // a missing file is fine -- not every consult target must exist
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    consultBuffer(pl, buf, rd);
    free(buf);
}

size_t prologMark(Prolog *pl) {
    ArenaMark m = arenaMark(&pl->query);
    if (pl->markStackTop == pl->markStackCap) {
        pl->markStackCap = pl->markStackCap ? pl->markStackCap * 2 : 8;
        pl->markStack = realloc(pl->markStack, pl->markStackCap * sizeof(ArenaMark));
    }
    size_t idx = pl->markStackTop++;
    pl->markStack[idx] = m;
    return idx;
}
void prologReset(Prolog *pl, size_t mark) {
    arenaReset(&pl->query, pl->markStack[mark]);
    pl->markStackTop = mark;
    if (pl->markStackTop == 0 && pl->retractsSinceCompact >= PROLOG_COMPACT_RETRACT_INTERVAL)
        compactProgram(pl);
}
size_t prologProgramBytes(Prolog *pl) {
    size_t total = 0;
    for (ArenaBlock *b = pl->program.first; b; b = b->next) total += b->used;
    return total;
}

PlTerm *prologParseTerm(Prolog *pl, const char *src) {
    Parser parser;
    initParser(&parser, pl, src, strlen(src), &pl->query);
    advance(&parser);
    Term *t = parseExpr(&parser, 1200);
    if (!parser.hadError) {
        skipTriviaMarkSpace(&parser);
        if (parser.cur.kind != TK_EOF) reportPlainErrorParser(&parser, "trailing input after term");
    }
    bool ok = !parser.hadError;
    destroyParser(&parser);
    return ok ? t : NULL;
}

bool prologIsNil(Prolog *pl, PlTerm *t) { t = deref(t); return t->tag == T_ATOM && t->u.atom == pl->atomNil; }
bool prologIsList(Prolog *pl, PlTerm *t) {
    t = deref(t); return t->tag == T_CMP && t->u.cmp.functor == pl->atomDot && t->u.cmp.arity == 2;
}
bool prologGetListHeadTail(Prolog *pl, PlTerm *t, PlTerm **head, PlTerm **tail) {
    t = deref(t);
    if (!(t->tag == T_CMP && t->u.cmp.functor == pl->atomDot && t->u.cmp.arity == 2)) return false;
    *head = t->u.cmp.args[0]; *tail = t->u.cmp.args[1];
    return true;
}
int prologArity(Prolog *pl, PlTerm *t) { (void)pl; t = deref(t); return t->tag == T_CMP ? t->u.cmp.arity : 0; }
const char *prologFunctorName(Prolog *pl, PlTerm *t) {
    t = deref(t);
    if (t->tag == T_ATOM) return atomName(pl, t->u.atom);
    if (t->tag == T_CMP) return atomName(pl, t->u.cmp.functor);
    return NULL;
}
PlTerm *prologArg(Prolog *pl, PlTerm *t, int i) {
    (void)pl; t = deref(t);
    if (t->tag != T_CMP || i < 1 || i > t->u.cmp.arity) return NULL;
    return t->u.cmp.args[i - 1];
}
bool prologGetText(Prolog *pl, PlTerm *t, const char **out, size_t *outLen) { return getTextFlexible(pl, t, out, outLen); }
bool prologGetInt(Prolog *pl, PlTerm *t, long *out) {
    (void)pl; t = deref(t);
    if (t->tag != T_INT) return false;
    *out = t->u.i;
    return true;
}

PlTerm *prologMkAtom(Prolog *pl, const char *name) { return mkAtomRaw(&pl->query, internAtom(pl, name)); }
PlTerm *prologMkCodeList(Prolog *pl, const char *chars, size_t len) { return mkCodeList(pl, chars, len); }
PlTerm *prologMkInt(Prolog *pl, long v) { return mkIntRaw(&pl->query, v); }
PlTerm *prologMkVar(Prolog *pl) { return newVar(&pl->query); }
PlTerm *prologMkCompound(Prolog *pl, const char *functor, int arity, PlTerm **args) {
    Term **owned = allocArgs(&pl->query, arity);
    for (int i = 0; i < arity; i++) owned[i] = args[i];
    return mkCompoundRaw(&pl->query, internAtom(pl, functor), arity, owned);
}
bool prologUnify(Prolog *pl, PlTerm *a, PlTerm *b) { return unify(pl, a, b); }

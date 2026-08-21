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
typedef enum { T_VAR, T_ATOM, T_INT, T_FLT, T_STR, T_CMP } Tag;

struct Term {
    Tag tag;
    union {
        struct Term *ref;   // T_VAR: self if unbound, else the bound value
        Atom atom;           // T_ATOM
        long i;              // T_INT
        double f;            // T_FLT
        struct { const char *chars; size_t len; } str; // T_STR
        struct { Atom functor; int arity; struct Term **args; } cmp; // T_CMP
    } u;
};
typedef struct Term Term;

// --- Arena (chunked bump allocator, mark/reset, never realloc'd) ------

#define ARENA_BLOCK_SIZE (64 * 1024)

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
static Term *mkStringRaw(Arena *a, const char *chars, size_t len) {
    char *copy = arenaAlloc(a, len + 1);
    memcpy(copy, chars, len);
    copy[len] = 0;
    Term *t = arenaAlloc(a, sizeof(Term));
    t->tag = T_STR; t->u.str.chars = copy; t->u.str.len = len;
    return t;
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
    case T_STR:  return a->u.str.len == b->u.str.len &&
                        memcmp(a->u.str.chars, b->u.str.chars, a->u.str.len) == 0;
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
    case T_STR:  return a->u.str.len == b->u.str.len &&
                        memcmp(a->u.str.chars, b->u.str.chars, a->u.str.len) == 0;
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
    case T_STR:  return mkStringRaw(dst, t->u.str.chars, t->u.str.len);
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
    case T_STR: wputn(w, t->u.str.chars, t->u.str.len); break;
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

static void formatBallMessage(Prolog *pl, Term *ball, char *buf, size_t bufsz) {
    Term *b = deref(ball);
    if (b->tag == T_CMP && b->u.cmp.arity == 1 && b->u.cmp.functor == pl->atomError) {
        Term *msg = deref(b->u.cmp.args[0]);
        if (msg->tag == T_STR) { snprintf(buf, bufsz, "%.*s", (int)msg->u.str.len, msg->u.str.chars); return; }
    }
    writeTerm(pl, b, buf, bufsz);
}

static void reportPlainError(Prolog *pl, const char *msg) {
    if (pl->errorFn) pl->errorFn(msg, pl->errorCtx);
}

// --- throw / errors ---------------------------------------------------

static _Noreturn void throwBall(Prolog *pl, Term *ball) {
    if (!pl->catchTop) abort(); // invariant: solveTopLevel always installs a frame first
    CatchFrame *f = pl->catchTop;
    pl->catchTop = f->prev;
    f->ball = ball;
    longjmp(f->jb, 1);
}

static _Noreturn void engineErrorV(Prolog *pl, const char *fmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof buf, fmt, ap);
    Term *msg = mkStringRaw(&pl->query, buf, strlen(buf));
    Term *ball = mkCompound1(&pl->query, pl->atomError, msg);
    throwBall(pl, ball);
}
static _Noreturn void engineError(Prolog *pl, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    engineErrorV(pl, fmt, ap);
    va_end(ap); // unreachable, silences "unused" warnings on some compilers
    abort();
}

_Noreturn void prologThrowMsg(Prolog *pl, const char *message) {
    engineError(pl, "%s", message);
}

// --- arithmetic ---------------------------------------------------------

typedef struct { bool isFloat; long i; double f; } Num;
static double numAsF(Num n) { return n.isFloat ? n.f : (double)n.i; }

static Num evalArith(Prolog *pl, Term *t) {
    t = deref(t);
    if (t->tag == T_INT) return (Num){ false, t->u.i, 0 };
    if (t->tag == T_FLT) return (Num){ true, 0, t->u.f };
    if (t->tag == T_VAR) engineError(pl, "arithmetic: unbound variable");
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
                if (fl) engineError(pl, "// requires integers");
                if (b.i == 0) engineError(pl, "division by zero");
                return (Num){ false, a.i / b.i, 0 };
            }
            if (streq(f, "mod")) {
                if (fl) engineError(pl, "mod requires integers");
                if (b.i == 0) engineError(pl, "division by zero");
                long m = a.i % b.i;
                if (m != 0 && ((m < 0) != (b.i < 0))) m += b.i;
                return (Num){ false, m, 0 };
            }
            if (streq(f, "min")) return numAsF(a) < numAsF(b) ? a : b;
            if (streq(f, "max")) return numAsF(a) > numAsF(b) ? a : b;
        }
    }
    engineError(pl, "arithmetic: not evaluable");
}

// --- getText (atom or string -> chars, for key/mod names, te_* args) -----

static bool getText(Prolog *pl, Term *t, const char **out, size_t *outLen) {
    t = deref(t);
    if (t->tag == T_ATOM) { const char *n = atomName(pl, t->u.atom); *out = n; *outLen = strlen(n); return true; }
    if (t->tag == T_STR) { *out = t->u.str.chars; *outLen = t->u.str.len; return true; }
    return false;
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
    else engineError(pl, "assert: clause head must be callable");
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
    if (goal->tag == T_VAR) engineError(pl, "instantiation error: unbound goal");
    Atom functor; int arity; Term **args;
    if (goal->tag == T_ATOM) { functor = goal->u.atom; arity = 0; args = NULL; }
    else if (goal->tag == T_CMP) { functor = goal->u.cmp.functor; arity = goal->u.cmp.arity; args = goal->u.cmp.args; }
    else engineError(pl, "type error: callable expected");

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
    if (functor == pl->atomCall && arity >= 1 && arity <= 3) {
        Term *base = deref(args[0]);
        Term *effective;
        int extra = arity - 1;
        if (extra == 0) effective = base;
        else {
            Atom bf; int bar; Term **bargs = NULL;
            if (base->tag == T_ATOM) { bf = base->u.atom; bar = 0; }
            else if (base->tag == T_CMP) { bf = base->u.cmp.functor; bar = base->u.cmp.arity; bargs = base->u.cmp.args; }
            else { engineError(pl, "call/%d: not callable", arity); }
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
    if (!pred) engineError(pl, "unknown procedure %s/%d", atomName(pl, functor), arity);

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
            return true;
        }
        undoTrailTo(pl, tmark);
    }
    return false;
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

static bool nativeAtomConcat(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *x, *y; size_t xl, yl;
    if (!getText(pl, a[0], &x, &xl) || !getText(pl, a[1], &y, &yl))
        engineError(pl, "atom_concat/3: both arguments must be bound (decomposition mode isn't supported)");
    char *buf = malloc(xl + yl + 1);
    memcpy(buf, x, xl); memcpy(buf + xl, y, yl); buf[xl + yl] = 0;
    Term *r = mkAtomRaw(&pl->query, internAtom(pl, buf));
    free(buf);
    return unify(pl, a[2], r);
}
static bool nativeStringConcat(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *x, *y; size_t xl, yl;
    if (!getText(pl, a[0], &x, &xl) || !getText(pl, a[1], &y, &yl))
        engineError(pl, "string_concat/3: both arguments must be bound");
    char *buf = malloc(xl + yl);
    memcpy(buf, x, xl); memcpy(buf + xl, y, yl);
    Term *r = mkStringRaw(&pl->query, buf, xl + yl);
    free(buf);
    return unify(pl, a[2], r);
}
static bool nativeAtomLength(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *s; size_t len;
    if (!getText(pl, a[0], &s, &len)) engineError(pl, "atom_length/2: first argument must be bound");
    return unify(pl, a[1], mkIntRaw(&pl->query, (long)len));
}
static bool nativeAtomString(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *A = deref(a[0]);
    if (A->tag != T_VAR) {
        const char *s; size_t len;
        if (!getText(pl, A, &s, &len)) engineError(pl, "atom_string/2: not atomic");
        return unify(pl, a[1], mkStringRaw(&pl->query, s, len));
    }
    const char *s; size_t len;
    if (!getText(pl, a[1], &s, &len)) engineError(pl, "atom_string/2: second argument must be bound when the first is unbound");
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, s)));
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
        if (!getText(pl, A, &txt, &len)) return false;
        Num n;
        if (!parseNum(txt, len, &n)) return false;
        return unify(pl, a[1], n.isFloat ? mkFloatRaw(&pl->query, n.f) : mkIntRaw(&pl->query, n.i));
    }
    Term *N = deref(a[1]);
    char buf[64];
    if (N->tag == T_INT) snprintf(buf, sizeof buf, "%ld", N->u.i);
    else if (N->tag == T_FLT) snprintf(buf, sizeof buf, "%g", N->u.f);
    else engineError(pl, "atom_number/2: second argument must be a number when the first is unbound");
    return unify(pl, a[0], mkAtomRaw(&pl->query, internAtom(pl, buf)));
}
static bool nativeNumberString(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    Term *N = deref(a[0]);
    if (N->tag == T_INT || N->tag == T_FLT) {
        char buf[64];
        if (N->tag == T_INT) snprintf(buf, sizeof buf, "%ld", N->u.i);
        else snprintf(buf, sizeof buf, "%g", N->u.f);
        return unify(pl, a[1], mkStringRaw(&pl->query, buf, strlen(buf)));
    }
    const char *txt; size_t len;
    if (!getText(pl, a[1], &txt, &len)) engineError(pl, "number_string/2: second argument must be bound when the first isn't a number");
    Num n;
    if (!parseNum(txt, len, &n)) return false;
    return unify(pl, a[0], n.isFloat ? mkFloatRaw(&pl->query, n.f) : mkIntRaw(&pl->query, n.i));
}
static bool nativeGetTime(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    return unify(pl, a[0], mkFloatRaw(&pl->query, (double)time(NULL)));
}
static bool nativeFormatTime(Prolog *pl, Term **a, int arity, void *ctx) {
    (void)arity; (void)ctx;
    const char *fmt; size_t fmtLen;
    if (!getText(pl, a[1], &fmt, &fmtLen)) engineError(pl, "format_time/3: format must be an atom or string");
    Term *ts = deref(a[2]);
    time_t tt;
    if (ts->tag == T_INT) tt = (time_t)ts->u.i;
    else if (ts->tag == T_FLT) tt = (time_t)ts->u.f;
    else engineError(pl, "format_time/3: timestamp must be a number (see get_time/1)");
    char fmtBuf[128]; size_t fl = fmtLen < 127 ? fmtLen : 127;
    memcpy(fmtBuf, fmt, fl); fmtBuf[fl] = 0;
    struct tm tmv = *localtime(&tt); // te is single-threaded; localtime's static buffer is fine here
    char out[256];
    size_t n = strftime(out, sizeof out, fmtBuf, &tmv);
    return unify(pl, a[0], mkStringRaw(&pl->query, out, n));
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
    prologRegisterNative(pl, "string_concat", 3, nativeStringConcat, NULL);
    prologRegisterNative(pl, "atom_length", 2, nativeAtomLength, NULL);
    prologRegisterNative(pl, "string_length", 2, nativeAtomLength, NULL);
    prologRegisterNative(pl, "atom_string", 2, nativeAtomString, NULL);
    prologRegisterNative(pl, "atom_number", 2, nativeAtomNumber, NULL);
    prologRegisterNative(pl, "number_string", 2, nativeNumberString, NULL);
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
    ":-", "->", "\\+", "\\==", "\\=", "==", "=<", ">=", "=:=", "=\\=", "//", NULL
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
    { "=:=", 700, false }, { "=\\=", 700, false },
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

static Term *parsePrimary(Parser *p) {
    Token t = p->cur;
    switch (t.kind) {
    case TK_INT: advance(p); return mkIntRaw(p->target, t.ival);
    case TK_FLOAT: advance(p); return mkFloatRaw(p->target, t.fval);
    case TK_STRING: advance(p); return mkStringRaw(p->target, t.text, t.textLen);
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

    Parser parser;
    initParser(&parser, pl, buf, rd, &pl->query);
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
bool prologGetText(Prolog *pl, PlTerm *t, const char **out, size_t *outLen) { return getText(pl, t, out, outLen); }
bool prologGetInt(Prolog *pl, PlTerm *t, long *out) {
    (void)pl; t = deref(t);
    if (t->tag != T_INT) return false;
    *out = t->u.i;
    return true;
}

PlTerm *prologMkAtom(Prolog *pl, const char *name) { return mkAtomRaw(&pl->query, internAtom(pl, name)); }
PlTerm *prologMkString(Prolog *pl, const char *chars, size_t len) { return mkStringRaw(&pl->query, chars, len); }
PlTerm *prologMkInt(Prolog *pl, long v) { return mkIntRaw(&pl->query, v); }
bool prologUnify(Prolog *pl, PlTerm *a, PlTerm *b) { return unify(pl, a, b); }

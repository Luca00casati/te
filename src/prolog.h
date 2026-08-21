// A small, from-scratch Prolog engine -- not editor-specific (see script.c
// for the `te`-facing integration that sits on top of this). Covers facts,
// rules, unification, backtracking, cut, if-then-else, arithmetic,
// catch/throw, assert/retract, findall, and a practical-subset parser with
// infix operators (no user-defined op/3). Not ISO-complete by design.
#ifndef TE_PROLOG_H
#define TE_PROLOG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Prolog Prolog;

// A term reference. Valid until the next prologReset() at or below the mark
// in effect when the term was created (see prologMark/prologReset).
typedef struct Term PlTerm;

// A native (C-implemented) predicate. `args` are already dereferenced through
// any variable bindings. Return true on success -- having unified any output
// arguments via prologUnify -- or false for an ordinary logical failure. For
// a usage error (wrong argument type, etc.), call prologThrowMsg instead of
// returning; it never returns to the caller.
typedef bool (*PrologNative)(Prolog *pl, PlTerm *args[], int arity, void *ctx);

// Called whenever a top-level prologSolve/consult directive throws an error
// that nothing caught (or hits an engine error: unknown procedure, a type
// error, a parse error, ...). Never called for a plain logical failure.
typedef void (*PrologErrorFn)(const char *message, void *ctx);

Prolog *prologCreate(void);
void prologDestroy(Prolog *pl);

void prologSetErrorHandler(Prolog *pl, PrologErrorFn fn, void *ctx);

// Registers a native predicate under name/arity. Overwrites any existing
// native or user-defined predicate at that name/arity.
void prologRegisterNative(Prolog *pl, const char *name, int arity, PrologNative fn, void *ctx);

// Parses and loads clauses from `path`, running `:- Goal.` directives
// immediately as encountered. A missing file is a silent no-op (mirrors a
// missing config file). Parse errors and directive errors are reported via the
// error handler; loading continues with the next clause rather than
// aborting.
void prologConsultFile(Prolog *pl, const char *path);

// --- Query-arena scratch space --------------------------------------------
// Everything a query allocates (parsed terms, fresh variables, clause
// renamings) lives in a bump-allocated arena reset back to a mark once the
// caller is done with it -- this is what keeps "re-solve this query every
// frame" cheap and leak-free. Take a mark before a batch of related work
// (parsing a goal, solving it, solving any handler it triggers) and reset
// once, after the *last* use of any term from that batch.
size_t prologMark(Prolog *pl);
void prologReset(Prolog *pl, size_t mark);

// Parses one term (a goal or a plain data term) from a NUL-terminated
// string in the current query arena. Returns NULL on a syntax error
// (reported via the error handler).
PlTerm *prologParseTerm(Prolog *pl, const char *src);

// Solves `goal` as a top-level call (call/1 semantics: first solution only),
// leaving goal's variables bound on success. An uncaught throw or engine
// error is reported via the error handler and makes this return false, the
// same as an ordinary failed goal -- callers don't need to distinguish "no
// match" from "errored".
bool prologSolve(Prolog *pl, PlTerm *goal);

// --- Term inspection (read-only; all deref through bound variables) -------
bool prologIsNil(Prolog *pl, PlTerm *t);                  // t == the atom '[]'
bool prologIsList(Prolog *pl, PlTerm *t);                 // t is '.'(Head,Tail)
bool prologGetListHeadTail(Prolog *pl, PlTerm *t, PlTerm **head, PlTerm **tail);
int prologArity(Prolog *pl, PlTerm *t);                    // 0 for atom/number/string
const char *prologFunctorName(Prolog *pl, PlTerm *t);      // atom/compound name, else NULL
PlTerm *prologArg(Prolog *pl, PlTerm *t, int i);            // 1-based
// Atom or string -> text (used for key/mod names, te_echo/te_insert/te_text).
bool prologGetText(Prolog *pl, PlTerm *t, const char **out_chars, size_t *out_len);
bool prologGetInt(Prolog *pl, PlTerm *t, long *out);

// --- Term construction (used by native predicates to build outputs) ------
PlTerm *prologMkAtom(Prolog *pl, const char *name);
PlTerm *prologMkString(Prolog *pl, const char *chars, size_t len);
PlTerm *prologMkInt(Prolog *pl, long v);
bool prologUnify(Prolog *pl, PlTerm *a, PlTerm *b);

// Throws a Prolog error(te_error(Message), _) ball from within a native
// predicate. Never returns.
_Noreturn void prologThrowMsg(Prolog *pl, const char *message);

#endif // TE_PROLOG_H

// Engine-level unit tests for src/prolog.c/.h -- unlike tests/unit_te.c,
// this doesn't need to #include src/main.c (the engine isn't editor-
// specific), so it links against build/prolog.o directly.
#include "../src/prolog.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define RUN(fn) do { fn(); } while (0)

static void onError(const char *msg, void *ctx) {
    (void)ctx;
    fprintf(stderr, "  prolog error: %s\n", msg);
}

// Parses `src` as a goal and solves it once, inside its own mark/reset --
// mirrors script.c's own prologMark/prologSolve/prologReset usage pattern.
static bool solveText(Prolog *pl, const char *src) {
    size_t mark = prologMark(pl);
    PlTerm *g = prologParseTerm(pl, src);
    bool ok = g && prologSolve(pl, g);
    prologReset(pl, mark);
    return ok;
}

static void test_assert_retract_roundtrip(void) {
    Prolog *pl = prologCreate();
    prologSetErrorHandler(pl, onError, NULL);
    CHECK(solveText(pl, "assertz(foo(1))"));
    CHECK(solveText(pl, "foo(1)"));
    CHECK(!solveText(pl, "foo(2)"));
    CHECK(solveText(pl, "retract(foo(1))"));
    CHECK(!solveText(pl, "foo(1)"));
    prologDestroy(pl);
}

// The regression test for the arena-compaction fix (prolog.c's
// compactProgram, triggered from prologReset once retracts accumulate past
// PROLOG_COMPACT_RETRACT_INTERVAL): repeatedly retract a singleton fact and
// assert its replacement -- the exact "mutable state" idiom src/undo_
// history.pl, src/search.pl, src/movement.pl, and (once buffer-local
// variables land) src/buffers.pl all use. Without the fix, `program` grows
// by roughly one leaked clause per iteration (retract unlinks but never
// frees); with it, growth is bounded by compaction kicking in periodically.
static void test_retract_does_not_leak_program_arena(void) {
    Prolog *pl = prologCreate();
    prologSetErrorHandler(pl, onError, NULL);
    CHECK(solveText(pl, "assertz(counter(0))"));

    const int iterations = 6000; // several multiples of the compaction interval
    for (int i = 1; i <= iterations; i++) {
        char goal[64];
        snprintf(goal, sizeof goal, "retract(counter(_)), assertz(counter(%d))", i);
        CHECK(solveText(pl, goal));
    }

    // Correctness: the fact itself must still be exactly right after however
    // many compactions fired along the way.
    CHECK(solveText(pl, "counter(6000)"));

    // Leak check: 6000 leaked clauses would run into the hundreds of KB
    // (each retract leaks a whole Clause + its head term); a generous cap
    // well below that only holds if compaction actually reclaimed memory.
    size_t bytes = prologProgramBytes(pl);
    CHECK(bytes < 50000);

    prologDestroy(pl);
}

// The regression test for first-argument clause indexing (prolog.c's
// buildPredIndex/mergeBySeq, triggered once a predicate passes
// PROLOG_INDEX_MIN_CLAUSES clauses): a naive index that just concatenated
// "keyed bucket, then wildcards" (or vice versa) instead of merging by
// original assertion order would return solutions in the wrong order here
// -- this is the one thing a full linear scan is guaranteed to get right
// that a careless index implementation can silently get wrong.
static void test_index_preserves_assertion_order(void) {
    Prolog *pl = prologCreate();
    prologSetErrorHandler(pl, onError, NULL);
    // route(_, _) clauses (10, past the indexing threshold): keyed facts for
    // a/b/target/target/target/c/d interleaved with wildcard (unbound first
    // arg) facts, in this exact assertion order.
    const char *facts[] = {
        "assertz(route(a, va))",
        "assertz(route(_, wa))",   // wildcard
        "assertz(route(b, vb))",
        "assertz(route(target, v1))",
        "assertz(route(_, wb))",   // wildcard
        "assertz(route(target, v2))",
        "assertz(route(c, vc))",
        "assertz(route(_, wc))",   // wildcard
        "assertz(route(target, v3))",
        "assertz(route(d, vd))",
    };
    for (size_t i = 0; i < sizeof facts / sizeof facts[0]; i++) CHECK(solveText(pl, facts[i]));

    size_t mark = prologMark(pl);
    PlTerm *g = prologParseTerm(pl, "findall(V, route(target, V), Results)");
    CHECK(g != NULL);
    bool ok = g && prologSolve(pl, g);
    CHECK(ok);
    if (ok) {
        // Every route(_, ...) wildcard and every route(target, ...) fact
        // matches, in the order they were asserted.
        const char *expected[] = { "wa", "v1", "wb", "v2", "wc", "v3" };
        size_t expectedCount = sizeof expected / sizeof expected[0];
        PlTerm *results = prologArg(pl, g, 3);
        size_t idx = 0;
        PlTerm *cur = results;
        while (prologIsList(pl, cur)) {
            PlTerm *head, *tail;
            CHECK(prologGetListHeadTail(pl, cur, &head, &tail));
            const char *name = prologFunctorName(pl, head);
            if (idx < expectedCount) CHECK(name && strcmp(name, expected[idx]) == 0);
            idx++;
            cur = tail;
        }
        CHECK(prologIsNil(pl, cur));
        CHECK(idx == expectedCount);
    }
    prologReset(pl, mark);
    prologDestroy(pl);
}

int main(void) {
    RUN(test_assert_retract_roundtrip);
    RUN(test_retract_does_not_leak_program_arena);
    RUN(test_index_preserves_assertion_order);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

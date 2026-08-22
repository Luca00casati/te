#define _DEFAULT_SOURCE // mkdtemp
// Black-box tests for `te`'s headless `--regex` mode: run the actual
// compiled binary as a subprocess (no GLFW window ever opens on this path,
// see grepMode() in main.c) and check its stdout, exit code, and file output
// against a handful of small fixture files.
//
// Complements unit_te.c, which reaches internal logic via #include and can't
// see argv parsing, process exit codes, or real file I/O the way a caller
// actually invokes the binary.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *g_te_path;
static char g_tmpdir[] = "/tmp/te_cli_test_XXXXXX";

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_STREQ(actual, expected) do { \
    g_checks++; \
    if (strcmp((actual), (expected)) != 0) { \
        g_failures++; \
        fprintf(stderr, "  FAIL %s:%d: expected %s, got %s\n", __FILE__, __LINE__, \
                nob_quote_or(expected), nob_quote_or(actual)); \
    } \
} while (0)

// Quotes a string for the failure message above (or prints (null)).
static const char *nob_quote_or(const char *s) {
    static char buf[4096];
    if (!s) return "(null)";
    snprintf(buf, sizeof(buf), "\"%s\"", s);
    return buf;
}

#define RUN(fn) do { fn(); } while (0)

// Runs `te` with the given argv (NULL-terminated, argv[0] ignored -- always
// replaced by g_te_path), feeding stdin_data to its stdin and merging its
// stdout+stderr into out_buf. Fixture sizes here are small (well under a
// pipe's 64KB buffer), so writing all of stdin before reading any output
// can't deadlock.
static int runTe(char *const argv[], const char *stdin_data, size_t stdin_len,
                  char *out_buf, size_t out_cap, size_t *out_len) {
    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0 || pipe(outpipe) != 0) {
        perror("pipe");
        exit(1);
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(outpipe[1], STDERR_FILENO);
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        execv(g_te_path, argv);
        _exit(127);
    }
    close(inpipe[0]);
    close(outpipe[1]);
    if (stdin_data && stdin_len > 0) {
        ssize_t w = write(inpipe[1], stdin_data, stdin_len);
        (void)w;
    }
    close(inpipe[1]); // EOF on the child's stdin

    size_t total = 0;
    ssize_t n;
    while (total < out_cap - 1 && (n = read(outpipe[0], out_buf + total, out_cap - 1 - total)) > 0) {
        total += (size_t)n;
    }
    out_buf[total] = 0;
    close(outpipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (out_len) *out_len = total;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void writeFile(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(contents, 1, strlen(contents), f);
    fclose(f);
}

static char *readFileAlloc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

// --- tests -----------------------------------------------------------------
static void test_match_found_exits_zero(void) {
    char in[512];
    snprintf(in, sizeof(in), "%s/in.txt", g_tmpdir);
    writeFile(in, "hello world\nfoo bar\nhello again\n");

    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "hello", in, NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 0);
    CHECK_STREQ(out, "1:hello world\n3:hello again\n");
}

static void test_no_match_exits_one(void) {
    char in[512];
    snprintf(in, sizeof(in), "%s/in.txt", g_tmpdir);
    writeFile(in, "hello world\n");

    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "zzzz", in, NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 1);
    CHECK(out_len == 0);
}

static void test_missing_pattern_exits_two(void) {
    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);
    CHECK(rc == 2);
}

static void test_bad_regex_exits_two(void) {
    char in[512];
    snprintf(in, sizeof(in), "%s/in.txt", g_tmpdir);
    writeFile(in, "anything\n");

    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "(unterminated", in, NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 2);
    CHECK(strstr(out, "invalid regex") != NULL);
}

static void test_missing_input_file_exits_two(void) {
    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "x", "/no/such/file/hopefully", NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 2);
    CHECK(strstr(out, "cannot read file") != NULL);
}

static void test_reads_from_stdin_when_no_file(void) {
    const char *stdin_data = "line one\nmatch here\nline three\n";
    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "match", NULL };
    int rc = runTe(argv, stdin_data, strlen(stdin_data), out, sizeof(out), &out_len);

    CHECK(rc == 0);
    CHECK_STREQ(out, "2:match here\n");
}

static void test_writes_to_output_file(void) {
    char in[512], outpath[512];
    snprintf(in, sizeof(in), "%s/in2.txt", g_tmpdir);
    snprintf(outpath, sizeof(outpath), "%s/out2.txt", g_tmpdir);
    writeFile(in, "apple\nbanana\napricot\n");

    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "ap", in, outpath, NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 0);
    CHECK(out_len == 0); // nothing to stdout when writing to a file

    char *written = readFileAlloc(outpath);
    CHECK(written != NULL);
    if (written) {
        CHECK_STREQ(written, "1:apple\n3:apricot\n");
        free(written);
    }
}

static void test_duplicate_matches_on_one_line_collapse(void) {
    char in[512];
    snprintf(in, sizeof(in), "%s/in3.txt", g_tmpdir);
    writeFile(in, "aXaXa\nplain\n");

    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "X", in, NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 0);
    CHECK_STREQ(out, "1:aXaXa\n"); // one line entry even though "X" matches twice
}

static void test_pcre_syntax_case_insensitive_flag(void) {
    char in[512];
    snprintf(in, sizeof(in), "%s/in4.txt", g_tmpdir);
    writeFile(in, "Hello World\nGOODBYE\n");

    char out[4096];
    size_t out_len;
    char *argv[] = { (char *)g_te_path, "--regex", "(?i)hello", in, NULL };
    int rc = runTe(argv, NULL, 0, out, sizeof(out), &out_len);

    CHECK(rc == 0);
    CHECK_STREQ(out, "1:Hello World\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-te-binary>\n", argv[0]);
        return 2;
    }
    g_te_path = argv[1];

    if (!mkdtemp(g_tmpdir)) {
        perror("mkdtemp");
        return 1;
    }

    RUN(test_match_found_exits_zero);
    RUN(test_no_match_exits_one);
    RUN(test_missing_pattern_exits_two);
    RUN(test_bad_regex_exits_two);
    RUN(test_missing_input_file_exits_two);
    RUN(test_reads_from_stdin_when_no_file);
    RUN(test_writes_to_output_file);
    RUN(test_duplicate_matches_on_one_line_collapse);
    RUN(test_pcre_syntax_case_insensitive_flag);

    char rm_cmd[512];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", g_tmpdir); // g_tmpdir is our own mkdtemp() output
    if (system(rm_cmd) != 0) fprintf(stderr, "warning: failed to clean up %s\n", g_tmpdir);

    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

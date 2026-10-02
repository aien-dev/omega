/* Executes the production lifecycle wrappers against a simulated M16 driver.
 * Each scenario gets a fresh process. There is intentionally no reset API. */
#include "omega_numeric_native.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

atomic_int omega_numeric_native_state = ATOMIC_VAR_INIT(OMEGA_NUMERIC_NATIVE_IDLE);
static int opens, closes, waits, open_failure, close_failure, fail_wait, wrong_word;
int m16_native_open(M16NativeContext *ctx) {
    (void)ctx; opens++; return open_failure ? -1 : 0;
}
int m16_native_close(M16NativeContext *ctx) {
    (void)ctx; closes++; return close_failure ? -1 : 0;
}
int m16_native_wait_marker(volatile uint32_t *word, uint32_t want, uint64_t ms) {
    assert(ms == 5000 || ms == 600000);
    waits++;
    if (waits == fail_wait) return -1;
    *word = wrong_word ? want + 1u : want;
    return 0;
}
/* Defined in another translation unit: the latch must be process-wide. */
int numeric_other_launcher_open(void);

static void check_blocked(void) {
    int before = opens;
    assert(numeric_other_launcher_open() != 0);
    assert(opens == before);
}
static void scenario(int which) {
    M16NativeContext ctx;
    volatile uint32_t marker = 0;
    if (which == 0) {
        open_failure = 1;
        assert(omega_numeric_native_open(&ctx) != 0);
        assert(closes == 0);
        open_failure = 0;
        assert(omega_numeric_native_open(&ctx) == 0);
        assert(omega_numeric_native_close(&ctx) == 0);
        return;
    }
    assert(omega_numeric_native_open(&ctx) == 0);
    if (which == 1) {
        check_blocked(); /* a simultaneous launcher cannot open another seat */
        assert(omega_numeric_native_close(&ctx) == 0); /* pre-submit cleanup */
        assert(omega_numeric_native_open(&ctx) == 0);
        assert(omega_numeric_native_close(&ctx) == 0);
        assert(closes == 2);
        return;
    }
    if (which == 2) {
        assert(omega_numeric_native_wait(&marker, 0x44444444u, 5000) == 0);
        assert(omega_numeric_native_wait(&marker, 6u, 5000) == 0);
        assert(omega_numeric_native_close(&ctx) == 0);
        assert(omega_numeric_native_open(&ctx) == 0);
        assert(omega_numeric_native_close(&ctx) == 0);
        return;
    }
    if (which == 3) {
        close_failure = 1;
        assert(omega_numeric_native_close(&ctx) != 0);
        check_blocked();
        return;
    }
    if (which == 4) wrong_word = 1;
    else fail_wait = which - 4; /* first marker, second marker, then semaphore */
    int failed = 0;
    const uint32_t expected[] = {0x44444444u, 0x46464646u, 6u};
    for (int i = 0; i < 3; i++) {
        if (omega_numeric_native_wait(&marker, expected[i], 600000) != 0) {
            failed = 1;
            break;
        }
    }
    assert(failed);
    assert(omega_numeric_native_uncertain());
    assert(omega_numeric_native_close(&ctx) != 0);
    assert(closes == 0); /* never release a possibly running channel */
    omega_numeric_native_release(); /* a late cleanup cannot clear poison */
    check_blocked();
}
int main(void) {
    for (int i = 0; i < 8; i++) {
        pid_t p = fork();
        assert(p >= 0);
        if (p == 0) { scenario(i); _exit(0); }
        int status;
        assert(waitpid(p, &status, 0) == p);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL numeric lifecycle scenario %d\n", i);
            return 1;
        }
    }
    puts("numeric lifecycle: PASS (8 simulated-driver scenarios; hardware NOT_RUN)");
    return 0;
}

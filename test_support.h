/*
 * Shared test scaffolding. Include this first: it sets the POSIX feature
 * macro the death tests need before any libc header is pulled in.
 */
#pragma once

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) t_check((condition), #condition, __FILE__, __LINE__)

/* Reports the first failed expectation and stops, so output stays readable. */
static inline void
t_check(int ok, const char *text, const char *file, int line)
{
    if (!ok) {
        (void) fprintf(stderr, "FAIL %s:%d: %s\n", file, line, text);
        exit(1);
    }
}

/*
 * Runs body in a child process and requires it to die on a contract abort.
 * Contract violations are fatal by design, so they can only be tested from a
 * process we are willing to lose.
 */
static inline void
t_expect_abort(void (*body)(void), const char *name)
{
    pid_t pid = fork();
    int status = 0;

    CHECK(pid >= 0);
    if (pid == 0) {
        /* The contract message is expected here and would only be noise. */
        FILE *silenced = freopen("/dev/null", "w", stderr);

        (void) silenced;
        body();
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        (void) fprintf(stderr, "FAIL %s did not abort\n", name);
        exit(1);
    }
}

/*
 * xorshift64star, so randomised tests are reproducible from their seed and a
 * failure can be replayed exactly.
 */
static inline uint64_t
t_random_next(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

/* Returns a value in [0, bound), which is uniform enough for test data. */
static inline uint32_t
t_random_below(uint64_t *state, uint32_t bound)
{
    CHECK(bound > 0U);
    return (uint32_t) (t_random_next(state) % bound);
}

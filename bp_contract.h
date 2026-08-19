#pragma once

#include <stdio.h>
#include <stdlib.h>

/*
 * Contract check that stays active in every build, release included.
 * Invalid caller input is a programming defect, not an operational condition,
 * so the process aborts immediately instead of repairing or reporting. Costs
 * a predictable branch; the nesting hot paths check once at their boundary
 * and then run unguarded.
 */
#define BP_REQUIRE(condition)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void) fprintf(stderr, "contract failed: %s at %s:%d\n",           \
                #condition, __FILE__, __LINE__);                               \
            abort();                                                           \
        }                                                                      \
    } while (0)

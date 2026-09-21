// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdio>

extern int g_failures;
extern int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                      \
    } while (0)

#define SECTION(name) printf("[test] %s\n", name)

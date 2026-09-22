// SPDX-License-Identifier: BSL-1.0
// Minimal headless test runner (no OpenXR/Vulkan runtime needed).
#include "test_util.h"
#include <cstdio>

int g_failures = 0;
int g_checks = 0;

void test_video_decoder();
void test_protocol_fec();
void test_foveation();

int main()
{
    test_video_decoder();
    test_protocol_fec();
    test_foveation();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

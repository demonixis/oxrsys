// SPDX-License-Identifier: MPL-2.0
// Minimal headless test runner (no OpenXR/Vulkan runtime needed).
#include "test_util.h"
#include <cstdio>

int g_failures = 0;
int g_checks = 0;

void test_video_decoder();
void test_protocol_fec();

int main()
{
    test_video_decoder();
    test_protocol_fec();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// SPDX-License-Identifier: MPL-2.0
// Decodes the bundled SBS test clip and checks the output is well-formed and
// genuinely stereo (left and right halves differ).
#include "test_util.h"
#include "video_decoder.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef TEST_ASSET_DIR
#define TEST_ASSET_DIR "assets"
#endif

// Mean absolute per-channel difference between the two SBS halves of an RGBA frame.
static double halvesDiffer(const std::vector<uint8_t>& rgba, int w, int h)
{
    const int half = w / 2;
    long long acc = 0;
    long long n = 0;
    for (int y = 0; y < h; y += 8) {
        for (int x = 0; x < half; x += 8) {
            const uint8_t* l = &rgba[((size_t)y * w + x) * 4];
            const uint8_t* r = &rgba[((size_t)y * w + (x + half)) * 4];
            for (int c = 0; c < 3; c++) { acc += std::abs((int)l[c] - (int)r[c]); ++n; }
        }
    }
    return n ? (double)acc / n : 0.0;
}

void test_video_decoder()
{
    SECTION("video_decoder: file mode decodes SBS HEVC");
    VideoDecoder dec;
    const std::string path = std::string(TEST_ASSET_DIR) + "/stereo_test.hevc";
    CHECK(dec.Open(path));

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool got = dec.NextFrameRGBA(rgba, w, h);
    CHECK(got);
    CHECK(w == 1920);
    CHECK(h == 1080);
    CHECK(rgba.size() == (size_t)w * h * 4);

    // The generated clip has distinct left/right halves — prove it decoded real
    // content (not a blank/constant frame) and is actually side-by-side stereo.
    if (got && w > 0) {
        double diff = halvesDiffer(rgba, w, h);
        CHECK(diff > 5.0); // clearly different halves, not noise-level
    }

    // Looping: many NextFrame calls keep returning frames past end-of-stream.
    int frames = 1;
    for (int i = 0; i < 120; i++)
        if (dec.NextFrameRGBA(rgba, w, h)) ++frames;
    CHECK(frames >= 100);
    dec.Close();
}

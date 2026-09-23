#!/usr/bin/env bash
# Regenerate the side-by-side stereo HEVC test fixture used by the tests and V1.
# Left half: color-bar test pattern. Right half: hue-shifted circle pattern.
set -euo pipefail
cd "$(dirname "$0")"
ffmpeg -y \
  -f lavfi -i "testsrc2=size=960x1080:rate=30:duration=3" \
  -f lavfi -i "testsrc=size=960x1080:rate=30:duration=3" \
  -filter_complex "[1:v]hue=h=180[r];[0:v][r]hstack=inputs=2" \
  -c:v libx265 -pix_fmt yuv420p -x265-params "keyint=30:log-level=error" \
  stereo_test.hevc
echo "wrote stereo_test.hevc"

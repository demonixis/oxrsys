# Native-arm64 hardware video encoder helper

## Why this exists

The OXRSys runtime dylib is `dlopen`ed **in-process by CrossOver's x86_64 Wine
host** (Rosetta). VideoToolbox **refuses the hardware HEVC encoder to an
x86_64/Rosetta process** — `VTCompressionSessionCreate` with
`RequireHardware=YES` fails `kVTCouldNotFindVideoEncoderErr (-12908)`, and with
`Require=NO` it silently falls back to the **software** encoder, which saturates
the CPU at full resolution. `VTCopyVideoEncoderList` on an M4 Pro shows exactly
this split: **12 hardware encoders natively, including `Apple HEVC (HW)`; 8
under Rosetta, where HEVC appears software-only** while hardware **H.264** is
still granted. (There is no AV1 *encoder* in either list — Apple Silicon has
hardware AV1 decode from M3 on, but exposes no encoder — so AV1 is out of scope
and stays on the in-process path.)

The only way to reach the hardware HEVC encoder is a **native-arm64 process**.
This helper is that process. It encodes whichever codec and profile the client
and runtime negotiated — **H.265 Main, H.265 Main10, or H.264 Main** — and never
substitutes another. All GPU composition (blit / downscale / foveation) stays in
the runtime; only the final `VTCompressionSessionEncodeFrame` moves
out-of-process. The runtime shares its compose IOSurfaces with the helper
**zero-copy**, so no frame pixels are ever copied between processes.

Main10 rides the same 8-bit BGRA surface contract the in-process path uses: it
is a 10-bit *bitstream* from an 8-bit source, which is what the in-process
encoder has always produced for `encoder_10bit`. True 10-bit source would mean a
new compose format end to end and is deliberately not attempted here.

## Measured, on an M4 Pro

`smoke_test.mm` encodes 2272×1264 frames one at a time down both paths, from the
same IOSurfaces, with the same session properties and thread QoS. Per-frame
averages over 35 frames after warmup:

| Parent process    | Codec        | In-process        | Helper (round trip) |
| ----------------- | ------------ | ----------------- | ------------------- |
| x86_64 (Rosetta)  | H.265 Main   | 19.5 ms *software* | **8.3 ms** hardware |
| x86_64 (Rosetta)  | H.265 Main10 | 33.6 ms *software* | **8.2 ms** hardware |
| x86_64 (Rosetta)  | H.264 Main   | 7.8 ms hardware   | 8.0 ms hardware     |
| arm64 (native)    | H.265 Main   | 8.4 ms hardware   | 8.3 ms hardware     |
| arm64 (native)    | H.265 Main10 | 8.4 ms hardware   | 8.6 ms hardware     |
| arm64 (native)    | H.264 Main   | 8.2 ms hardware   | 7.9 ms hardware     |

The IPC round trip itself costs **0.03–0.06 ms** — the boundary is not the cost.
The helper is worth it exactly where the in-process encoder is software, and
**not** where it is already hardware: on native arm64, and for H.264 under
Rosetta, the two paths are the same speed and the helper only adds a process.
That is what the selection policy below encodes. (The software HEVC numbers here
are for synthetic content; real frames measured 27–40 ms.)

## Architecture

```
  x86_64 runtime dylib (Rosetta)                 native-arm64 helper process
  ------------------------------                 ---------------------------
  VideoEncoder                                    main.mm
    Metal compose -> IOSurface slot   ── mach ──►  IOSurfaceLookupFromMachPort
    EncoderHelperClient                        CVPixelBufferCreateWithIOSurface
      posix_spawn(helper)                          VTCompressionSession
      bootstrap_check_in(name)                       RequireHardware = YES
      send Init         ───────── unix socket ─────► read Init
      recv child port   ◄──────── mach ────────────  send child port
      send N surfaces   ───────── mach ────────────► recv + wrap surfaces
      recv InitAck(hw)  ◄──────── unix socket ──────  create session, reply
      SubmitFrame(slot) ───────── unix socket ─────► VTCompressionSessionEncodeFrame
      onNal / onDone    ◄──────── unix socket ──────  Annex-B NAL units + metrics
```

* **Control + frame submission + encoded results:** a Unix stream socket
  (`EncoderHelperIpc.h`, explicit little-endian framing).
* **IOSurface transfer:** Mach send rights (`IOSurfaceCreateMachPort` →
  `IOSurfaceLookupFromMachPort`), transferred once at startup over a bootstrap
  rendezvous. The parent `bootstrap_check_in`s a per-spawn name *before*
  spawning; the child `bootstrap_look_up`s it (name passed via argv) and hands
  the parent a send right so surfaces can be pushed to it.
* **Lifecycle:** the client object is released only after `VideoEncoder`'s
  callback drain is empty, and every frame still in flight to a helper that
  stopped or died is finalized as dropped so its slot and drain lease are
  returned — a mid-session helper crash degrades to the in-process encoder
  rather than starving it of slots.
* **No SIGPIPE, either way.** The runtime is a library inside the game's
  process, so a write to a helper that has died must not raise SIGPIPE (whose
  default action would kill the game). Every control-socket descriptor gets
  `SO_NOSIGPIPE` and every write goes through `send(..., MSG_NOSIGNAL)`
  (`enc_ipc::SendAll`); a dead peer is then just `EPIPE`, which marks the helper
  dead and falls back. The runtime never installs a process-wide
  `signal(SIGPIPE, SIG_IGN)` in its host. The helper, being its own process,
  does ignore SIGPIPE, so a host that vanishes ends it through EOF rather than a
  signal (that also covers its stderr pipe). A dead helper's socket is shut down,
  not closed, until `Stop()` has joined the reader thread, so the reader can
  never end up reading a descriptor number the host has since reused.
* **Frame ownership.** Publishing a frame's context to the in-flight map hands
  it to the helper's reader thread, which may finalize it at once (FrameDone, or
  helper death). Everything the submit needs is read before publishing; a frame
  whose submit was never written is reclaimed by its cookie, a per-encoder
  sequence number rather than the context's address.
* **Timestamps** cross the boundary as int64 ns the parent supplies and the
  child echoes back; the child never generates a timestamp the parent compares
  (mach clocks are not comparable across the Rosetta boundary). Encode duration
  is measured entirely within the helper and reported as milliseconds.

> Do **not** switch the rendezvous to
> `posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT)`: libxpc latches the
> bootstrap port during libSystem init, before `main()`, and the child's first
> XPC-touching call (IOSurface lookup / VideoToolbox) then hangs forever. The
> child must use the normally inherited bootstrap port.

## Colour

Both sessions apply one colour contract, defined once in
`EncoderSessionColor.h`: BT.709 primaries, transfer function and YCbCr matrix,
for every codec and profile (Main10 included; it is a 10-bit bitstream from the
same 8-bit SDR source, not HDR). VideoToolbox writes these into the SPS VUI and
uses the matching matrix for its RGB-to-YCbCr conversion. The helper once set
none of them and emitted an SPS with no VUI at all, so its stream could decode
with different colours from the in-process one; `TestEncoderHelperClient.mm`
now parses the helper's SPS and checks the colour description.

Every path signals video (limited) range. VideoToolbox offers no property for
the range flag; a session takes it from its source. Every hardware encoder (the
helper's, and the in-process one on arm64 or for H.264 under Rosetta) writes
video range for the BGRA compose surface. The software HEVC encoder, which a
Rosetta process falls back to when the helper dies, writes **full** range for a
BGRA source once HEVC Main is requested. So an in-process software session is
never handed BGRA: each frame is first converted with a `VTPixelTransferSession`
into an explicitly video-range `420YpCbCr8BiPlanarVideoRange` buffer with the
same BT.709 matrix (about 0.3 ms at 2272x1264 under Rosetta, against 20-34 ms
for the software encode itself). The helper is hardware-only and needs no
conversion. `TestVideoToolboxEncoder.mm` pins a software session on any machine
and checks the range for H.264, HEVC Main and HEVC Main10, and checks it again
on the frames encoded after the helper is killed mid-stream.

## Build

The helper is part of the standard build. `runtime/CMakeLists.txt` adds this
directory, and the `oxrsys-encoder-helper` target:

* is **always arm64**, whatever `CMAKE_OSX_ARCHITECTURES` the runtime uses
  (`x86_64`, `arm64` or universal), via the target's own `OSX_ARCHITECTURES`;
* is written next to `liboxrsys-runtime.dylib` (`build/<preset>/runtime/`),
  where the runtime looks for it by default;
* is a dependency of `oxrsys_runtime`, so `cmake --build` and
  `cmake --build … --target oxrsys_runtime` (what the packaging scripts run)
  both produce it;
* is ad-hoc signed after linking.

```sh
cmake --preset macos-x64
cmake --build build/macos-x64 --target oxrsys_runtime
lipo -archs build/macos-x64/runtime/liboxrsys-runtime.dylib   # x86_64
lipo -archs build/macos-x64/runtime/oxrsys-encoder-helper     # arm64
```

`scripts/macos_build_package.sh` copies it into the package as
`runtime/oxrsys-encoder-helper` and refuses a helper that is not arm64-only;
`scripts/macos_sign_notarize.sh` signs it with the hardened runtime and ships
it in the archive. ctest runs `oxrsys_videotoolbox_encoder_tests` with
`OXRSYS_ENCODER_HELPER_PATH` pointing at the build's own helper.

To iterate on the helper alone, either of these still works:

```sh
runtime/encoder_helper/build-helper.sh              # -> build/helper/oxrsys-encoder-helper
cmake -S runtime/encoder_helper -B build/helper -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/helper
```

## Selection policy

`runtime/src/EncoderPathPolicy.{h,cpp}` decides, once per session, which process
encodes. It is a pure function of (negotiated codec, profile, whether **this
process** can actually get a hardware encoder for that codec, `encoder_helper`
override), so it is unit-tested without a GPU in
`tests/TestEncoderPathPolicy.cpp`.

Hardware availability is **asked of VideoToolbox**, never inferred from the
process architecture: `VTCopyVideoEncoderList` is filtered by
`kVTVideoEncoderList_CodecType` + `IsHardwareAccelerated`, falling back to an
actual `RequireHardware=YES` session probe if the list is unavailable. If Apple
changes what Rosetta is granted, the policy follows without a code change.

| Situation                                  | Path       |
| ------------------------------------------ | ---------- |
| Hardware encoder available in this process | in-process |
| No hardware encoder for this codec here    | **helper** |
| Codec the helper cannot encode (AV1)       | in-process |
| `encoder_helper = true` / `false`          | forced     |

Because the in-process path is only chosen when hardware is actually available,
the session is now created with
`RequireHardwareAcceleratedVideoEncoder = YES` in that case — the unconditional
`NO` is what made the Rosetta software fallback silent. If the create still
fails it is retried without the requirement and warns loudly, so a session is
never lost over it, and the runtime logs whether the session it ended up with is
hardware or software either way.

## Enable

Automatic by default. In `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`:

```toml
encoder_helper = "auto"    # "true" / "false" force it on or off for debugging
# encoder_helper_path = ""   # empty = sibling of the runtime dylib
```

The build and the package already put `oxrsys-encoder-helper` next to
`liboxrsys-runtime.dylib`. When installing by hand, copy it alongside the dylib
(or set `encoder_helper_path` / `$OXRSYS_ENCODER_HELPER_PATH`); it must stay
signed (the build ad-hoc signs it; release packaging uses a Developer ID).

The runtime log then shows the decision and its reason, e.g.
`VideoEncoder: out-of-process native-arm64 helper encode path for H.265: no
in-process hardware encoder (this process is refused a hardware encoder for the
negotiated codec)`, followed by
`EncoderHelper: ready — hardware H.265 encoder live in native-arm64 helper`.
On any failure it logs where it broke and continues on the in-process encoder.

## Offline test / benchmark

`smoke_test.mm` reproduces the whole handoff without Wine/the game *and* measures
both paths: it creates IOSurface-backed BGRA buffers, encodes them in-process,
then spawns the arm64 helper, performs the mach rendezvous + surface transfer and
encodes the same frames again — printing `hardware=YES` and the per-frame encode
time for each. Build it x86_64 to mimic the Rosetta runtime, arm64 for the
native-host numbers.

```sh
for arch in x86_64 arm64; do
  xcrun clang++ -arch $arch -std=c++17 -O2 runtime/encoder_helper/smoke_test.mm \
    -o build/helper/smoke_test_$arch \
    -framework Foundation -framework CoreFoundation -framework CoreVideo \
    -framework CoreMedia -framework IOSurface -framework VideoToolbox
  codesign --force --sign - build/helper/smoke_test_$arch
done
build/helper/smoke_test_x86_64 build/helper/oxrsys-encoder-helper
build/helper/smoke_test_arm64  build/helper/oxrsys-encoder-helper
#   --codec hevc|h264  --profile main|main10  --frames N
#   --skip-helper / --skip-in-process to measure one path alone
```

Beware when comparing paths by hand: the two VideoToolbox sessions must be
configured identically. An early version of this benchmark left
`DataRateLimits` and `MaxKeyFrameIntervalDuration` off the in-process session
only, and that alone made in-process look 3 ms/frame slower than the helper on
native arm64 — a property difference, not an encoder difference.

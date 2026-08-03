// SPDX-License-Identifier: MPL-2.0

// Generation-chaos fixture for NativeHelperEncoderTransport: a FAKE HELPER
// PEER (tests/FakeEncoderHelperMain.mm, spawned via OXRSYS_ENCODER_HELPER)
// plays the child side of the real wire protocol + Mach surface rendezvous
// well enough to drive the transport's exactly-once (generation, frameId)
// bookkeeping through: a stale-generation result racing a duplicate delivery
// for the current generation, AND a forged FrameResult that reuses the
// current generation's frameId under the OLD generation (pinning the
// (generation, frameId) COMPOUND key, not just a frameId-only map) —
// without needing real hardware encode or the production helper
// (oxrsys_encoder_helper). See FakeEncoderHelperMain.mm's header comment for
// the exact script this plays against.
//
// Consumption of the trailing stray results is proven by a causal barrier
// (transport->Drain(), see the inline comment at its call site below) rather
// than a fixed settle delay: the single serial socket plus each side's
// single-threaded reactive loop (fake) / reader loop (transport) guarantee
// DrainComplete cannot be dispatched before the strays are.
//
// May-skip condition (environment, not a regression): OXRSYS_ENCODER_HELPER
// not set (a plain `oxrsys_runtime_tests` run outside the
// oxrsys_native_helper_chaos ctest entry that points it at the fake helper).

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "encoder/NativeHelperEncoderTransport.h"

#include "EncoderTestSupport.h"

namespace
{

constexpr uint32_t kDim = 64; // content is never inspected — smallest legal size

/// Per-frame completion tracking: how many times onFrameComplete fired for
/// THIS frame's token, and whether it was ever reported dropped. Each
/// BeginFrame() call gets its own FrameCallbacks closure, so a stale or
/// duplicate result for a DIFFERENT frame can never touch these counters —
/// that separation is exactly what this fixture is asserting.
struct FrameOutcome
{
    std::mutex mutex;
    std::condition_variable condition;
    int completeCount = 0;
    int droppedCount = 0;
};

oxrsys::encoder::FrameCallbacks MakeCallbacks(FrameOutcome& outcome)
{
    oxrsys::encoder::FrameCallbacks callbacks;
    callbacks.onFrameComplete = [&outcome](const oxrsys::encoder::EncodedFrameMetrics& metrics)
    {
        std::lock_guard<std::mutex> lock(outcome.mutex);
        outcome.completeCount++;
        if (metrics.frameDropped)
        {
            outcome.droppedCount++;
        }
        outcome.condition.notify_all();
    };
    callbacks.releaseResources = [] {};
    return callbacks;
}

bool WaitForCount(FrameOutcome& outcome, int expected, std::chrono::seconds timeout)
{
    std::unique_lock<std::mutex> lock(outcome.mutex);
    return outcome.condition.wait_for(lock, timeout,
                                      [&] { return outcome.completeCount >= expected; });
}

/// Runs `fn` on a background thread and CHECKs (rather than hangs the suite)
/// if it doesn't finish within `timeout`. The teardown paths under test are
/// each internally bounded to roughly a second (kShutdownAckTimeoutMs +
/// kTermWaitMs, or StopForProcessExit's non-blocking WNOHANG-only reap), so
/// this is a generous safety net, not the primary bound — ctest's own
/// per-test TIMEOUT is the final backstop if even this watchdog wedges.
/// On timeout the worker thread is deliberately detached rather than joined:
/// a genuine hang must not also hang this test process. Any references `fn`
/// captured (e.g. into locals of the calling TEST_CASE) are then only safe
/// for as long as those locals happen to outlive the runaway thread — an
/// accepted trade-off for a path that is not expected to ever trigger.
template <typename Fn>
void RunWithWatchdog(Fn&& fn, std::chrono::seconds timeout, const char* what)
{
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> future = done->get_future();
    std::thread worker(
        [fn = std::forward<Fn>(fn), done]() mutable
        {
            fn();
            done->set_value();
        });
    const bool finished = future.wait_for(timeout) == std::future_status::ready;
    // Detach/join BEFORE the fail-fast REQUIRE below: REQUIRE throws on
    // failure, and that throw must never skip past reclaiming the thread
    // handle (leaked-but-still-joinable is UB at worker's destructor).
    if (finished)
    {
        worker.join();
    }
    else
    {
        worker.detach();
    }
    INFO(what);
    // Fail-fast: a fired watchdog means the transport is wedged, so the
    // TEST_CASE must abort here rather than continue into further SECTIONs
    // against a transport already known to be broken. This is safe to make
    // fatal because the oxrsys_native_helper_chaos ctest entry runs this
    // suite subset in its OWN process (see CMakeLists.txt) — a detached,
    // permanently wedged worker thread here cannot poison any other ctest
    // entry.
    REQUIRE(finished);
}

} // namespace

TEST_CASE("Native helper transport resolves a stale-generation result and drops a duplicate "
          "exactly once",
          "[helper-chaos]")
{
    const char* helperPath = getenv("OXRSYS_ENCODER_HELPER");
    if (helperPath == nullptr || helperPath[0] == '\0')
    {
        SKIP("OXRSYS_ENCODER_HELPER not set (run the oxrsys_native_helper_chaos ctest entry)");
    }
    if (access(helperPath, X_OK) != 0)
    {
        SKIP("fake helper binary not present/executable: " << helperPath);
    }

    auto transport = std::make_unique<oxrsys::encoder::NativeHelperEncoderTransport>();
    oxrsys::encoder::EncoderConfig config;
    config.width = kDim;
    config.height = kDim;
    config.fps = 72;
    config.bitrateMbps = 5;
    config.codec = oxr::protocol::VideoCodec::H264;
    config.keyframeIntervalSec = 5;

    // --- Generation G: submit frame A. The fake helper HOLDS its
    // FrameResult (FakeEncoderHelperMain.mm's script) instead of replying.
    REQUIRE(transport->Configure(config));
    REQUIRE(transport->IsHealthy());

    CVPixelBufferRef bufferA = MakeSurfaceBackedBuffer(kDim, /*metalCompatible=*/false);
    REQUIRE(bufferA != nullptr);
    REQUIRE(CVPixelBufferGetIOSurface(bufferA) != nullptr);

    FrameOutcome outcomeA;
    auto tokenA = transport->BeginFrame(MakeCallbacks(outcomeA), {});
    REQUIRE(tokenA != nullptr);
    REQUIRE(tokenA->Submit(bufferA, /*timestampNs=*/0, /*forceKeyframe=*/false));

    // --- Reconfigure to generation G+1 while frame A is still outstanding
    // (deliberately skipping the normal Drain-before-reconfigure contract —
    // that's the chaos this fixture targets). Configure() is synchronous
    // (blocks for ConfigureAck), and frame A's FrameSubmit was fully
    // enqueued to the transport's FIFO frame queue before this call even
    // starts, so frame A's own generation identity is fixed regardless of
    // how the wire interleaves it with this ConfigureGeneration.
    REQUIRE(transport->Configure(config));
    REQUIRE(transport->IsHealthy());

    CVPixelBufferRef bufferB = MakeSurfaceBackedBuffer(kDim, /*metalCompatible=*/false);
    REQUIRE(bufferB != nullptr);
    REQUIRE(CVPixelBufferGetIOSurface(bufferB) != nullptr);

    FrameOutcome outcomeB;
    auto tokenB = transport->BeginFrame(MakeCallbacks(outcomeB), {});
    REQUIRE(tokenB != nullptr);
    // This is the submit that makes the fake helper release, back to back:
    // (a) frame B's own normal FrameResult, (b) the held stale-G result for
    // frame A, (c) a duplicate of (a), (d) a forged FrameResult tagged
    // generation G (stale) but carrying B's own frameId.
    REQUIRE(tokenB->Submit(bufferB, /*timestampNs=*/1, /*forceKeyframe=*/false));

    // Frame B must complete — exactly once, even with a duplicate FrameResult
    // for it in flight right behind the real one.
    REQUIRE(WaitForCount(outcomeB, 1, std::chrono::seconds(10)));
    // Frame A's own (merely late) result must also land, exactly once.
    REQUIRE(WaitForCount(outcomeA, 1, std::chrono::seconds(10)));

    // --- Causal barrier instead of a blind sleep-then-assert.
    //
    // By this point the fake has already sent, back to back, in response to
    // frame B's Submit: (a) B's normal FrameResult, (b) the held stale-G
    // result for A, (c) a duplicate of (a), and (d) a FORGED FrameResult
    // tagged with the OLD generation G but frame B's OWN frameId (which
    // exists only in G+1) — see FakeEncoderHelperMain.mm's script. The two
    // REQUIRE(WaitForCount(...)) above only prove (a) and (b) were
    // dispatched; (c) and (d) are the ones actually under test below, and
    // nothing yet proves the reader has gotten to them.
    //
    // The fake's Drain handler only fires after it has read this Drain
    // request off the wire, which — because the socket is a single serial
    // stream and the fake is a single reactive loop that fully sends (a)-(d)
    // before it ever loops back to read the next incoming message — cannot
    // happen until (a)-(d) are already written. Symmetrically,
    // NativeHelperEncoderTransport::ReaderLoop is one thread that reads and
    // fully dispatches one message at a time in strict wire order, so it
    // cannot dispatch DrainComplete (waking the Drain() call below) until it
    // has already dispatched (c) and (d) through OnFrameResult/TakeOutstanding
    // — dropped or not. That causal chain is what makes Drain() a valid
    // barrier here in place of a fixed settle delay.
    transport->Drain();
    // The chaos exchange above (including the forged cross-generation
    // frameId) must never have knocked the link down: a dead link here would
    // mean the fake hit a protocol violation or crashed instead of politely
    // dropping the strays as scripted, and the fake is only expected to exit
    // once it later receives Shutdown (below).
    REQUIRE(transport->IsHealthy());

    {
        std::lock_guard<std::mutex> lock(outcomeB.mutex);
        // Exactly-once: (c) the duplicate FrameResult and (d) the forged
        // (old-generation, B's-frameId) FrameResult were both discarded.
        // (d) specifically pins TakeOutstanding's (generation, frameId)
        // COMPOUND key — a frameId-only map would have matched B's entry
        // regardless of the wrong generation and mis-delivered a second (or
        // third) completion here.
        CHECK(outcomeB.completeCount == 1);
        CHECK(outcomeB.droppedCount == 0);
    }
    {
        std::lock_guard<std::mutex> lock(outcomeA.mutex);
        // The stale result is frame A's own, legitimate — if late —
        // completion, not a drop, and it must never touch outcomeB's count
        // above.
        CHECK(outcomeA.completeCount == 1);
        CHECK(outcomeA.droppedCount == 0);
    }

    CVPixelBufferRelease(bufferA);
    CVPixelBufferRelease(bufferB);

    SECTION("Shutdown() completes without hanging")
    {
        // The REQUIRE(transport->IsHealthy()) above already proved the link
        // was still up (fake still running, script fully played) going into
        // this SECTION. Shutdown()'s own contract only runs its
        // Drain -> Shutdown/ShutdownAck -> teardown exchange when entered
        // with the link alive; the fake replies ShutdownAck and only then
        // exits cleanly (exitCode 0 unless a read error occurred — see
        // FakeEncoderHelperMain.mm's main loop). So completing within the
        // watchdog bound (no hang) plus flipping unhealthy here — rather
        // than having already gone unhealthy earlier via HandleLinkDown's
        // EOF/crash path — is exactly the fake's normal, requested,
        // full-script-then-clean-exit path being observed from the
        // transport's side.
        RunWithWatchdog([&] { transport->Shutdown(); }, std::chrono::seconds(10),
                        "NativeHelperEncoderTransport::Shutdown() hung");
        CHECK_FALSE(transport->IsHealthy());
    }

    SECTION("StopForProcessExit() completes without hanging")
    {
        // Fold the subsequent destructor-driven teardown (the reader/writer
        // thread joins StopForProcessExit() itself deliberately skips) into
        // the same watchdog bound, by destroying the transport inside it.
        RunWithWatchdog(
            [&]
            {
                transport->StopForProcessExit();
                transport.reset();
            },
            std::chrono::seconds(10), "NativeHelperEncoderTransport::StopForProcessExit() hung");
    }
}

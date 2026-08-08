// SPDX-License-Identifier: MPL-2.0

// oxrsys-encoder-helper-chaos-fake: a SCRIPTED stand-in for the native-arm64
// out-of-process encoder helper, used only by tests/TestNativeHelperChaos.mm
// ([helper-chaos]) to drive NativeHelperEncoderTransport's generation /
// exactly-once bookkeeping through a stale-result-racing-a-duplicate
// scenario without needing real hardware encode. It speaks the exact wire
// protocol (EncoderIpcProtocol.h / EncoderIpcSocket.h) and the real Mach
// surface rendezvous (EncoderMachSurface.h) so the transport under test
// never knows it isn't talking to the production helper — only the
// *content* of the replies is canned, and no VideoToolbox/Metal dependency
// exists here at all.
//
// Spawned exactly like the real helper (NativeHelperEncoderTransport's
// LocateHelperBinary() honors OXRSYS_ENCODER_HELPER, which the
// oxrsys_native_helper_chaos ctest entry points at this binary):
//   oxrsys-encoder-helper-chaos-fake --socket-fd <N> --bootstrap-name <name>
//
// Script (purely reactive — no side channel besides the wire itself; the
// two Configure()/Submit() pairs the test drives are distinguished only by
// MESSAGE ORDER, which is all a real peer could ever observe):
//   1. Hello                    -> HelloReply (arm64, translated=0, both
//                                   codecs report hardware+low-latency caps
//                                   so Configure() never has to care which
//                                   codec the test picked)
//   2. ConfigureGeneration #1   -> ConfigureAck(ok)            [generation G]
//   3. FrameSubmit #1 (gen G)   -> HELD: no reply yet (the chaos fixture)
//   4. ConfigureGeneration #2   -> ConfigureAck(ok)          [generation G+1]
//   5. FrameSubmit #2 (gen G+1) -> four replies, back to back, in this
//                                   order:
//                                     a) FrameResult for THIS submit — the
//                                        normal G+1 completion
//                                     b) the FrameResult HELD at step 3 —
//                                        now stale, delivered after the
//                                        reconfigure raced right past it
//                                     c) a DUPLICATE of (a): same
//                                        (generation, frameId), proving the
//                                        transport's TakeOutstanding
//                                        discipline drops it
//                                     d) a FORGED FrameResult: the OLD
//                                        generation G (this submit's
//                                        generation minus one) but THIS
//                                        submit's own frameId — a frameId
//                                        that exists only in G+1, never in
//                                        G. Only a genuine (generation,
//                                        frameId) COMPOUND key can reject
//                                        this; a frameId-only map would
//                                        mis-deliver it as a second (or
//                                        third) completion for this frame.
//   6. Drain                    -> DrainComplete (nothing left outstanding)
//   7. Shutdown                 -> ShutdownAck, then a clean exit
//   Socket EOF at any point (e.g. StopForProcessExit()'s abrupt teardown) is
//   itself a clean-exit trigger, exactly like the production helper.
//
// Surface registrations are drained on a background thread purely so the
// parent's Mach sends never queue up unbounded; the received IOSurfaceRef is
// released immediately — this fixture never touches pixel data, only the
// (generation, frameId) identity that rides the socket.

#include <CoreFoundation/CoreFoundation.h>

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "encoder/EncoderIpcProtocol.h"
#include "encoder/EncoderIpcSocket.h"
#include "encoder/EncoderMachSurface.h"

namespace ipc = oxrsys::encoder::ipc;
namespace mach_surface = oxrsys::encoder::mach_surface;

namespace
{

// Drains Mach surface-transfer messages so ParentSurfaceRendezvous::SendSurface
// never has to rely on the receive port's default queue depth; the tag is
// never inspected because this fixture never encodes anything.
void DrainSurfaces(mach_surface::ChildSurfaceRendezvous& rendezvous, std::atomic<bool>& stop)
{
    while (!stop.load())
    {
        void* surface = nullptr;
        mach_surface::SurfaceTag tag;
        if (rendezvous.ReceiveSurface(&surface, tag, 100 /*ms*/))
        {
            CFRelease((CFTypeRef)surface);
        }
    }
}

// One canned Annex-B-shaped IDR NAL. Content is never inspected by the test
// (the chaos fixture is about (generation, frameId) identity, not pixels) —
// it only has to Deserialize successfully on the parent side.
ipc::FrameResult MakeCannedResult(const ipc::FrameSubmit& submit)
{
    ipc::FrameResult wire;
    wire.generation = submit.generation;
    wire.frameId = submit.frameId;
    wire.status = 0;
    wire.flags = ipc::kFrameResultFlagIsIdr;
    wire.encodeStartNs = submit.composedAtNs;
    wire.callbackAtNs = submit.composedAtNs + 1000000; // +1ms, arbitrary
    static const uint8_t kAnnexBIdr[] = {0, 0, 0, 1, 0x65, 0xAB, 0xCD};
    wire.data.assign(kAnnexBIdr, kAnnexBIdr + sizeof(kAnnexBIdr));
    wire.nalUnits.push_back({0, (uint32_t)sizeof(kAnnexBIdr)});
    return wire;
}

} // namespace

int main(int argc, char** argv)
{
    // The parent may vanish at any time (StopForProcessExit's abrupt
    // teardown); writes then fail with EPIPE, handled via the socket layer's
    // EOF/Error result instead of killing this process.
    signal(SIGPIPE, SIG_IGN);

    int socketFd = -1;
    std::string bootstrapName;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--socket-fd") == 0 && i + 1 < argc)
        {
            socketFd = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--bootstrap-name") == 0 && i + 1 < argc)
        {
            bootstrapName = argv[++i];
        }
        else
        {
            socketFd = -1; // force the single check below to reject
            break;
        }
    }
    if (socketFd < 0 || bootstrapName.empty())
    {
        fprintf(stderr, "usage: %s --socket-fd N --bootstrap-name NAME\n", argv[0]);
        return 64;
    }

    ipc::EncoderIpcSocket socket;
    socket.Adopt(socketFd);
    ipc::EncoderIpcSocket::SetNoSigPipe(socketFd);

    mach_surface::ChildSurfaceRendezvous rendezvous;
    if (!rendezvous.CheckIn(bootstrapName, 5000))
    {
        fprintf(stderr, "oxrsys-encoder-helper-chaos-fake: bootstrap rendezvous '%s' failed\n",
                bootstrapName.c_str());
        return 2;
    }

    std::atomic<bool> stopSurfaceDrain{false};
    std::thread surfaceDrainThread(
        [&] { DrainSurfaces(rendezvous, stopSurfaceDrain); });

    uint64_t sequence = 0;
    const auto Send = [&](ipc::MessageType type, const std::vector<uint8_t>& payload)
    { socket.WriteMessage(type, 0, sequence++, payload); };

    // Step 3's held reply, released at step 5.
    bool haveHeldFrame = false;
    std::vector<uint8_t> heldFrameResultPayload;

    int exitCode = 0;
    for (;;)
    {
        ipc::MessageHeader header;
        std::vector<uint8_t> payload;
        const ipc::IoResult result = socket.ReadMessage(header, payload);
        if (result == ipc::IoResult::Eof)
        {
            break; // parent gone — clean exit, exactly like the real helper
        }
        if (result != ipc::IoResult::Ok)
        {
            exitCode = 3;
            break;
        }

        switch ((ipc::MessageType)header.type)
        {
            case ipc::MessageType::Hello:
            {
                ipc::HelloReply reply;
                reply.arch = ipc::kArchArm64;
                reply.translated = 0;
                reply.capsH264 = ipc::kCodecCapHardware | ipc::kCodecCapLowLatency;
                reply.capsH265 = ipc::kCodecCapHardware | ipc::kCodecCapLowLatency;
                reply.macosMajor = 27;
                reply.helperPid = (uint32_t)getpid();
                std::vector<uint8_t> out;
                reply.Serialize(out);
                Send(ipc::MessageType::HelloReply, out);
                break;
            }
            case ipc::MessageType::ConfigureGeneration:
            {
                ipc::ConfigureGeneration configure;
                ipc::ConfigureAck ack;
                if (ipc::ConfigureGeneration::Deserialize(payload.data(), payload.size(),
                                                           configure))
                {
                    ack.generation = configure.generation;
                    ack.status = 0;
                }
                else
                {
                    ack.status = (uint32_t)-1;
                }
                std::vector<uint8_t> out;
                ack.Serialize(out);
                Send(ipc::MessageType::ConfigureAck, out);
                break;
            }
            case ipc::MessageType::FrameSubmit:
            {
                ipc::FrameSubmit submit;
                if (!ipc::FrameSubmit::Deserialize(payload.data(), payload.size(), submit))
                {
                    break; // unparseable: no frame identity to script a reply to
                }
                const ipc::FrameResult wire = MakeCannedResult(submit);
                std::vector<uint8_t> resultPayload;
                wire.Serialize(resultPayload);

                if (!haveHeldFrame)
                {
                    // Step 3: hold.
                    haveHeldFrame = true;
                    heldFrameResultPayload = std::move(resultPayload);
                    break;
                }

                // Step 5: second submit. (d) is a FORGED wire message this
                // fake fabricates directly (no corresponding real submit
                // under generation G ever carried B's frameId) — it exists
                // purely to probe TakeOutstanding's (generation, frameId)
                // compound key on the transport side.
                ipc::FrameResult forged = wire;
                forged.generation = submit.generation - 1; // stale G, not G+1
                std::vector<uint8_t> forgedPayload;
                forged.Serialize(forgedPayload);

                Send(ipc::MessageType::FrameResult, resultPayload);          // (a) normal G+1
                Send(ipc::MessageType::FrameResult, heldFrameResultPayload); // (b) stale G
                Send(ipc::MessageType::FrameResult, resultPayload);          // (c) duplicate G+1
                Send(ipc::MessageType::FrameResult, forgedPayload); // (d) forged: gen G, B's frameId
                break;
            }
            case ipc::MessageType::Drain:
            {
                ipc::Drain drain;
                ipc::Drain::Deserialize(payload.data(), payload.size(), drain);
                ipc::DrainComplete complete;
                complete.generation = drain.generation;
                std::vector<uint8_t> out;
                complete.Serialize(out);
                Send(ipc::MessageType::DrainComplete, out);
                break;
            }
            case ipc::MessageType::Shutdown:
            {
                Send(ipc::MessageType::ShutdownAck, {});
                goto out; // orderly: drain the surface thread and exit below
            }
            default:
                break; // additive minor-version tolerance, mirrors the real helper
        }
    }
out:
    stopSurfaceDrain.store(true);
    if (surfaceDrainThread.joinable())
    {
        surfaceDrainThread.join();
    }
    rendezvous.Close();
    socket.Close();
    return exitCode;
}

// SPDX-License-Identifier: MPL-2.0
//
// oxrsys_pacing_probe: loopback measurement of the video sender's packet pacing.
//
// Sends a synthetic stream (default 90 fps, 50 Mbps, P frames 20-115 kB, one
// 300 kB keyframe per second) packetised exactly like StreamingServer::SendNalUnit
// (VideoPacketHeader + 1400 B payload, one parity packet per FEC_GROUP_SIZE) to a
// receiver socket on 127.0.0.1 in the same process, paced by PacketPacer, and
// reports burst length, inter-packet spacing, peak 1 ms rate and added latency.
// Loopback only by design: it never sends to another host.
//
//   oxrsys_pacing_probe --window-ms 0            # today's burst
//   oxrsys_pacing_probe --window-ms 4 --keyframe-ms 8 --csv out.csv

#include "PacketPacer.h"

#include <Protocol.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace oxrsys::streaming;
namespace proto = oxr::protocol;

namespace
{

    struct Options
    {
        float windowMs = 0.0f;
        float keyframeMs = 0.0f;
        double seconds = 5.0;
        uint32_t fps = 90;
        double mbps = 50.0;
        uint32_t seed = 1;
        std::string csv;
        // AP queue model (not a measurement): drain rate and queue depth.
        double modelDrainMbps = 150.0;
        uint32_t modelQueuePackets = 48;
    };

    struct SendRecord
    {
        uint32_t frame;
        uint16_t index;
        uint8_t flags;
        bool failed;
        int64_t sendNs;
        uint32_t bytes;
    };

    struct RecvRecord
    {
        uint32_t frame;
        uint16_t index;
        uint8_t flags;
        int64_t kernelUs;
        int64_t userNs;
    };

    struct FrameInfo
    {
        int64_t startNs = 0;
        int64_t lastSendNs = 0;
        uint32_t packets = 0;
        bool keyframe = false;
        bool flushed = false;
        int64_t maxLatenessNs = 0;
    };

    double Percentile(std::vector<double> v, double p)
    {
        if (v.empty())
        {
            return 0.0;
        }
        std::sort(v.begin(), v.end());
        const size_t i = std::min(v.size() - 1, static_cast<size_t>(std::llround(p * (v.size() - 1))));
        return v[i];
    }

    void Usage()
    {
        std::fprintf(stderr, "usage: oxrsys_pacing_probe [--window-ms W] [--keyframe-ms K] [--seconds S]\n"
                             "       [--fps F] [--mbps M] [--seed N] [--csv path]\n"
                             "       [--model-drain-mbps R] [--model-queue-packets Q]\n");
    }

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char*
        {
            if (i + 1 >= argc)
            {
                Usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--window-ms")
            opt.windowMs = std::stof(next());
        else if (a == "--keyframe-ms")
            opt.keyframeMs = std::stof(next());
        else if (a == "--seconds")
            opt.seconds = std::stod(next());
        else if (a == "--fps")
            opt.fps = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--mbps")
            opt.mbps = std::stod(next());
        else if (a == "--seed")
            opt.seed = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--csv")
            opt.csv = next();
        else if (a == "--model-drain-mbps")
            opt.modelDrainMbps = std::stod(next());
        else if (a == "--model-queue-packets")
            opt.modelQueuePackets = static_cast<uint32_t>(std::stoul(next()));
        else
        {
            Usage();
            return 2;
        }
    }

    // ── Receiver on loopback ────────────────────────────────────────────────
    const int rx = socket(AF_INET, SOCK_DGRAM, 0);
    const int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0 || tx < 0)
    {
        std::perror("socket");
        return 1;
    }
    int rcvbuf = 0;
    for (int want = 8 * 1024 * 1024; want >= 256 * 1024; want /= 2)
    {
        if (setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want)) == 0)
        {
            rcvbuf = want;
            break;
        }
    }
    int sndbuf = 4 * 1024 * 1024; // as StreamingServer sets on the video socket
    setsockopt(tx, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int one = 1;
    setsockopt(rx, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one));
    timeval rcvTimeout{0, 200'000};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::perror("bind");
        return 1;
    }
    socklen_t addrLen = sizeof(addr);
    getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
    {
        std::fprintf(stderr, "refusing: destination is not loopback\n");
        return 1;
    }

    std::vector<RecvRecord> received;
    received.reserve(1 << 20);
    std::atomic<bool> stop{false};
    std::thread receiver(
        [&]
        {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            uint8_t buf[proto::VIDEO_PACKET_SIZE + 64];
            alignas(cmsghdr) uint8_t control[256];
            while (!stop.load())
            {
                iovec iov{buf, sizeof(buf)};
                msghdr msg{};
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = control;
                msg.msg_controllen = sizeof(control);
                const ssize_t n = recvmsg(rx, &msg, 0);
                const int64_t userNs = PacingNowNs();
                if (n < static_cast<ssize_t>(sizeof(proto::VideoPacketHeader)))
                {
                    continue;
                }
                int64_t kernelUs = 0;
                for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
                {
                    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP)
                    {
                        timeval tv{};
                        std::memcpy(&tv, CMSG_DATA(c), sizeof(tv));
                        kernelUs = static_cast<int64_t>(tv.tv_sec) * 1'000'000 + tv.tv_usec;
                    }
                }
                proto::VideoPacketHeader h{};
                std::memcpy(&h, buf, sizeof(h));
                received.push_back({h.frameIndex, h.packetIndex, h.flags, kernelUs, userNs});
            }
        });

    // ── Sender: synthetic 90 fps stream, packetised like SendNalUnit ────────
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    PacketPacingConfig pacing;
    pacing.windowMs = opt.windowMs;
    pacing.keyframeWindowMs = opt.keyframeMs;

    const int64_t periodNs = 1'000'000'000LL / opt.fps;
    const uint32_t frames = static_cast<uint32_t>(opt.seconds * opt.fps);
    // P frames uniform in [lo, hi]; one 300 kB keyframe per second. Scale P so
    // the mean hits the target bitrate.
    const double keyBytes = 300'000.0;
    const double meanBytes = opt.mbps * 1e6 / 8.0 / opt.fps;
    const double pMean = (meanBytes * opt.fps - keyBytes) / (opt.fps - 1);
    const double pLo = std::max(15'000.0, pMean - 47'500.0);
    const double pHi = pMean + (pMean - pLo);
    std::mt19937 rng(opt.seed);
    std::uniform_real_distribution<double> pSize(pLo, pHi);

    std::vector<uint8_t> payload(400'000);
    for (size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>(rng());
    }

    std::vector<SendRecord> sent;
    sent.reserve(1 << 20);
    std::vector<FrameInfo> frameInfo(frames);
    uint8_t packet[proto::VIDEO_PACKET_SIZE];
    uint64_t totalBytes = 0;

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // receiver up
    const int64_t t0 = PacingNowNs() + 10'000'000;
    for (uint32_t f = 0; f < frames; ++f)
    {
        const int64_t frameDue = t0 + static_cast<int64_t>(f) * periodNs;
        WaitUntilNs(frameDue);

        const bool keyframe = (f % opt.fps) == 0;
        const size_t size = keyframe ? static_cast<size_t>(keyBytes) : static_cast<size_t>(pSize(rng));
        const uint32_t dataPackets =
            static_cast<uint32_t>((size + proto::MAX_PACKET_PAYLOAD - 1) / proto::MAX_PACKET_PAYLOAD);
        const uint32_t fecPackets = (dataPackets + proto::FEC_GROUP_SIZE - 1) / proto::FEC_GROUP_SIZE;

        FrameInfo& info = frameInfo[f];
        info.startNs = PacingNowNs();
        info.keyframe = keyframe;
        info.packets = dataPackets + fecPackets;
        PacketPacer pacer(info.startNs, PacingWindowNs(pacing, keyframe, opt.fps), info.packets);
        // A newer frame is ready once the next frame's slot has begun.
        const int64_t nextDue = frameDue + periodNs;
        auto newerFrameWaiting = [&] { return PacingNowNs() >= nextDue; };

        auto sendOne = [&](uint16_t index, uint8_t flags, size_t bytes, const uint8_t* src)
        {
            pacer.WaitForNextPacket(newerFrameWaiting);
            proto::VideoPacketHeader h{};
            h.frameIndex = f;
            h.packetIndex = index;
            h.totalPackets = static_cast<uint16_t>(dataPackets);
            h.payloadSize = static_cast<uint16_t>(bytes);
            h.flags = flags;
            h.presentationTimeNs = info.startNs;
            std::memcpy(packet, &h, sizeof(h));
            std::memcpy(packet + sizeof(h), src, bytes);
            const int64_t now = PacingNowNs();
            const ssize_t r =
                sendto(tx, packet, sizeof(h) + bytes, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            sent.push_back({f, index, flags, r < 0, now, static_cast<uint32_t>(sizeof(h) + bytes)});
            totalBytes += sizeof(h) + bytes;
            info.lastSendNs = now;
        };

        size_t offset = 0;
        for (uint32_t i = 0; i < dataPackets; ++i)
        {
            const size_t bytes = std::min(size - offset, proto::MAX_PACKET_PAYLOAD);
            uint8_t flags = proto::VIDEO_FLAG_STEREO | (keyframe ? proto::VIDEO_FLAG_KEYFRAME : 0);
            if (i + 1 == dataPackets)
            {
                flags |= proto::VIDEO_FLAG_END_OF_FRAME;
            }
            sendOne(static_cast<uint16_t>(i), flags, bytes, payload.data() + offset);
            offset += bytes;
            if ((i + 1) % proto::FEC_GROUP_SIZE == 0 || i + 1 == dataPackets)
            {
                sendOne(static_cast<uint16_t>(i / proto::FEC_GROUP_SIZE),
                        proto::VIDEO_FLAG_FEC | proto::VIDEO_FLAG_STEREO, proto::MAX_PACKET_PAYLOAD, payload.data());
            }
        }
        info.flushed = pacer.Flushed();
        info.maxLatenessNs = pacer.MaxLatenessNs();
    }
    const int64_t tEnd = PacingNowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    receiver.join();

    // ── Analysis ────────────────────────────────────────────────────────────
    size_t sendFailures = 0;
    for (const SendRecord& s : sent)
    {
        sendFailures += s.failed ? 1 : 0;
    }

    // Per-frame arrival spans and gaps, from kernel receive timestamps (us).
    std::map<uint32_t, std::vector<int64_t>> arrivals;
    for (const RecvRecord& r : received)
    {
        arrivals[r.frame].push_back(r.kernelUs);
    }
    std::vector<double> spanP, spanK, gaps, addedP, addedK, lateness;
    size_t flushedFrames = 0;
    for (auto& [frame, ts] : arrivals)
    {
        std::sort(ts.begin(), ts.end());
        const double span = static_cast<double>(ts.back() - ts.front());
        (frameInfo[frame].keyframe ? spanK : spanP).push_back(span);
        for (size_t i = 1; i < ts.size(); ++i)
        {
            gaps.push_back(static_cast<double>(ts[i] - ts[i - 1]));
        }
    }
    for (const FrameInfo& fi : frameInfo)
    {
        const double added = (fi.lastSendNs - fi.startNs) / 1e6;
        (fi.keyframe ? addedK : addedP).push_back(added);
        lateness.push_back(fi.maxLatenessNs / 1e3);
        flushedFrames += fi.flushed ? 1 : 0;
    }

    // Peak rate in 1 ms bins (sender timestamps, ns) - what a queue downstream sees.
    std::map<int64_t, uint64_t> bins;
    for (const SendRecord& s : sent)
    {
        bins[(s.sendNs - t0) / 1'000'000] += s.bytes;
    }
    std::vector<double> binMbps;
    for (const auto& [bin, bytes] : bins)
    {
        binMbps.push_back(bytes * 8.0 / 1e3); // bytes per 1 ms -> Mbps
    }

    // AP queue model (NOT a measurement): FIFO of Q packets drained at R Mbps,
    // fed with the measured send times; tail drop when full.
    const double drainNsPerByte = 8.0 * 1e3 / opt.modelDrainMbps; // ns per byte
    std::vector<int64_t> departures;                              // departure times of packets in queue
    size_t head = 0;
    int64_t lastDepart = 0;
    size_t modelDrops = 0;
    std::map<uint32_t, uint32_t> modelDropsPerFrame;
    for (const SendRecord& s : sent)
    {
        while (head < departures.size() && departures[head] <= s.sendNs)
        {
            ++head;
        }
        if (departures.size() - head >= opt.modelQueuePackets)
        {
            ++modelDrops;
            modelDropsPerFrame[s.frame]++;
            continue;
        }
        const int64_t begin = std::max(lastDepart, s.sendNs);
        lastDepart = begin + static_cast<int64_t>(s.bytes * drainNsPerByte);
        departures.push_back(lastDepart);
    }
    // A frame is unrecoverable when some FEC group lost 2+ packets; approximate
    // with "2+ drops in the frame" (the drops of a tail burst are consecutive).
    size_t modelLostFrames = 0;
    for (const auto& [frame, n] : modelDropsPerFrame)
    {
        modelLostFrames += n >= 2 ? 1 : 0;
    }

    const double secs = (tEnd - t0) / 1e9;
    std::printf(
        "pacing window=%.2fms keyframe=%.2fms (effective P %.2f / key %.2f ms) fps=%u target=%.0fMbps rcvbuf=%d\n",
        opt.windowMs, opt.keyframeMs, PacingWindowNs(pacing, false, opt.fps) / 1e6,
        PacingWindowNs(pacing, true, opt.fps) / 1e6, opt.fps, opt.mbps, rcvbuf);
    std::printf(
        "  sent: frames=%u packets=%zu actual=%.1fMbps sendFailures=%zu received=%zu lost=%zu flushedFrames=%zu\n",
        frames, sent.size(), totalBytes * 8.0 / secs / 1e6, sendFailures, received.size(),
        sent.size() - received.size(), flushedFrames);
    std::printf("  burst span (first->last arrival, us): P p50=%.0f p95=%.0f max=%.0f | key p50=%.0f max=%.0f\n",
                Percentile(spanP, 0.5), Percentile(spanP, 0.95), Percentile(spanP, 1.0), Percentile(spanK, 0.5),
                Percentile(spanK, 1.0));
    std::printf("  intra-frame packet gap (us): p5=%.0f p50=%.0f p95=%.0f\n", Percentile(gaps, 0.05),
                Percentile(gaps, 0.5), Percentile(gaps, 0.95));
    std::printf("  peak rate in 1 ms bins (Mbps): p50=%.0f p99=%.0f max=%.0f\n", Percentile(binMbps, 0.5),
                Percentile(binMbps, 0.99), Percentile(binMbps, 1.0));
    std::printf("  added latency (frame start->last packet sent, ms): P p50=%.2f p99=%.2f | key max=%.2f\n",
                Percentile(addedP, 0.5), Percentile(addedP, 0.99), Percentile(addedK, 1.0));
    std::printf("  pacer wake lateness per frame (us): p50=%.1f p99=%.1f max=%.1f\n", Percentile(lateness, 0.5),
                Percentile(lateness, 0.99), Percentile(lateness, 1.0));
    std::printf("  MODEL (not measured) AP FIFO %u pkts @ %.0f Mbps: drops=%zu frames with 2+ drops=%zu/%u\n",
                opt.modelQueuePackets, opt.modelDrainMbps, modelDrops, modelLostFrames, frames);

    if (!opt.csv.empty())
    {
        FILE* out = std::fopen(opt.csv.c_str(), "w");
        if (out)
        {
            std::fprintf(out, "frame,index,flags,recv_kernel_us,recv_user_ns\n");
            for (const RecvRecord& r : received)
            {
                std::fprintf(out, "%u,%u,%u,%lld,%lld\n", r.frame, r.index, r.flags, static_cast<long long>(r.kernelUs),
                             static_cast<long long>(r.userNs));
            }
            std::fclose(out);
        }
    }
    close(rx);
    close(tx);
    return 0;
}

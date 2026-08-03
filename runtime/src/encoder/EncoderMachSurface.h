// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

/**
 * Mach rendezvous + IOSurface transfer between the runtime (parent) and the
 * native-arm64 encoder helper (child) — the Gate B-proven route in its
 * production shape.
 *
 * Route (bootstrap namespace left INTACT):
 *   1. PARENT bootstrap_check_in()s the per-spawn name
 *      "org.winevr.oxrsys.enc.<pid>.<64-bit-hex-nonce>" BEFORE spawning; the
 *      receive right stays with the parent.
 *   2. CHILD bootstrap_look_up()s that name (passed via argv) and mach_msg-
 *      sends its own check-in port (a receive right it allocated, sent as
 *      MAKE_SEND) to the parent.
 *   3. PARENT transfers IOSurface send rights (IOSurfaceCreateMachPort ->
 *      mach_msg_port_descriptor_t, MOVE_SEND) to the child's port, each
 *      tagged {generation, slot, width, height, pixelFormat}.
 *   4. CHILD resolves IOSurfaceLookupFromMachPort and validates the tag.
 *
 * Do NOT switch this to posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT):
 * Gate B proved libxpc latches the bootstrap port during libSystem init,
 * before main(), and restoring the real port afterwards does not heal it —
 * the child's first XPC-touching call (IOSurface lookup / Metal shader
 * compiler / VideoToolbox) then hangs forever in bootstrap_look_up2.
 *
 * Right-ownership rules, per outcome:
 *  - Parent check-in: bootstrap_check_in yields ONE receive right, owned by
 *    ParentSurfaceRendezvous and destroyed (mach_port_mod_refs RECEIVE -1) in
 *    Close(). The bootstrap name unregisters with the receive right.
 *  - Child check-in message: carries MAKE_SEND on the child's receive right;
 *    the parent ends up with one send right (childPort_), deallocated in
 *    Close(). The child keeps its receive right and destroys it on exit
 *    (process teardown reclaims it on crash — kernel-side, nothing leaks
 *    into the parent, verified by the Gate B kill/respawn port audit).
 *  - Surface transfer SUCCESS: IOSurfaceCreateMachPort creates a send right
 *    (+1 surface use count); MOVE_SEND moves it into the message, so the
 *    parent no longer owns it — no parent-side deallocation. The child's
 *    IOSurfaceLookupFromMachPort does NOT consume the received right: the
 *    child must mach_port_deallocate it after lookup (ReceiveSurface does),
 *    and the looked-up IOSurfaceRef (+1 CF ref) is released by the child
 *    when the generation is retired.
 *  - Surface transfer SEND FAILURE: the right did not move; the parent must
 *    (and SendSurface does) mach_port_deallocate the orphaned send right,
 *    dropping the extra surface use count.
 *  - Partial registration (child dies mid-transfer): rights already moved
 *    die with the child task; the parent's own IOSurfaces are unaffected
 *    (each live port only raises the surface use count). Re-registration to
 *    a respawned child re-creates fresh ports — Gate B ran 5 kill/respawn
 *    cycles with a flat parent port count.
 *  - Child-side EOF/stale-generation: a received right for a stale
 *    generation is still deallocated by the child after (optional) lookup;
 *    rejecting the tag must never leak the port.
 *
 * IOSurfaceRef is passed as void* so this header stays framework-free for
 * the transport's header includes; the .mm resolves the real types.
 */
namespace oxrsys::encoder::mach_surface
{

struct SurfaceTag
{
    uint32_t generation = 0;
    uint32_t slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixelFormat = 0; ///< fourcc, e.g. 'BGRA'
};

/// "org.winevr.oxrsys.enc.<pid>.<64-bit-hex-nonce>" for this process.
std::string MakeRendezvousName();

class ParentSurfaceRendezvous
{
public:
    ParentSurfaceRendezvous() = default;
    ~ParentSurfaceRendezvous();

    ParentSurfaceRendezvous(const ParentSurfaceRendezvous&) = delete;
    ParentSurfaceRendezvous& operator=(const ParentSurfaceRendezvous&) = delete;

    /// bootstrap_check_in the name. Must run BEFORE spawning the child.
    bool CheckIn(const std::string& name);

    /// Receive the child's check-in message (bounded). On success the child's
    /// port is held internally for SendSurface.
    bool WaitForChild(uint32_t timeoutMs);

    /**
     * Transfer one IOSurface send right (MOVE_SEND) tagged with `tag`.
     * `iosurface` is an IOSurfaceRef. On send failure the orphaned right is
     * deallocated here (see ownership rules above).
     */
    bool SendSurface(void* iosurface, const SurfaceTag& tag);

    /// Drop the child send right only (keeps the check-in receive right for a
    /// future respawn).
    void ForgetChild();
    void Close();

private:
    uint32_t receivePort_ = 0; ///< mach_port_t receive right from check-in
    uint32_t childPort_ = 0;   ///< send right to the child's check-in port
};

class ChildSurfaceRendezvous
{
public:
    ChildSurfaceRendezvous() = default;
    ~ChildSurfaceRendezvous();

    ChildSurfaceRendezvous(const ChildSurfaceRendezvous&) = delete;
    ChildSurfaceRendezvous& operator=(const ChildSurfaceRendezvous&) = delete;

    /// bootstrap_look_up(name) and send our freshly allocated receive right's
    /// MAKE_SEND to the parent. Bounded retries cover the spawn race.
    bool CheckIn(const std::string& name, uint32_t timeoutMs);

    /**
     * Receive one surface-transfer message (bounded wait). On success returns
     * the looked-up IOSurfaceRef (+1 CF ref, caller releases) via
     * `outSurface`, fills `tag`, and has already deallocated the carried port
     * right. Returns false on timeout, receive error, or failed lookup (the
     * port right is deallocated in every path once received).
     */
    bool ReceiveSurface(void** outSurface, SurfaceTag& tag, uint32_t timeoutMs);

    void Close();

private:
    uint32_t receivePort_ = 0; ///< the child's check-in receive right
};

} // namespace oxrsys::encoder::mach_surface

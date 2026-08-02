// SPDX-License-Identifier: MPL-2.0

#import "EncoderMachSurface.h"

#import <IOSurface/IOSurface.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>

namespace oxrsys::encoder::mach_surface
{

namespace
{

enum : mach_msg_id_t
{
    kMsgChildCheckin = 0x4F581001, // child -> parent: port = child rx (MAKE_SEND)
    kMsgSurface = 0x4F581002,      // parent -> child: port = IOSurface (MOVE_SEND)
};

// Fixed-layout rendezvous message: one port descriptor + the surface tag.
// This is a Mach message (kernel-copied within one host), not a cross-machine
// wire format, so a packed struct is safe here — unlike the socket protocol.
struct RendezvousMsg
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t generation;
    uint32_t slot;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
};

struct RendezvousMsgRecv
{
    RendezvousMsg msg;
    mach_msg_trailer_t trailer;
};

kern_return_t SendPortMsg(mach_port_t dest, mach_msg_id_t id, mach_port_t port,
                          mach_msg_type_name_t disposition, const SurfaceTag& tag)
{
    RendezvousMsg msg = {};
    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size = sizeof(msg);
    msg.header.msgh_remote_port = dest;
    msg.header.msgh_id = id;
    msg.body.msgh_descriptor_count = 1;
    msg.port.name = port;
    msg.port.disposition = disposition;
    msg.port.type = MACH_MSG_PORT_DESCRIPTOR;
    msg.generation = tag.generation;
    msg.slot = tag.slot;
    msg.width = tag.width;
    msg.height = tag.height;
    msg.pixelFormat = tag.pixelFormat;
    return mach_msg(&msg.header, MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

kern_return_t RecvPortMsg(mach_port_t rx, RendezvousMsgRecv& out, uint32_t timeoutMs)
{
    memset(&out, 0, sizeof(out));
    return mach_msg(&out.msg.header, MACH_RCV_MSG | (timeoutMs != 0 ? MACH_RCV_TIMEOUT : 0), 0,
                    sizeof(out), rx, timeoutMs, MACH_PORT_NULL);
}

} // namespace

std::string MakeRendezvousName()
{
    std::random_device rd;
    const uint64_t nonce = ((uint64_t)rd() << 32) | rd();
    char name[128];
    snprintf(name, sizeof(name), "org.winevr.oxrsys.enc.%d.%016" PRIx64, (int)getpid(), nonce);
    return name;
}

// ---------------------------------------------------------------------------
// Parent side.
// ---------------------------------------------------------------------------

ParentSurfaceRendezvous::~ParentSurfaceRendezvous()
{
    Close();
}

bool ParentSurfaceRendezvous::CheckIn(const std::string& name)
{
    Close();
    mach_port_t rx = MACH_PORT_NULL;
    // bootstrap_check_in registers the name in the (intact) bootstrap
    // namespace and hands US the receive right — the inversion of the probe's
    // deprecated child-side bootstrap_register, same namespace principle.
    const kern_return_t kr = bootstrap_check_in(bootstrap_port, name.c_str(), &rx);
    if (kr != KERN_SUCCESS)
    {
        return false;
    }
    receivePort_ = rx;
    return true;
}

bool ParentSurfaceRendezvous::WaitForChild(uint32_t timeoutMs)
{
    if (receivePort_ == 0)
    {
        return false;
    }
    RendezvousMsgRecv msg;
    const kern_return_t kr = RecvPortMsg(receivePort_, msg, timeoutMs);
    if (kr != KERN_SUCCESS || msg.msg.header.msgh_id != kMsgChildCheckin ||
        msg.msg.port.name == MACH_PORT_NULL)
    {
        // A non-checkin message could still carry a right; release it so a
        // confused peer cannot make us leak.
        if (kr == KERN_SUCCESS && (msg.msg.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0 &&
            msg.msg.port.name != MACH_PORT_NULL)
        {
            mach_port_deallocate(mach_task_self(), msg.msg.port.name);
        }
        return false;
    }
    ForgetChild();
    childPort_ = msg.msg.port.name;
    return true;
}

bool ParentSurfaceRendezvous::SendSurface(void* iosurface, const SurfaceTag& tag)
{
    if (childPort_ == 0 || iosurface == nullptr)
    {
        return false;
    }
    // Creates a send right that holds +1 use count on the surface for as long
    // as the right is alive anywhere.
    const mach_port_t surfacePort = IOSurfaceCreateMachPort((IOSurfaceRef)iosurface);
    if (surfacePort == MACH_PORT_NULL)
    {
        return false;
    }
    // MOVE_SEND: on success the right belongs to the message/child — the
    // parent must NOT deallocate it. On failure it never left, so we must.
    const kern_return_t kr =
        SendPortMsg(childPort_, kMsgSurface, surfacePort, MACH_MSG_TYPE_MOVE_SEND, tag);
    if (kr != KERN_SUCCESS)
    {
        mach_port_deallocate(mach_task_self(), surfacePort);
        return false;
    }
    return true;
}

void ParentSurfaceRendezvous::ForgetChild()
{
    if (childPort_ != 0)
    {
        mach_port_deallocate(mach_task_self(), childPort_);
        childPort_ = 0;
    }
}

void ParentSurfaceRendezvous::Close()
{
    ForgetChild();
    if (receivePort_ != 0)
    {
        // Destroy the receive right; the bootstrap registration dies with it.
        mach_port_mod_refs(mach_task_self(), receivePort_, MACH_PORT_RIGHT_RECEIVE, -1);
        receivePort_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Child side.
// ---------------------------------------------------------------------------

ChildSurfaceRendezvous::~ChildSurfaceRendezvous()
{
    Close();
}

bool ChildSurfaceRendezvous::CheckIn(const std::string& name, uint32_t timeoutMs)
{
    Close();
    mach_port_t parentPort = MACH_PORT_NULL;
    kern_return_t kr = KERN_FAILURE;
    // The parent checks the name in before spawning us, so the first lookup
    // normally succeeds; the retry loop only covers scheduler skew.
    const uint32_t stepMs = 50;
    for (uint32_t waited = 0;; waited += stepMs)
    {
        kr = bootstrap_look_up(bootstrap_port, name.c_str(), &parentPort);
        if (kr == KERN_SUCCESS || waited >= timeoutMs)
        {
            break;
        }
        usleep(stepMs * 1000);
    }
    if (kr != KERN_SUCCESS)
    {
        return false;
    }

    mach_port_t rx = MACH_PORT_NULL;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rx);
    if (kr == KERN_SUCCESS)
    {
        SurfaceTag emptyTag;
        // MAKE_SEND mints a send right from our receive right into the message;
        // we keep the receive right.
        kr = SendPortMsg(parentPort, kMsgChildCheckin, rx, MACH_MSG_TYPE_MAKE_SEND, emptyTag);
    }
    mach_port_deallocate(mach_task_self(), parentPort);
    if (kr != KERN_SUCCESS)
    {
        if (rx != MACH_PORT_NULL)
        {
            mach_port_mod_refs(mach_task_self(), rx, MACH_PORT_RIGHT_RECEIVE, -1);
        }
        return false;
    }
    receivePort_ = rx;
    return true;
}

bool ChildSurfaceRendezvous::ReceiveSurface(void** outSurface, SurfaceTag& tag, uint32_t timeoutMs)
{
    if (outSurface == nullptr || receivePort_ == 0)
    {
        return false;
    }
    *outSurface = nullptr;
    RendezvousMsgRecv msg;
    const kern_return_t kr = RecvPortMsg(receivePort_, msg, timeoutMs);
    if (kr != KERN_SUCCESS)
    {
        return false;
    }
    const mach_port_t surfacePort = msg.msg.port.name;
    if (msg.msg.header.msgh_id != kMsgSurface || surfacePort == MACH_PORT_NULL)
    {
        if ((msg.msg.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0 &&
            surfacePort != MACH_PORT_NULL)
        {
            mach_port_deallocate(mach_task_self(), surfacePort);
        }
        return false;
    }
    tag.generation = msg.msg.generation;
    tag.slot = msg.msg.slot;
    tag.width = msg.msg.width;
    tag.height = msg.msg.height;
    tag.pixelFormat = msg.msg.pixelFormat;

    // Lookup does NOT consume the port right; deallocate it in every path.
    // The looked-up IOSurfaceRef itself keeps the surface alive from here on.
    IOSurfaceRef surface = IOSurfaceLookupFromMachPort(surfacePort);
    mach_port_deallocate(mach_task_self(), surfacePort);
    if (surface == nullptr)
    {
        return false;
    }

    // Validate against the tag; a mismatched surface is released, not used.
    const size_t width = IOSurfaceGetWidth(surface);
    const size_t height = IOSurfaceGetHeight(surface);
    const OSType format = IOSurfaceGetPixelFormat(surface);
    if (width != tag.width || height != tag.height ||
        (tag.pixelFormat != 0 && format != tag.pixelFormat))
    {
        CFRelease(surface);
        return false;
    }
    *outSurface = surface;
    return true;
}

void ChildSurfaceRendezvous::Close()
{
    if (receivePort_ != 0)
    {
        mach_port_mod_refs(mach_task_self(), receivePort_, MACH_PORT_RIGHT_RECEIVE, -1);
        receivePort_ = 0;
    }
}

} // namespace oxrsys::encoder::mach_surface

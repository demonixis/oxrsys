// SPDX-License-Identifier: MPL-2.0

#import <Metal/Metal.h>

extern "C" void* OxrsysTestCreateMetalCommandQueue()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
    {
        return nullptr;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    return (void*)queue;
}

extern "C" void OxrsysTestReleaseMetalObject(void* object)
{
    if (object != nullptr)
    {
        [(id)object release];
    }
}

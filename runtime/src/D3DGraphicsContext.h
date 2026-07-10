// SPDX-License-Identifier: MPL-2.0

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

struct D3D11GraphicsContext
{
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateContext = nullptr;
    LUID adapterLuid = {};
};

struct D3D12GraphicsContext
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    LUID adapterLuid = {};
};

#endif // defined(_WIN32)

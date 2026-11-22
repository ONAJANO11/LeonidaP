// DXGI shim: entry points CreateDXGIFactory, CreateDXGIFactory1, CreateDXGIFactory2,
// DXGIDeclareAdapterRemovalSupport, and DXGIGetDebugInterface1.
// Prevents guest binaries from resolving host dxgi.dll from System32 (docs/d3d12_vulkan.md section 3.1).
#include <windows.h>
#include <dxgi1_5.h>

#include <cstdio>

#ifdef LEONIDA_VULKAN_DEVICE
#include "d3d12_com.h"
#endif
#include "shims.h"

namespace {

HRESULT WINAPI CreateDXGIFactory2_shim(UINT flags, REFIID riid, void** out) {
    if (!out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    (void)flags;
#ifdef LEONIDA_VULKAN_DEVICE
    return rt::d3d12::create_dxgi_factory(nullptr, riid, out);
#else
    (void)riid;
    return DXGI_ERROR_NOT_FOUND;
#endif
}

HRESULT WINAPI CreateDXGIFactory1_shim(REFIID riid, void** out) {
    return CreateDXGIFactory2_shim(0, riid, out);
}

HRESULT WINAPI CreateDXGIFactory_shim(REFIID riid, void** out) {
    return CreateDXGIFactory2_shim(0, riid, out);
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport_shim() {
    return S_OK;
}

HRESULT WINAPI DXGIGetDebugInterface1_shim(UINT flags, REFIID riid, void** out) {
    (void)flags;
    (void)riid;
    if (!out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    return E_NOINTERFACE;
}

}  // namespace

SHIM_TABLE(dxgi,
    {"dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&CreateDXGIFactory_shim)},
    {"dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&CreateDXGIFactory1_shim)},
    {"dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&CreateDXGIFactory2_shim)},
    {"dxgi.dll", "DXGIDeclareAdapterRemovalSupport", reinterpret_cast<void*>(&DXGIDeclareAdapterRemovalSupport_shim)},
    {"dxgi.dll", "DXGIGetDebugInterface1", reinterpret_cast<void*>(&DXGIGetDebugInterface1_shim)},
);

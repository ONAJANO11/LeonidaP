// Command lists from the Xbox-ABI device are console-order list proxies (d3d12_xs_device.h): past slot 26 their
// vtable is not the PC one. A test that records through the PC header swaps the proxy for its PC list, keeping
// one reference; a list that is not a proxy is left as it is.
#pragma once

#include "../shims/d3d12_xs_device.h"

template <class List>
void use_pc_list(List*& list) {
    ID3D12CommandList* real = rt::d3d12_xs::list_proxy_real_list(list);
    if (!real) return;
    List* pc = nullptr;
    real->QueryInterface(__uuidof(List), reinterpret_cast<void**>(&pc));
    list->Release();
    list = pc;
}

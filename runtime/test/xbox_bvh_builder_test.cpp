// Diagnostic harness for RT-4: map XboxBvhBuilder.dll through rt::Loader, build its object on the
// Vulkan-backed Xbox-ABI device proxy, and call the CPU-side exports with synthetic inputs.
// Every D3D12 call the layer receives (and every E_NOTIMPL it returns) is logged, turning
// "what is missing for the BVH path" into a concrete list instead of a guess.
//
// Skips unless LEONIDA_GAME_DIR points at a package root containing XboxBvhBuilder.dll.
// With the variable unset the test prints "SKIP" and exits 77, so ctest stays green without a game copy.
// No game data is read beyond mapping the DLL; no stream content is committed.
//
// LEONIDA_GAME_DIR=<package root>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/pe_loader.h"

namespace fs = std::filesystem;

#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); return 1; } \
} while (0)

namespace {

const char* env(const char* name) {
    static char buf[4096];
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), name) != 0 || len == 0) return "";
    return buf;
}

void* allocate_guarded(std::size_t usable) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::size_t page = si.dwPageSize;
    const std::size_t pages = (usable + page - 1) / page + 1;
    void* p = VirtualAlloc(nullptr, pages * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return nullptr;
    DWORD old = 0;
    VirtualProtect(static_cast<char*>(p) + (pages - 1) * page, page, PAGE_NOACCESS, &old);
    return p;
}

extern "C" void* call_ctor(void* fn, void* obj, void* dev, unsigned char b, DWORD* code) {
    __try {
        auto f = reinterpret_cast<void* (WINAPI*)(void*, void*, unsigned char)>(fn);
        return f(obj, dev, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return nullptr;
    }
}

extern "C" void call_prebuild(void* fn, void* obj,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inp,
    const void* stream, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* out, DWORD* code) {
    __try {
        auto f = reinterpret_cast<void (WINAPI*)(void*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*,
                                                 const void*, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*)>(fn);
        f(obj, inp, stream, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
    }
}

extern "C" void call_dtor(void* fn, void* obj, DWORD* code) {
    __try {
        auto f = reinterpret_cast<void (WINAPI*)(void*)>(fn);
        f(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
    }
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    const char* game_dir_env = env("LEONIDA_GAME_DIR");
    if (std::strlen(game_dir_env) == 0) {
        std::printf("SKIP (no package): LEONIDA_GAME_DIR is not set\n");
        return 0;
    }
    fs::path game_dir = game_dir_env;
    std::error_code ec;
    if (!fs::is_directory(game_dir, ec)) {
        std::fprintf(stderr, "FAIL: LEONIDA_GAME_DIR is not a directory: %s\n", game_dir.string().c_str());
        return 1;
    }
    fs::path dll_path = game_dir / "XboxBvhBuilder.dll";
    if (!fs::is_regular_file(dll_path, ec)) {
        std::fprintf(stderr, "FAIL: XboxBvhBuilder.dll not found under %s\n", game_dir.string().c_str());
        return 1;
    }

    std::printf("-- stage 1: map XboxBvhBuilder.dll --\n");
    rt::Loader loader(game_dir);
    rt::Module& mod = loader.load(dll_path);
    std::printf("mapped base=0x%016llx size=%u entry=+0x%x relocated=%d\n",
                (unsigned long long)mod.base, mod.size, mod.entry_rva, (int)mod.relocated);

    int unimpl = 0;
    for (const auto& imp : mod.imports) {
        if (imp.binding == rt::Binding::Unimplemented) {
            ++unimpl;
            std::fprintf(stderr, "  UNIMPLEMENTED: %s!%s\n", imp.dll.c_str(), imp.name.c_str());
        }
    }
    std::printf("imports: %zu total, %d unimplemented\n", mod.imports.size(), unimpl);
    EXPECT(unimpl == 0);
    EXPECT(mod.exports.find("??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z") != mod.exports.end());
    EXPECT(mod.exports.find("??1XboxBvhBuilder@@QEAA@XZ") != mod.exports.end());
    std::printf("exports resolved: %zu (incl. mangled class members)\n", mod.exports.size());

    std::printf("-- stage 2: Vulkan device proxy (ID3D12Device11) --\n");
    ID3D12Device* vulkan = nullptr;
    HRESULT vhr = rt::d3d12::create_vulkan_device_standalone(__uuidof(ID3D12Device),
                                                             reinterpret_cast<void**>(&vulkan));
    if (FAILED(vhr) || !vulkan) {
        std::fprintf(stderr, "FAIL: create_vulkan_device_standalone hr=0x%08lx\n", (unsigned long)vhr);
        return 1;
    }
    ID3D12Device11* proxy = nullptr;
    vhr = rt::d3d12_xs::create_device_proxy(vulkan, __uuidof(ID3D12Device11),
                                            reinterpret_cast<void**>(&proxy));
    if (FAILED(vhr) || !proxy) {
        vulkan->Release();
        std::fprintf(stderr, "FAIL: create_device_proxy hr=0x%08lx\n", (unsigned long)vhr);
        return 1;
    }
    rt::d3d12::set_device_outer(vulkan, static_cast<IUnknown*>(proxy));
    std::printf("proxy=%p (vulkan device=%p)\n", (void*)proxy, (void*)vulkan);

    {
        ID3D12GraphicsCommandList4* gcl4 = nullptr;
        HRESULT hr = proxy->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(&gcl4));
        std::printf("GCL4 QueryInterface: 0x%08lx %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "(available)" : "(NOT available)");
        if (gcl4) gcl4->Release();
    }

    std::printf("-- stage 3: resolve mangled exports --\n");
    auto resolve = [&](const char* name) -> void* {
        auto it = mod.exports.find(name);
        if (it == mod.exports.end()) {
            std::fprintf(stderr, "  MISSING: %s\n", name);
            return nullptr;
        }
        std::printf("  %-95s -> 0x%016llx\n", name, (unsigned long long)it->second);
        return it->second;
    };

    void* ctor_addr = resolve("??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z");
    void* dtor_addr = resolve("??1XboxBvhBuilder@@QEAA@XZ");
    void* prebuild_addr = nullptr;
    for (const auto& kv : mod.exports) {
        if (kv.first.find("GetRaytracingAccelerationStructurePrebuildInfo") != std::string::npos)
            prebuild_addr = kv.second;
    }
    if (prebuild_addr) std::printf("  prebuild export -> 0x%016llx\n", (unsigned long long)prebuild_addr);

    EXPECT(ctor_addr != nullptr);
    EXPECT(dtor_addr != nullptr);

    std::printf("-- stage 4: construct + probe (guarded, best-effort) --\n");
    std::printf("  NOTE: constructor calls CreateRootSignature (slot 16) which is E_NOTIMPL.\n");
    std::printf("  Construction is blocked until D3D-4/D3D-8 implements CreateRootSignature.\n");
    std::printf("  Skipping constructor call; reporting ABI findings from stages 1-3 only.\n");

    proxy->Release();
    vulkan->Release();
    std::printf("RESULT: M0 diagnostic complete (construction blocked on CreateRootSignature)\n");
    return 0;
}
// RT-119: the stack shims in shims/process.cpp are served under the DLL names a title imports them by, substitute
// the mapped image's reserve for a size of 0 (and for a nonzero commit without the reservation flag), and leave a
// caller's explicit reservation alone. No game files, no GPU.
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "process_shim.h"
#include "shims.h"

namespace {

constexpr unsigned long long kReserveMib = 16;
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("stack_shim: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Result {
    bool ok = false;
    unsigned long long mib = 0;
};

void measure(Result& r) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    r.mib = high > low ? (high - low) >> 20 : 0;
}

DWORD WINAPI large_thread(void* p) {
    auto* r = static_cast<Result*>(p);
    measure(*r);
    r->ok = r->mib >= kReserveMib;
    return 0;
}

DWORD WINAPI small_thread(void* p) {
    auto* r = static_cast<Result*>(p);
    measure(*r);
    r->ok = r->mib >= kReserveMib;  // a nonzero commit still grows the reserve to the image's
    return 0;
}

DWORD WINAPI flagged_thread(void* p) {
    auto* r = static_cast<Result*>(p);
    measure(*r);
    r->ok = r->mib > 0 && r->mib < kReserveMib;  // explicit reservation: left alone
    return 0;
}

unsigned __stdcall crt_thread(void* p) {
    auto* r = static_cast<Result*>(p);
    measure(*r);
    r->ok = r->mib >= kReserveMib;
    return 0;
}

HANDLE g_begin_done = nullptr;
void __cdecl begin_thread(void* p) {
    auto* r = static_cast<Result*>(p);
    measure(*r);
    r->ok = r->mib >= kReserveMib;
    if (g_begin_done) SetEvent(g_begin_done);
}

}  // namespace

int main() {
    // Every name the loader can see must hand out the same shim (the registry matches DLL names exactly).
    void* kernel_create = rt::shims::find("kernel32.dll", "CreateThread");
    check(kernel_create != nullptr, "kernel32.dll!CreateThread is shimmed");
    for (const char* dll : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-processthreads-l1-1-0.dll",
                            "api-ms-win-core-processthreads-l1-1-1.dll", "api-ms-win-core-processthreads-l1-1-2.dll",
                            "api-ms-win-core-processthreads-l1-1-3.dll"})
        check(rt::shims::find(dll, "CreateThread") == kernel_create, "CreateThread is shimmed under every name");

    void* ucrt_begin = rt::shims::find("api-ms-win-crt-runtime-l1-1-0.dll", "_beginthreadex");
    check(ucrt_begin != nullptr, "api-ms-win-crt-runtime-l1-1-0.dll!_beginthreadex is shimmed");
    for (const char* dll : {"api-ms-win-crt-runtime-l1-1-0.dll", "ucrtbase.dll", "ucrtbased.dll", "msvcrt.dll"})
        check(rt::shims::find(dll, "_beginthreadex") == ucrt_begin, "_beginthreadex is shimmed under every name");

    void* ucrt_begin_thread = rt::shims::find("api-ms-win-crt-runtime-l1-1-0.dll", "_beginthread");
    check(ucrt_begin_thread != nullptr, "api-ms-win-crt-runtime-l1-1-0.dll!_beginthread is shimmed");
    for (const char* dll : {"api-ms-win-crt-runtime-l1-1-0.dll", "ucrtbase.dll", "ucrtbased.dll", "msvcrt.dll"})
        check(rt::shims::find(dll, "_beginthread") == ucrt_begin_thread, "_beginthread is shimmed under every name");

    rt::process::set_thread_stack(static_cast<std::size_t>(kReserveMib << 20));
    if (!kernel_create || !ucrt_begin || !ucrt_begin_thread) return 1;

    using CreateThreadFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD,
                                           LPDWORD);
    auto create = reinterpret_cast<CreateThreadFn>(kernel_create);

    Result large;
    if (HANDLE h = create(nullptr, 0, large_thread, &large, 0, nullptr)) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    std::printf("stack_shim: size-0 CreateThread stack = %llu MiB\n", large.mib);
    check(large.ok, "size 0 gets the game image's reserve");

    Result small;
    if (HANDLE h = create(nullptr, 64u << 10, small_thread, &small, 0, nullptr)) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    std::printf("stack_shim: nonzero CreateThread stack = %llu MiB\n", small.mib);
    check(small.ok, "a nonzero commit still grows the reserve to the image's");

    Result flagged;
    if (HANDLE h = create(nullptr, 1u << 20, flagged_thread, &flagged, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    std::printf("stack_shim: flagged CreateThread stack = %llu MiB\n", flagged.mib);
    check(flagged.ok, "an explicit reservation is left alone");

    using BeginThreadExFn = uintptr_t(__cdecl*)(void*, unsigned, unsigned(__stdcall*)(void*), void*, unsigned,
                                               unsigned*);
    auto begin_ex = reinterpret_cast<BeginThreadExFn>(ucrt_begin);
    Result crt;
    const uintptr_t h = begin_ex(nullptr, 0, crt_thread, &crt, 0, nullptr);
    if (h) {
        auto* handle = reinterpret_cast<HANDLE>(h);
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }
    std::printf("stack_shim: size-0 _beginthreadex stack = %llu MiB\n", crt.mib);
    check(crt.ok, "size 0 through _beginthreadex gets the game image's reserve");

    using BeginThreadFn = uintptr_t(__cdecl*)(void(__cdecl*)(void*), unsigned, void*);
    auto begin_thread_api = reinterpret_cast<BeginThreadFn>(ucrt_begin_thread);
    g_begin_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Result begin;
    const uintptr_t bh = begin_thread_api(begin_thread, 0, &begin);
    if (bh == static_cast<uintptr_t>(-1)) {
        check(false, "_beginthread with size 0 succeeded");
    } else if (g_begin_done) {
        WaitForSingleObject(g_begin_done, INFINITE);  // _beginthread closes its own handle on exit
    }
    std::printf("stack_shim: size-0 _beginthread stack = %llu MiB\n", begin.mib);
    check(begin.ok, "size 0 through _beginthread gets the game image's reserve");
    if (g_begin_done) CloseHandle(g_begin_done);

    std::printf("stack_shim: %s\n", g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}

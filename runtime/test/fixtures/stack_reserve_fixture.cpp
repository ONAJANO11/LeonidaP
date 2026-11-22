// A stand-in game EXE for RT-119, linked with a large /STACK (64 MiB, see runtime/CMakeLists.txt). Under
// `gta6rt --run`, its entry point runs on the thread run_entry creates, and the threads it creates go through the
// CreateThread / _beginthreadex / _beginthread shims. On Windows a size-0 thread inherits the main image's
// SizeOfStackReserve; a nonzero commit keeps that reserve too. Every measured thread must see at least 64 MiB.
// Test code; no game data.
#include <windows.h>

#include <process.h>

#include <cstdint>
#include <cstdio>

namespace {

constexpr ULONG_PTR kExpected = 64ull << 20;
int g_failures = 0;
HANDLE g_begin_done = nullptr;

void check(bool ok, const char* what) {
    std::printf("stack_fixture: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void check_stack(const char* what) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const ULONG_PTR bytes = high > low ? high - low : 0;
    std::printf("stack_fixture: %s limits=[%p,%p] size=%llu MiB\n", what, reinterpret_cast<void*>(low),
                reinterpret_cast<void*>(high), static_cast<unsigned long long>(bytes >> 20));
    check(bytes >= kExpected, what);
}

DWORD WINAPI thread_fn(void*) {
    check_stack("a size-0 CreateThread inherits the game image's stack reserve");
    return 0;
}

DWORD WINAPI commit_thread_fn(void*) {
    check_stack("a nonzero CreateThread commit keeps the game image's stack reserve");
    return 0;
}

unsigned __stdcall crt_thread_fn(void*) {
    check_stack("a size-0 _beginthreadex inherits the game image's stack reserve");
    return 0;
}

void __cdecl begin_thread_fn(void*) {
    check_stack("a size-0 _beginthread inherits the game image's stack reserve");
    if (g_begin_done) SetEvent(g_begin_done);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    check_stack("the entry point runs with the game image's stack reserve");

    HANDLE t = CreateThread(nullptr, 0, thread_fn, nullptr, 0, nullptr);
    if (!t) {
        check(false, "CreateThread with size 0 succeeded");
    } else {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }

    HANDLE t2 = CreateThread(nullptr, 64u << 10, commit_thread_fn, nullptr, 0, nullptr);
    if (!t2) {
        check(false, "CreateThread with a nonzero commit succeeded");
    } else {
        WaitForSingleObject(t2, INFINITE);
        CloseHandle(t2);
    }

    const uintptr_t h = _beginthreadex(nullptr, 0, crt_thread_fn, nullptr, 0, nullptr);
    if (!h) {
        check(false, "_beginthreadex with size 0 succeeded");
    } else {
        auto* handle = reinterpret_cast<HANDLE>(h);
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }

    g_begin_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const uintptr_t bh = _beginthread(begin_thread_fn, 0, nullptr);
    if (bh == static_cast<uintptr_t>(-1)) {
        check(false, "_beginthread with size 0 succeeded");
    } else if (g_begin_done) {
        WaitForSingleObject(g_begin_done, INFINITE);  // _beginthread closes its own handle on exit
    }
    if (g_begin_done) CloseHandle(g_begin_done);

    std::printf("stack_fixture: %s\n", g_failures ? "FAILED" : "OK");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
}

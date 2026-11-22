// A stand-in game EXE for RT-119, linked with a large /STACK (64 MiB, see runtime/CMakeLists.txt). Under
// `gta6rt --run`, its entry point runs on the thread run_entry creates, and the threads it creates with size 0 go
// through the CreateThread / _beginthreadex shims. On Windows a size-0 thread inherits the main image's
// SizeOfStackReserve, so all three must see at least 64 MiB. Test code; no game data.
#include <windows.h>

#include <process.h>

#include <cstdint>
#include <cstdio>

namespace {

constexpr ULONG_PTR kExpected = 64ull << 20;
int g_failures = 0;

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

unsigned __stdcall crt_thread_fn(void*) {
    check_stack("a size-0 _beginthreadex inherits the game image's stack reserve");
    return 0;
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

    const uintptr_t h = _beginthreadex(nullptr, 0, crt_thread_fn, nullptr, 0, nullptr);
    if (!h) {
        check(false, "_beginthreadex with size 0 succeeded");
    } else {
        auto* handle = reinterpret_cast<HANDLE>(h);
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }

    std::printf("stack_fixture: %s\n", g_failures ? "FAILED" : "OK");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
}

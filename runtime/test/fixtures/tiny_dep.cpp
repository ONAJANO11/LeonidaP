// A second compiler-built fixture used to verify dependency-first DllMain ordering.
#include <windows.h>

namespace {

LONG g_attached = 0;

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) InterlockedExchange(&g_attached, 1);
    if (reason == DLL_PROCESS_DETACH) InterlockedExchange(&g_attached, 0);
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl tiny_dep_is_attached() {
    return InterlockedCompareExchange(&g_attached, 0, 0);
}

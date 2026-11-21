// Synthetic importing DLL; its attach result records whether tiny_dep initialized first.
#include <windows.h>

extern "C" __declspec(dllimport) int __cdecl tiny_dep_is_attached();

namespace {

LONG g_dependency_was_attached = 0;

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        InterlockedExchange(&g_dependency_was_attached, tiny_dep_is_attached());
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl tiny_dependency_was_attached() {
    return InterlockedCompareExchange(&g_dependency_was_attached, 0, 0);
}

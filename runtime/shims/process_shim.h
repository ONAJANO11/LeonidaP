// The game's command line, exit and stack, for `gta6rt --run` (shims/process.cpp).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace rt::process {

// What the game sees as argv[0..] and GetCommandLine: `program` then `args`, quoted where needed. The wide forms
// (__wargv, GetCommandLineW) hold the arguments exactly; the narrow ones are their ANSI code page text, with a
// character the code page can't represent replaced as the CRT does (RT-84).
void set_command_line(const std::wstring& program, const std::vector<std::wstring>& args);

// The mapped main image's OptionalHeader.SizeOfStackReserve, read by the loader before any image code runs. Windows
// gives a size-0 thread the main image's reserve; under gta6rt the host EXE's reserve would be used, so the
// CreateThread/_beginthreadex shims and run_entry use this instead. 0 means the image named none: use the default.
void set_thread_stack(std::size_t reserve_bytes);

inline constexpr std::size_t kDefaultStackReserve = 8u << 20;  // gta6rt's old fixed reserve

// Runs a mapped EXE's entry point on a new thread with `stack_bytes` of reserved stack (the reserve set by
// set_thread_stack when this is 0) and returns the game's exit code: the entry point's return value, or what it
// passed to exit/ExitProcess (3 for abort). Returns when the first of those happens; other game threads are left
// running (the host process ends them when it exits).
int run_entry(void* entry, std::size_t stack_bytes = 0);

}  // namespace rt::process

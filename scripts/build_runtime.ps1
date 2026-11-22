# Builds runtime\build\gta6rt.exe with MSVC at BelowNormal priority, on half the cores.
# Uses VS's CMake + Ninja (or cmake/ninja on PATH) via runtime\CMakePresets.json when available,
# otherwise calls cl.exe directly. Pass -Test to run CTest after building.
# The GameInput controller shim (SDL3, docs/gameinput.md) is built only on the CMake path: with an installed SDL3, or
# with -FetchSdl3, which lets CMake download the pinned SDL3 release. The cl.exe fallback never builds it.
param([switch]$Debug, [switch]$FetchSdl3, [switch]$Test)
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools wasn't found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$jobs = [Math]::Max(1, [int]([Environment]::ProcessorCount / 2))

$src = Join-Path $root "runtime"
$out = Join-Path $src "build"
New-Item -ItemType Directory -Force $out | Out-Null

$cmakeDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
$cmake = Join-Path $cmakeDir "CMake\bin\cmake.exe"
$ctest = Join-Path $cmakeDir "CMake\bin\ctest.exe"
$ninja = Join-Path $cmakeDir "Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not (Test-Path $ctest)) { $ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source }
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
$sdl = if ($FetchSdl3) { " -DLEONIDA_SDL3_FETCH=ON" } else { "" }

if ($cmake -and $ninja) {
    if ($Test -and -not $ctest) {
        throw "Running -Test requires ctest (install VS's 'C++ CMake tools' or put ctest on PATH)."
    }
    $cfgOverride = if ($Debug) { " -DCMAKE_BUILD_TYPE=Debug" } else { "" }
    $testCmd = if ($Test) { "`r`n`"$ctest`" --preset windows-ninja || exit /b 1" } else { "" }
    $body = @"
cd /d "$src" || exit /b 1
"$cmake" --preset windows-ninja "-DCMAKE_MAKE_PROGRAM=$ninja"$cfgOverride$sdl || exit /b 1
"$cmake" --build --preset windows-ninja -j $jobs || exit /b 1$testCmd
"@
} else {
    if ($Test) { throw "Running -Test requires CMake and Ninja (install VS's 'C++ CMake tools' or put them on PATH)." }
    # Keep this list in step with runtime\CMakeLists.txt. Optional parts that need a dependency are left out here:
    # the GameInput shim (SDL3), so controllers go through the host's GameInput.dll, if any, as without the shim.
    Write-Warning "CMake and Ninja not found (VS C++ CMake tools or PATH): building with cl.exe directly, without the GameInput shim (SDL3). Install CMake and Ninja to get it."
    $xtaskqueueDir = Join-Path $root "third_party\xtaskqueue"
    $xtaskqueueFiles = "Source\Task\TaskQueue.cpp", "Source\Task\AsyncLib.cpp", "Source\Task\ThreadPool_win32.cpp", "Source\Task\WaitTimer_win32.cpp" | ForEach-Object { "`"$xtaskqueueDir\$_`"" }
    $files = "src\main.cpp", "src\pe_loader.cpp", "shims\registry.cpp", "shims\xmem.cpp", "shims\d3d12_xs.cpp", "shims\d3d12_xs_device.cpp", "shims\d3d12_xs_rootsig.cpp", "shims\xgameruntime.cpp", "shims\library_loader.cpp", "shims\process.cpp" | ForEach-Object { "`"$src\$_`"" }
    $includes = "/I `"$src\src`" /I `"$src\shims`" /I `"$src\shims\xtaskqueue`" /I `"$xtaskqueueDir\Include`" /I `"$xtaskqueueDir\Source\Task`" /I `"$xtaskqueueDir\Source\Common`""
    $opt = if ($Debug) { "/Od /MDd" } else { "/O2 /MD" }
    $body = @"
cd /d "$out" || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /Zi $opt /MP$jobs /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE $includes $($files -join ' ') $($xtaskqueueFiles -join ' ') /Fe:gta6rt.exe /link /OPT:NOREF psapi.lib ole32.lib || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /O2 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX "$src\src\tls_host.cpp" /LD /Fe:gta6rt_tls_host.dll || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "$src\shims" "$src\shims\d3d12_xs_rootsig.cpp" "$src\shims\registry.cpp" "$src\shims\d3d12_xs.cpp" "$src\shims\d3d12_xs_device.cpp" "$src\test\root_sig_test.cpp" /Fe:root_sig_test.exe /link d3d12.lib || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "$src\shims" "$src\shims\d3d12_xs_rootsig.cpp" "$src\shims\registry.cpp" "$src\shims\d3d12_xs.cpp" "$src\shims\d3d12_xs_device.cpp" "$src\test\device_proxy_test.cpp" /Fe:device_proxy_test.exe /link d3d12.lib dxgi.lib || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "$src\src" "$src\test\descriptor_table_test.cpp" /Fe:descriptor_table_test.exe /link /OPT:NOREF || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "$src\src" "$src\test\command_list_test.cpp" /Fe:command_list_test.exe /link /OPT:NOREF || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" "$src\test\vk_pipeline_check.cpp" /Fe:vk_pipeline_check.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_texture_test.cpp" /Fe:vk_texture_test.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\gfx10_descriptors_test.cpp" /Fe:gfx10_descriptors_test.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_bindless_test.cpp" /Fe:vk_bindless_test.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_descriptor_bench.cpp" /Fe:vk_descriptor_bench.exe || exit /b 1
rem The GPU test harnesses check.py and run_gpu_tests.py require and the fuzzers run (tests\test_build_runtime_fallback.py).
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_harness.cpp" /Fe:vk_harness.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_compute_test.cpp" /Fe:vk_compute_test.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_store_test.cpp" /Fe:vk_store_test.exe || exit /b 1
if defined VULKAN_SDK cl /nologo /std:c++20 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%VULKAN_SDK%\Include" /I "$src\src" "$src\test\vk_image_fuzz.cpp" /Fe:vk_image_fuzz.exe || exit /b 1
"@
}

# A .cmd file avoids cmd.exe's quote stripping on paths with spaces.
# Switch to UTF-8 (CP 65001) before invoking CMake/Ninja/cl.exe so non-English MSVC /showIncludes prefixes
# (e.g. Spanish "Nota: inclusión del archivo:") match Ninja's msvc_deps_prefix instead of being mojibaked in OEM CP.
$bat = Join-Path $out "_build.cmd"
"@echo off`r`nchcp 65001 >nul`r`nset `"PATH=$installer;%PATH%`"`r`ncall `"$vcvars`" >nul || exit /b 1`r`n$body" | Set-Content -Encoding ascii $bat

$p = Start-Process cmd.exe -ArgumentList "/c", "`"$bat`"" -NoNewWindow -PassThru
$null = $p.Handle  # keeps the exit code readable after WaitForExit
try { $p.PriorityClass = "BelowNormal" } catch {}
$p.WaitForExit()
if ($p.ExitCode -ne 0) { throw "build failed ($($p.ExitCode))" }
Write-Host "built $out\gta6rt.exe"

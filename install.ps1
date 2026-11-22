#Requires -Version 5.1
<#
.SYNOPSIS
  Bootstraps the Leonida toolchain on Windows, clones the repository, and starts the build.

.DESCRIPTION
  Installs the prerequisites with winget, clones ItsNotRexed/Leonida with its submodule,
  builds a native llvm-mc with the AMDGPU target when needed, and hands the actual work to
  make_game.py. It never downloads the game, keys, or any game data: you supply your own
  copy of the Xbox Series package.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install.ps1

.EXAMPLE
  .\install.ps1 -Game "D:\GTA VI" -Out "E:\Leonida" -Work "E:\Leonida-work"
#>
[CmdletBinding()]
param(
    [string]$Dir = "",
    [string]$Game = "",
    [string]$Out = "",
    [string]$Work = "",
    [string]$Stages = "",
    [int]$Jobs = 0,
    [switch]$Wav,
    [switch]$Check,
    [switch]$SkipDeps,
    [string]$RuntimeBuild = "",
    [switch]$Plain,
    [switch]$DryRun,
    [switch]$Yes,
    [switch]$SelfTest,
    [Alias("h")]
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$TotalSteps = 6
$script:Step = 1
$script:Permille = 0
$script:Label = "starting"
$script:Anim = $false
$script:Color = $false
$script:Unicode = $false
$script:OldCodePage = ""
$script:ProgressStopped = $false
$script:Started = Get-Date
$script:ExitCode = 0
$script:Py = $null
$script:MakeGame = ""
$script:RunDir = ""
$script:Log = ""

function Show-Usage {
    @"
Leonida installer (Windows)

Usage: install.ps1 [options]

  -Dir DIR             clone here (default: this checkout, or %USERPROFILE%\Leonida)
  -Game DIR            your Xbox Series package (found automatically if omitted)
  -Out DIR             finished port (default: Documents\Leonida)
  -Work DIR            intermediate files, about 350 GB (default: <out>\work)
  -Stages LIST         comma list: check,shaders,assets,runtime,pipelines
  -Jobs N              worker processes (default: half the CPU threads)
  -Wav                 also decode Opus to PCM WAV (large)
  -RuntimeBuild DIR    use a Windows build of runtime\ instead of building it
  -Check               check the game copy and stop
  -SkipDeps            install nothing; use the tools already on PATH
  -Plain               no animation or color
  -DryRun              print what would run, change nothing
  -Yes                 skip the install confirmation (required when input is redirected)
  -SelfTest            compile the progress helper and exit (used by CI)
  -Help                this text

The installer never downloads the game, keys, or game data.
"@
}

function Format-Time([int]$Seconds) {
    if ($Seconds -lt 0) { $Seconds = 0 }
    $h = [int]($Seconds / 3600)
    $m = [int](($Seconds % 3600) / 60)
    $s = [int]($Seconds % 60)
    if ($h -gt 0) { return "{0}:{1:00}:{2:00}" -f $h, $m, $s }
    return "{0:00}:{1:00}" -f $m, $s
}

function Write-PlainBar([int]$Number, [int]$Total, [string]$Text) {
    $filled = [int](10 * $Number / [Math]::Max(1, $Total))
    if ($filled -gt 10) { $filled = 10 }
    $bar = ("#" * $filled) + ("-" * (10 - $filled))
    Write-Host ("[{0}] {1}/{2} {3}" -f $bar, $Number, $Total, $Text)
}

function Set-Step([int]$Number, [string]$Text) {
    $script:Step = $Number
    $script:Permille = 0
    $script:Label = $Text
    if ($script:Anim) {
        [LeonidaProgress]::Set($script:Step, $TotalSteps, 0, $script:Label)
    } else {
        Write-PlainBar $script:Step $TotalSteps $script:Label
    }
}

function Set-Percent([int]$Value) {
    if ($Value -lt 0) { $Value = 0 }
    if ($Value -gt 1000) { $Value = 1000 }
    $script:Permille = $Value
    if ($script:Anim) {
        [LeonidaProgress]::Set($script:Step, $TotalSteps, $script:Permille, $script:Label)
    }
}

function Set-Label([string]$Text) {
    $script:Label = $Text
    if ($script:Anim) {
        [LeonidaProgress]::Set($script:Step, $TotalSteps, $script:Permille, $script:Label)
    } else {
        Write-PlainBar $script:Step $TotalSteps $Text
    }
}

function Note([string]$Text) {
    if ($script:Anim) {
        [LeonidaProgress]::Message($Text)
    } else {
        Write-Host $Text
    }
}

function Stop-Progress([bool]$Ok, [string]$Text) {
    if ($script:ProgressStopped) { return }
    $script:ProgressStopped = $true
    $elapsed = [int]((Get-Date) - $script:Started).TotalSeconds
    $clean = (($Text -split "`r?`n") | Where-Object { $_ } | Select-Object -First 1)
    if (-not $clean) { $clean = "installer stopped" }
    $line = "{0} ({1})" -f $clean, (Format-Time $elapsed)
    $haveType = $null -ne ("LeonidaProgress" -as [type])
    if ($script:Anim -and $haveType) {
        if ($Ok) {
            [LeonidaProgress]::Set($TotalSteps, $TotalSteps, 1000, "finished")
            Start-Sleep -Milliseconds 150
        }
        [LeonidaProgress]::Stop($Ok, $line)
    } elseif ($Ok) {
        Write-Host ("[OK] " + $line)
    } else {
        Write-Host ("[FAIL] " + $line)
    }
}

function Restore-CodePage {
    if ($script:OldCodePage -and $script:OldCodePage -ne "65001") {
        try { & chcp.com $script:OldCodePage > $null } catch { }
    }
}

function Get-Tail([string]$Path, [int]$Lines = 20) {
    if (-not (Test-Path $Path)) { return "" }
    try {
        return ((Get-Content -Path $Path -Tail $Lines -ErrorAction Stop) -join [Environment]::NewLine)
    } catch {
        return ""
    }
}

function Invoke-Logged {
    param(
        [string]$Exe,
        [string[]]$ArgList,
        [string]$LogPath,
        [switch]$AllowFail
    )
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $code = 0
    $writer = New-Object System.IO.StreamWriter($LogPath, $true, (New-Object System.Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    try {
        & $Exe @ArgList 2>&1 | ForEach-Object { $writer.WriteLine($_.ToString()) }
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    } catch {
        $writer.WriteLine("error: " + $_.Exception.Message)
        $code = 1
    } finally {
        $writer.Dispose()
        $ErrorActionPreference = $old
    }
    if ($code -ne 0 -and -not $AllowFail) {
        throw ("{0} failed with exit code {1}.{2}{3}" -f $Exe, $code, [Environment]::NewLine, (Get-Tail $LogPath 15))
    }
    return $code
}

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $user = [Environment]::GetEnvironmentVariable("Path", "User")
    $parts = @()
    foreach ($p in @($machine, $user)) {
        if ($p) { $parts += ($p -split ";" | Where-Object { $_ }) }
    }
    if ($parts.Count -gt 0) { $env:Path = ($parts -join ";") }

    foreach ($name in @("VULKAN_SDK", "LLVM_MC", "DOTNET_ROOT")) {
        $value = [Environment]::GetEnvironmentVariable($name, "Machine")
        if (-not $value) { $value = [Environment]::GetEnvironmentVariable($name, "User") }
        if ($value) { Set-Item -Path ("Env:" + $name) -Value $value }
    }
    Add-VulkanBin
    Add-BuildToolPath
}

function Add-VulkanBin {
    if (-not $env:VULKAN_SDK) { return }
    $bin = Join-Path $env:VULKAN_SDK "Bin"
    if ((Test-Path (Join-Path $bin "spirv-as.exe")) -and ($env:Path -notlike ("*" + $bin + "*"))) {
        $env:Path = $bin + ";" + $env:Path
    }
}

function Add-BuildToolPath {
    foreach ($exe in @((Resolve-CMake), (Resolve-Ninja))) {
        if (-not $exe) { continue }
        $dir = Split-Path $exe -Parent
        if ($dir -and ($env:Path -notlike ("*" + $dir + "*"))) {
            $env:Path = $dir + ";" + $env:Path
        }
    }
}

function Test-Command([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Find-Python {
    $candidates = @()
    if (Test-Command "python") { $candidates += @{ Exe = "python"; Args = @() } }
    foreach ($v in @("3.14", "3.13", "3.12", "3")) {
        if (Test-Command "py") { $candidates += @{ Exe = "py"; Args = @("-$v") } }
    }
    foreach ($v in @("314", "313", "312")) {
        if ($env:LOCALAPPDATA) {
            $path = Join-Path $env:LOCALAPPDATA ("Programs\Python\Python" + $v + "\python.exe")
            if (Test-Path $path) { $candidates += @{ Exe = $path; Args = @() } }
        }
        $path = "C:\Python" + $v + "\python.exe"
        if (Test-Path $path) { $candidates += @{ Exe = $path; Args = @() } }
    }
    foreach ($c in $candidates) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & $c.Exe @($c.Args) -c "import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)" 2>$null | Out-Null
            if ($LASTEXITCODE -eq 0) {
                $exe = $c.Exe
                $pyArgs = @($c.Args)
                if ($c.Exe -eq "py") {
                    $resolved = @(& $c.Exe @($c.Args) -c "import sys; print(sys.executable)" 2>$null)[0]
                    if ($resolved -and (Test-Path $resolved)) {
                        $exe = $resolved
                        $pyArgs = @()
                    }
                }
                if ($exe -match '[\\/]') {
                    $pyDir = Split-Path $exe -Parent
                    if ($pyDir -and ($env:Path -notlike ("*" + $pyDir + "*"))) {
                        $env:Path = $pyDir + ";" + $env:Path
                    }
                }
                return [pscustomobject]@{ Exe = $exe; Args = $pyArgs }
            }
        } catch {
        } finally {
            $ErrorActionPreference = $old
        }
    }
    return $null
}

function Test-PythonOk {
    return ($null -ne (Find-Python))
}

function Test-DotNetSdk {
    if (-not (Test-Command "dotnet")) { return $false }
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $sdks = & dotnet --list-sdks 2>$null
        foreach ($line in @($sdks)) {
            if ($line -match '^(\d+)\.') {
                if ([int]$Matches[1] -ge 8) { return $true }
            }
        }
    } catch {
    } finally {
        $ErrorActionPreference = $old
    }
    return $false
}

function Get-VsCmake {
    $vs = Get-VsInstall
    if (-not $vs) { return $null }
    $path = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $path) { return $path }
    return $null
}

function Get-VsNinja {
    $vs = Get-VsInstall
    if (-not $vs) { return $null }
    $path = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path $path) { return $path }
    return $null
}

function Test-ExeVersion([string]$Exe, [version]$Minimum) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $first = @(& $Exe --version 2>$null)[0]
        if ($first -match '(\d+\.\d+(\.\d+)?)') {
            return ([version]$Matches[1] -ge $Minimum)
        }
    } catch {
    } finally {
        $ErrorActionPreference = $old
    }
    return $false
}

function Resolve-CMake {
    $cmd = Get-Command "cmake" -ErrorAction SilentlyContinue
    if ($cmd -and (Test-ExeVersion $cmd.Source ([version]"3.24"))) { return $cmd.Source }
    $vs = Get-VsCmake
    if ($vs -and (Test-ExeVersion $vs ([version]"3.24"))) { return $vs }
    return $null
}

function Test-CMake {
    return [bool](Resolve-CMake)
}

function Resolve-Ninja {
    $cmd = Get-Command "ninja" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return (Get-VsNinja)
}

function Test-Ninja {
    return [bool](Resolve-Ninja)
}

function Test-Vulkan {
    Add-VulkanBin
    if ($env:VULKAN_SDK -and (Test-Path (Join-Path $env:VULKAN_SDK "Bin\spirv-as.exe"))) { return $true }
    if (Test-Command "spirv-as") { return $true }
    $root = "C:\VulkanSDK"
    if (Test-Path $root) {
        $hits = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "Bin\spirv-as.exe" } |
            Where-Object { Test-Path $_ }
        $hit = @($hits)[0]
        if ($hit) {
            $env:VULKAN_SDK = Split-Path (Split-Path $hit -Parent) -Parent
            Add-VulkanBin
            return $true
        }
    }
    return $false
}

function Get-VsInstall {
    $root = ${env:ProgramFiles(x86)}
    if (-not $root) { return $null }
    $vswhere = Join-Path $root "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and $found) { return ($found | Select-Object -First 1).Trim() }
    } catch {
    } finally {
        $ErrorActionPreference = $old
    }
    return $null
}

function Test-VsCpp {
    return [bool](Get-VsInstall)
}

function Test-WindowsSdk {
    $base = ${env:ProgramFiles(x86)}
    if (-not $base) { return $false }
    $root = Join-Path $base "Windows Kits\10\Include"
    if (-not (Test-Path $root)) { return $false }
    foreach ($d in @(Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue)) {
        $name = $d.Name
        if ($name -match '^\d+\.\d+\.\d+\.\d+$') {
            if ([version]$name -ge [version]"10.0.26100.0") { return $true }
        }
    }
    return $false
}

function Test-FFmpeg {
    return (Test-Command "ffmpeg")
}

function Test-LlvmMc {
    if ($env:LLVM_MC -and -not (Test-Path $env:LLVM_MC)) {
        # A command line such as "wsl -d Ubuntu -e .../llvm-mc" is make_game.py's job to validate.
        return $true
    }
    $candidates = @()
    if ($env:LLVM_MC) { $candidates += $env:LLVM_MC }
    if (Test-Command "llvm-mc") { $candidates += (Get-Command "llvm-mc").Source }
    $profileDir = if ($env:USERPROFILE) { $env:USERPROFILE } else { $HOME }
    if ($profileDir) {
        $local = Join-Path $profileDir ".leonida\llvm-mc\llvm-mc.exe"
        if (Test-Path $local) { $candidates += $local }
    }
    foreach ($c in $candidates) {
        if (-not (Test-Path $c)) { continue }
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $version = (& $c --version 2>$null) -join "`n"
            if ($version -match "amdgcn") { return $true }
        } catch {
        } finally {
            $ErrorActionPreference = $old
        }
    }
    return $false
}

function Test-Stage([string]$Name) {
    return ("," + $script:Stages + ",").Contains("," + $Name + ",")
}

function Get-WantedPackages {
    $list = @()
    if (-not (Test-Command "git")) {
        $list += [pscustomobject]@{ Id = "Git.Git"; Label = "Git"; Extra = @(); Need = { Test-Command "git" } }
    }
    if (-not (Test-PythonOk)) {
        $list += [pscustomobject]@{ Id = "Python.Python.3.12"; Label = "Python 3.12"; Extra = @(); Need = { Test-PythonOk } }
    }
    if ($script:NeedAssets -and -not (Test-DotNetSdk)) {
        $list += [pscustomobject]@{ Id = "Microsoft.DotNet.SDK.8"; Label = ".NET 8 SDK"; Extra = @(); Need = { Test-DotNetSdk } }
    }
    if ($script:NeedVulkan -and -not (Test-Vulkan)) {
        $list += [pscustomobject]@{ Id = "KhronosGroup.VulkanSDK"; Label = "Vulkan SDK"; Extra = @(); Need = { Test-Vulkan } }
    }
    if ($script:NeedBuildTools -and -not (Test-CMake)) {
        $list += [pscustomobject]@{ Id = "Kitware.CMake"; Label = "CMake"; Extra = @(); Need = { Test-CMake } }
    }
    if ($script:NeedBuildTools -and -not (Test-Ninja)) {
        $list += [pscustomobject]@{ Id = "Ninja-build.Ninja"; Label = "Ninja"; Extra = @(); Need = { Test-Ninja } }
    }
    if ($script:NeedWav -and -not (Test-FFmpeg)) {
        $list += [pscustomobject]@{ Id = "Gyan.FFmpeg"; Label = "ffmpeg"; Extra = @(); Need = { Test-FFmpeg } }
    }
    if ($script:NeedRuntime -and -not (Test-VsCpp)) {
        $list += [pscustomobject]@{
            Id = "Microsoft.VisualStudio.2022.BuildTools"
            Label = "Visual Studio 2022 Build Tools (C++)"
            Extra = @("--override", "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended")
            Need = { Test-VsCpp }
        }
    }
    if ($script:NeedRuntime -and -not (Test-WindowsSdk)) {
        $list += [pscustomobject]@{ Id = "Microsoft.WindowsSDK.10.0.26100"; Label = "Windows SDK 10.0.26100"; Extra = @(); Need = { Test-WindowsSdk } }
    }
    return $list
}

function Show-Plan {
    Write-Host "Leonida installer plan"
    Write-Host ("  clone:  {0}" -f $script:Dir)
    if ($SkipDeps) {
        Write-Host "  install: skipped (-SkipDeps)"
    } else {
        $packages = @(Get-WantedPackages)
        if ($packages.Count -eq 0) {
            Write-Host "  install: nothing (all prerequisites present)"
        } else {
            Write-Host ("  install: {0}" -f (($packages | ForEach-Object { $_.Label }) -join ", "))
        }
        if ($script:NeedShaders) {
            if (Test-LlvmMc) {
                Write-Host "  llvm-mc: present"
            } else {
                Write-Host "  llvm-mc: would build 21 with AMDGPU from source (20 to 40 minutes)"
            }
        }
    }
    Write-Host ("  build:  {0}" -f (Join-Path $script:Dir "make_game.py"))
    if ($Game) { Write-Host ("  game:   {0}" -f $Game) }
    if ($RuntimeBuild) { Write-Host ("  runtime: using {0}" -f $RuntimeBuild) }
    Write-Host "The installer never downloads the game, keys, or game data."
}

function Confirm-Install([string[]]$Labels) {
    if ($Yes) { return }
    $text = ($Labels -join ", ")
    if ([Console]::IsInputRedirected) {
        throw ("Refusing to install packages without confirmation: {0}. Re-run with -Yes after reading -Help." -f $text)
    }
    if ($script:Anim) { [LeonidaProgress]::Pause() }
    try {
        Write-Host ("The installer will install: {0}" -f $text)
        Write-Host "winget accepts the package licences and installs them without further prompts."
        $answer = Read-Host "Continue? [Y/n]"
    } finally {
        if ($script:Anim) { [LeonidaProgress]::Resume() }
    }
    if ($answer -and $answer -notmatch '^(y|yes)$') {
        throw "cancelled"
    }
}

function Install-Prerequisites {
    Set-Step 2 "installing packages"
    if ($SkipDeps) {
        Set-Label "checking the tools on PATH"
        Set-Percent 1000
        return
    }
    if (-not (Test-Command "winget")) {
        throw "winget (App Installer) was not found. Update Windows or install App Installer from the Microsoft Store, then run again, or use -SkipDeps."
    }
    $packages = @(Get-WantedPackages)
    if ($packages.Count -eq 0) {
        Note "all prerequisites are already installed"
        Set-Percent 1000
        return
    }
    Confirm-Install ($packages | ForEach-Object { $_.Label })
    $index = 0
    foreach ($pkg in $packages) {
        $index++
        Set-Percent ([int](($index - 1) * 1000 / $packages.Count))
        if (& $pkg.Need) {
            Note ("{0} is already installed" -f $pkg.Label)
            continue
        }
        Set-Label ("installing " + $pkg.Label)
        Note ("installing {0} ({1}) with winget" -f $pkg.Label, $pkg.Id)
        $wingetArgs = @(
            "install", "--id", $pkg.Id, "--exact", "--source", "winget",
            "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity"
        )
        if ($pkg.Extra -contains "--override") {
            $wingetArgs += @($pkg.Extra)
        } else {
            $wingetArgs += @("--silent") + @($pkg.Extra)
        }
        $null = Invoke-Logged "winget" $wingetArgs $script:Log -AllowFail
        Refresh-Path
        if (-not (& $pkg.Need)) {
            throw ("winget could not install {0} ({1}). Install it by hand and run again." -f $pkg.Label, $pkg.Id)
        }
    }
    Set-Percent 1000
}

function Assert-Prerequisites {
    Add-BuildToolPath
    $missing = @()
    if (-not (Test-Command "git")) { $missing += "Git" }
    $script:Py = Find-Python
    if (-not $script:Py) { $missing += "Python 3.12 or newer" }
    if ($script:NeedAssets -and -not (Test-DotNetSdk)) { $missing += ".NET 8 SDK" }
    if ($script:NeedVulkan -and -not (Test-Vulkan)) { $missing += "Vulkan SDK" }
    if ($script:NeedBuildTools -and -not (Test-CMake)) { $missing += "CMake 3.24 or newer" }
    if ($script:NeedBuildTools -and -not (Test-Ninja)) { $missing += "Ninja" }
    if ($script:NeedWav -and -not (Test-FFmpeg)) { $missing += "ffmpeg" }
    if ($script:NeedRuntime -and -not (Test-VsCpp)) { $missing += "Visual Studio 2022 with the C++ x64 tools" }
    if ($script:NeedRuntime -and -not (Test-WindowsSdk)) { $missing += "Windows SDK 10.0.26100 or newer" }
    if ($missing.Count -gt 0) {
        throw ("Still missing: {0}. Install them and run again, or drop the stages that need them." -f ($missing -join ", "))
    }
}

function Clone-Repo {
    Set-Step 3 "cloning or updating"
    if (Test-Path (Join-Path $script:Dir ".git")) {
        $dirty = & git -C $script:Dir status --porcelain 2>$null
        if ($dirty) {
            Note ("local changes in {0}; not updating" -f $script:Dir)
        } else {
            $upstream = & git -C $script:Dir rev-parse --abbrev-ref --symbolic-full-name "@{u}" 2>$null
            if ($LASTEXITCODE -ne 0 -or -not $upstream) {
                Note "the clone has no upstream branch; not updating it"
            } else {
                $null = Invoke-Logged "git" @("-C", $script:Dir, "pull", "--ff-only") $script:Log
            }
            $null = Invoke-Logged "git" @("-C", $script:Dir, "submodule", "update", "--init", "--recursive") $script:Log
        }
    } else {
        if (Test-Path $script:Dir) {
            throw ("{0} exists but is not a git clone. Move it or pass -Dir." -f $script:Dir)
        }
        $parent = Split-Path $script:Dir -Parent
        if ($parent -and -not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        $null = Invoke-Logged "git" @("clone", "--recursive", "https://github.com/ItsNotRexed/Leonida.git", $script:Dir) $script:Log
    }
    Set-Percent 1000
}

function Resolve-Game {
    Set-Step 4 "finding your game copy"
    if ($Game) {
        if (-not (Test-Path $Game)) { throw ("The game folder was not found: {0}" -f $Game) }
        Set-Percent 1000
        return $Game
    }
    Set-Label "scanning Downloads, Desktop, Documents, Games and every drive"
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $found = @(& $script:Py.Exe @($script:Py.Args) -I $script:MakeGame --find 2>$null |
            Where-Object { $_ -match '\s+\(.+\)$' })
    } finally {
        $ErrorActionPreference = $old
    }
    if ($found.Count -eq 1) {
        $path = ($found[0] -replace '\s+\(.+\)$', "").Trim()
        Note ("game: " + $path)
        Set-Percent 1000
        return $path
    }
    if ($found.Count -gt 1) {
        Note "more than one copy was found:"
        foreach ($line in $found) { Note ("  " + $line) }
    }
    if ($Yes -or [Console]::IsInputRedirected) {
        throw "Your game files were not found automatically. Put version 1.0.2.1 on this PC and pass -Game, or run without -Yes."
    }
    if ($script:Anim) { [LeonidaProgress]::Pause() }
    try {
        Write-Host "Your game folder holds MicrosoftGame.Config and the .rpf archives, as plain files."
        $answer = Read-Host "Paste the folder (or leave empty to stop)"
    } finally {
        if ($script:Anim) { [LeonidaProgress]::Resume() }
    }
    $answer = "$answer".Trim().Trim('"')
    if (-not $answer) { throw "No game folder was given." }
    if (-not (Test-Path $answer)) { throw ("The game folder was not found: {0}" -f $answer) }
    Set-Percent 1000
    return $answer
}

function Prepare-LlvmMc {
    Set-Step 5 "checking llvm-mc"
    if (Test-LlvmMc) {
        Note "llvm-mc with the AMDGPU target is available"
        Set-Percent 1000
        return
    }
    Set-Label "building llvm-mc (20 to 40 minutes)"
    Note "The official Windows LLVM has no AMDGPU target, so llvm-mc is built once from source."
    $builder = Join-Path $script:Dir "scripts\build_llvm_mc.ps1"
    if (-not (Test-Path $builder)) { throw ("Missing {0}" -f $builder) }
    $null = Invoke-Logged "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $builder) $script:Log
    Refresh-Path
    if (-not (Test-LlvmMc)) {
        throw "llvm-mc was built but was not found. Set LLVM_MC to the llvm-mc.exe path and run again."
    }
    Set-Percent 1000
}

function Invoke-Build {
    Add-BuildToolPath
    Set-Step 6 "building the port"
    $buildArgs = @("-I", $script:MakeGame)
    if ($script:GamePath) { $buildArgs += @("--game", $script:GamePath) }
    if ($Out) { $buildArgs += @("--out", $Out) }
    if ($Work) { $buildArgs += @("--work", $Work) }
    if ($Stages) { $buildArgs += @("--stages", $Stages) }
    if ($Jobs -gt 0) { $buildArgs += @("--jobs", "$Jobs") }
    if ($Wav) { $buildArgs += "--wav" }
    if ($RuntimeBuild) { $buildArgs += @("--runtime-build", $RuntimeBuild) }
    if ($Check) { $buildArgs += "--check" }

    $pyArgs = @($script:Py.Args) + $buildArgs
    $code = Invoke-Logged $script:Py.Exe $pyArgs $script:Log -AllowFail
    Copy-Item -Path $script:Log -Destination (Join-Path $script:Dir "install.log") -Force -ErrorAction SilentlyContinue
    if ($code -ne 0) {
        throw ("the build stopped with exit code {0}" -f $code)
    }
}

# ---------------------------------------------------------------------------

if ($Help) {
    Show-Usage
    return
}

if (-not $Dir) {
    if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot "make_game.py"))) {
        $Dir = $PSScriptRoot
    } else {
        $Dir = Join-Path $HOME "Leonida"
    }
}
if ($Dir.StartsWith("~")) {
    $Dir = Join-Path $HOME ($Dir.Substring(1).TrimStart([char[]]@('\', '/')))
}
$Dir = [IO.Path]::GetFullPath($Dir)
$script:Dir = $Dir
$script:MakeGame = Join-Path $Dir "make_game.py"

if ($Stages) {
    $script:Stages = (($Stages -split ",") | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join ","
} else {
    $script:Stages = "check,shaders,assets,runtime,pipelines"
}
if (-not $script:Stages) { $script:Stages = "check,shaders,assets,runtime,pipelines" }

$script:NeedRuntime = (-not $Check) -and (-not $RuntimeBuild) -and (Test-Stage "runtime")
$script:NeedAssets = (-not $Check) -and (Test-Stage "assets")
$script:NeedShaders = (-not $Check) -and (Test-Stage "shaders")
$script:NeedPipelines = (-not $Check) -and (Test-Stage "pipelines")
$script:NeedVulkan = $script:NeedShaders -or $script:NeedPipelines
$script:NeedBuildTools = $script:NeedRuntime -or $script:NeedPipelines
$script:NeedWav = $Wav -and $script:NeedAssets
$script:GamePath = $Game

if ($DryRun) {
    Show-Plan
    return
}

$script:Anim = (-not $Plain) -and (-not $env:NO_COLOR) -and (-not [Console]::IsOutputRedirected) -and
    ($Host.Name -eq "ConsoleHost") -and ($env:TERM -ne "dumb")
if ($script:Anim) {
    $cp = ""
    try { $cp = (& chcp.com) -replace "[^0-9]", "" } catch { $cp = "" }
    $script:OldCodePage = $cp
    if ($cp -ne "65001") {
        try {
            & chcp.com 65001 > $null
            $cp = (& chcp.com) -replace "[^0-9]", ""
        } catch {
            $cp = ""
        }
    }
    if ($cp -eq "65001") {
        $script:Unicode = $true
        try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
    }
    $script:Color = -not $env:NO_COLOR
}

$progressSource = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

public static class LeonidaProgress
{
    private const int StdOutputHandle = -11;
    private const uint EnableVirtualTerminalProcessing = 0x0004;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetStdHandle(int handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetConsoleMode(IntPtr handle, out uint mode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleMode(IntPtr handle, uint mode);

    private static readonly object Gate = new object();
    private static readonly Regex Ansi = new Regex("\u001b\\[[0-9;?]*[A-Za-z]", RegexOptions.Compiled);
    private static readonly string[] SpinnerAscii = new string[] { "|", "/", "-", "\\" };
    private static readonly string[] SpinnerUnicode = new string[] {
        "\u280B", "\u2819", "\u2839", "\u2838", "\u283C", "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"
    };

    private static Thread worker;
    private static ConsoleCancelEventHandler cancelHandler;
    private static volatile bool running;
    private static volatile bool paused;
    private static bool color;
    private static bool unicode;
    private static bool vt;
    private static int step = 1;
    private static int total = 6;
    private static int permille;
    private static string label = "starting";
    private static string logPath = "";
    private static DateTime started = DateTime.UtcNow;
    private static int lastLength;

    public static void Start(bool useColor, bool useUnicode)
    {
        vt = EnableVt();
        color = useColor && vt;
        unicode = useUnicode;
        started = DateTime.UtcNow;
        running = true;
        paused = false;
        try
        {
            cancelHandler = new ConsoleCancelEventHandler(OnCancel);
            Console.CancelKeyPress += cancelHandler;
        }
        catch { }
        worker = new Thread(new ThreadStart(Loop));
        worker.IsBackground = true;
        worker.Start();
    }

    private static void OnCancel(object sender, ConsoleCancelEventArgs e)
    {
        running = false;
        lock (Gate)
        {
            ClearLocked();
            Console.Out.Write('\n');
            Console.Out.Flush();
        }
    }

    public static void SetLog(string path)
    {
        lock (Gate) { logPath = path; }
    }

    public static void Set(int newStep, int newTotal, int newPermille, string newLabel)
    {
        lock (Gate)
        {
            step = newStep;
            total = newTotal;
            permille = newPermille;
            label = newLabel == null ? "" : newLabel;
        }
    }

    public static void Pause()
    {
        paused = true;
        lock (Gate)
        {
            ClearLocked();
            Console.Out.Flush();
        }
    }

    public static void Resume()
    {
        paused = false;
    }

    public static void Message(string text)
    {
        lock (Gate)
        {
            ClearLocked();
            Console.Out.WriteLine(text);
            Console.Out.Flush();
        }
    }

    public static void Stop(bool ok, string message)
    {
        running = false;
        if (cancelHandler != null)
        {
            try { Console.CancelKeyPress -= cancelHandler; } catch { }
            cancelHandler = null;
        }
        Thread w = worker;
        worker = null;
        if (w != null)
        {
            try { w.Join(1500); } catch { }
        }
        lock (Gate)
        {
            ClearLocked();
            if (color) { Console.Out.Write(ok ? "\u001b[1;32m" : "\u001b[1;31m"); }
            Console.Out.WriteLine((ok ? "[OK] " : "[FAIL] ") + message);
            if (color) { Console.Out.Write("\u001b[0m"); }
            Console.Out.Flush();
        }
    }

    private static void Loop()
    {
        int frame = 0;
        while (running)
        {
            if (!paused) { Render(frame); }
            frame++;
            Thread.Sleep(90);
        }
    }

    private static void Render(int frame)
    {
        int s;
        int t;
        int p;
        string lab;
        string path;
        lock (Gate)
        {
            s = step;
            t = total;
            p = permille;
            lab = label;
            path = logPath;
        }

        string detail = Tail(path);
        if (String.IsNullOrEmpty(detail)) { detail = lab; }
        if (s == 6)
        {
            int mapped = StagePercent(detail);
            if (mapped > p) { p = mapped; }
        }

        int overall = (int)(((long)(s - 1) * 1000L + p) / Math.Max(1, t));
        if (overall < 0) { overall = 0; }
        if (overall > 1000) { overall = 1000; }

        int width = 100;
        try { width = Console.WindowWidth; } catch { }
        if (width < 50) { width = 50; }
        if (width > 140) { width = 140; }

        int barWidth = width - 58;
        if (barWidth < 10) { barWidth = 10; }
        if (barWidth > 44) { barWidth = 44; }

        int headMax = width - barWidth - 34;
        if (headMax < 14) { headMax = 14; }

        string[] spinner = unicode ? SpinnerUnicode : SpinnerAscii;
        string head = spinner[frame % spinner.Length] + " [" + s + "/" + t + "] " + detail;
        if (head.Length > headMax) { head = head.Substring(0, headMax - 1) + "~"; }
        else { head = head.PadRight(headMax); }

        int elapsed = (int)(DateTime.UtcNow - started).TotalSeconds;
        if (elapsed < 0) { elapsed = 0; }
        int eta = -1;
        if (overall > 0) { eta = (int)((long)elapsed * (1000 - overall) / overall); }

        string pct = (overall / 10).ToString().PadLeft(3) + "%";

        StringBuilder sb = new StringBuilder();
        sb.Append('\r');
        if (vt) { sb.Append("\u001b[K"); }
        if (color) { sb.Append("\u001b[1;36m"); }
        sb.Append(head);
        if (color) { sb.Append("\u001b[0m"); }
        sb.Append(' ');
        for (int i = 0; i < barWidth; i++)
        {
            bool on = (long)i * 1000L < (long)overall * barWidth;
            if (color)
            {
                if (on)
                {
                    int shade = barWidth > 1 ? i * 255 / (barWidth - 1) : 0;
                    int r = shade;
                    int g = 210 - 170 * shade / 255;
                    int b = 255 - 55 * shade / 255;
                    if ((i + frame) % 9 == 0) { r = 255; g = 255; b = 255; }
                    sb.Append("\u001b[38;2;").Append(r).Append(';').Append(g).Append(';').Append(b).Append('m');
                }
                else
                {
                    sb.Append("\u001b[38;2;70;70;95m");
                }
            }
            if (on) { sb.Append(unicode ? "\u2588" : "#"); }
            else { sb.Append(unicode ? "\u2591" : "-"); }
        }
        if (color) { sb.Append("\u001b[0m"); }
        sb.Append(' ');
        if (color) { sb.Append("\u001b[1;37m"); }
        sb.Append(pct);
        if (color) { sb.Append("\u001b[0m"); }
        sb.Append("  ").Append(FormatTime(elapsed)).Append(" ETA ").Append(eta < 0 ? "--:--" : FormatTime(eta));

        string line = sb.ToString();
        lock (Gate)
        {
            if (!vt)
            {
                int visible = line.Length;
                if (visible < lastLength)
                {
                    line = line + new string(' ', lastLength - visible);
                }
                lastLength = visible;
            }
            Console.Out.Write(line);
            Console.Out.Flush();
        }
    }

    private static void ClearLocked()
    {
        Console.Out.Write('\r');
        if (vt) { Console.Out.Write("\u001b[K"); }
        else if (lastLength > 0)
        {
            Console.Out.Write(new string(' ', lastLength));
            Console.Out.Write('\r');
        }
        lastLength = 0;
    }

    private static string Tail(string path)
    {
        if (String.IsNullOrEmpty(path)) { return null; }
        try
        {
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                long len = fs.Length;
                if (len <= 0) { return null; }
                int take = (int)Math.Min(len, 8192L);
                fs.Seek(len - take, SeekOrigin.Begin);
                byte[] buffer = new byte[take];
                int read = fs.Read(buffer, 0, take);
                string text = Encoding.UTF8.GetString(buffer, 0, read);
                string[] lines = text.Split(new char[] { '\n' });
                for (int i = lines.Length - 1; i >= 0; i--)
                {
                    string candidate = lines[i].Trim('\r', ' ', '\t', '\uFEFF');
                    if (candidate.Length == 0) { continue; }
                    candidate = Ansi.Replace(candidate, "");
                    StringBuilder printable = new StringBuilder(candidate.Length);
                    foreach (char c in candidate)
                    {
                        if (c >= ' ' && c != '\u007f') { printable.Append(c); }
                    }
                    if (printable.Length > 0) { return printable.ToString(); }
                }
            }
        }
        catch { }
        return null;
    }

    private static int StagePercent(string detail)
    {
        if (String.IsNullOrEmpty(detail)) { return 0; }
        string d = detail.ToLowerInvariant();
        if (d.IndexOf("finished in") >= 0) { return 1000; }
        if (d.IndexOf("pipelines:") >= 0) { return 980; }
        if (d.IndexOf("runtime:") >= 0) { return 950; }
        if (d.IndexOf("assets:") >= 0) { return 700; }
        if (d.IndexOf("shaders:") >= 0) { return 400; }
        if (d.IndexOf("check:") >= 0) { return 200; }
        return 0;
    }

    private static string FormatTime(int seconds)
    {
        if (seconds < 0) { seconds = 0; }
        int h = seconds / 3600;
        int m = (seconds % 3600) / 60;
        int sec = seconds % 60;
        if (h > 0) { return h.ToString() + ":" + m.ToString("00") + ":" + sec.ToString("00"); }
        return m.ToString("00") + ":" + sec.ToString("00");
    }

    private static bool EnableVt()
    {
        if (Environment.OSVersion.Platform != PlatformID.Win32NT) { return false; }
        try
        {
            IntPtr handle = GetStdHandle(StdOutputHandle);
            uint mode;
            if (!GetConsoleMode(handle, out mode)) { return false; }
            return SetConsoleMode(handle, mode | EnableVirtualTerminalProcessing);
        }
        catch { return false; }
    }
}
'@

if ($SelfTest) {
    Add-Type -TypeDefinition $progressSource -Language CSharp
    [LeonidaProgress]::Set(1, 6, 500, "self test")
    [LeonidaProgress]::Start($false, $false)
    [LeonidaProgress]::Message("progress helper compiled")
    [LeonidaProgress]::Stop($true, "self test")
    return
}

try {
    if ($script:Anim) {
        Add-Type -TypeDefinition $progressSource -Language CSharp
        [LeonidaProgress]::Start($script:Color, $script:Unicode)
    }

    $script:RunDir = Join-Path ([IO.Path]::GetTempPath()) ("leonida-install-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $script:RunDir | Out-Null
    $script:Log = Join-Path $script:RunDir "current.log"
    New-Item -ItemType File -Force -Path $script:Log | Out-Null
    if ($script:Anim) { [LeonidaProgress]::SetLog($script:Log) }

    Set-Step 1 "checking prerequisites"
    if ($SkipDeps) {
        Assert-Prerequisites
    } else {
        Install-Prerequisites
        Refresh-Path
        Assert-Prerequisites
    }
    Set-Percent 1000

    Clone-Repo

    if (-not (Test-Path $script:MakeGame)) {
        throw ("make_game.py was not found in {0}. Pass -Dir with a clone of the repository." -f $Dir)
    }

    $script:GamePath = Resolve-Game

    if ($Check) {
        Set-Step 6 "checking your game copy"
        $checkArgs = @("-I", $script:MakeGame, "--game", $script:GamePath, "--check")
        $code = Invoke-Logged $script:Py.Exe (@($script:Py.Args) + $checkArgs) $script:Log -AllowFail
        if ($code -ne 0) { throw "the game copy did not pass the check" }
        Set-Percent 1000
        Stop-Progress $true "check finished"
        Note ("game: " + $script:GamePath)
        return
    }

    if ($script:NeedShaders) {
        Prepare-LlvmMc
    } else {
        Set-Step 5 "llvm-mc not needed for these stages"
        Set-Percent 1000
    }

    Invoke-Build

    $outPath = $Out
    if (-not $outPath) { $outPath = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Leonida" }
    Stop-Progress $true "build finished"
    Note ("port: " + $outPath)
    Note ("log:  " + (Join-Path $Dir "install.log"))
} catch {
    Stop-Progress $false $_.Exception.Message
    if ($script:Log -and (Test-Path $script:Log)) {
        Write-Host "--- last lines ---"
        Get-Content -Path $script:Log -Tail 25 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        if (Test-Path $Dir) {
            Copy-Item -Path $script:Log -Destination (Join-Path $Dir "install.log") -Force -ErrorAction SilentlyContinue
            Write-Host ("log: " + (Join-Path $Dir "install.log"))
        }
    }
    $script:ExitCode = 1
} finally {
    if (-not $script:ProgressStopped) { Stop-Progress $false "installer stopped" }
    Restore-CodePage
    if ($script:RunDir -and (Test-Path $script:RunDir)) {
        Remove-Item -Path $script:RunDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $script:ExitCode

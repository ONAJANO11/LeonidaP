# Installing and building with the one-liner

`install.ps1` (Windows) and `install.sh` (Ubuntu) do the setup around `make_game.py`:

1. Check the machine and install what is missing.
2. Install the packages (winget on Windows, apt on Ubuntu).
3. Clone `ItsNotRexed/Leonida` with its submodule, or update an existing clone.
4. Find your copy of the game, or ask for its folder.
5. Make sure `llvm-mc` has the AMDGPU target. On Windows this builds it once from source.
6. Run `python -I make_game.py` and keep its log.

They never download the game, keys, or game data. You still need your own copy of the Xbox Series package,
version 1.0.2.1, and about 350 GB free for the work files.

The build itself is unchanged: `make_game.py` checks the game files, picks the stages, and continues a stopped
build where it left off. The installer is only the setup around it.

## Before it installs anything

Both scripts ask once, listing exactly what is missing, before they run `winget` or `apt-get`. Answer `y`, or pass
`-Yes` / `--yes`. The flag is required when input is redirected, so an unattended run cannot accept package
licences by accident. `-DryRun` / `--dry-run` uses the same checks, lists only what would actually be installed,
and exits.

The one-liners fetch `install.ps1` or `install.sh` from the current `main` branch. If you want to audit the exact
script you run, download it, read it, then run the local copy.

```powershell
$d = Join-Path $env:TEMP "leonida-install.ps1"
irm https://raw.githubusercontent.com/ItsNotRexed/Leonida/main/install.ps1 -OutFile $d
notepad $d
powershell -NoProfile -ExecutionPolicy Bypass -File $d
```

```bash
curl -fsSL https://raw.githubusercontent.com/ItsNotRexed/Leonida/main/install.sh -o /tmp/leonida-install.sh
less /tmp/leonida-install.sh
bash /tmp/leonida-install.sh
```

The installers use the vendors' official sources: `winget` and the distribution's apt repositories, the
`apt.llvm.org` repository and its signing key, `dot.net/v1/dotnet-install.sh` (run as your user, into `~/.dotnet`),
the Kitware apt repository, and the deadsnakes PPA. Those sources are fetched over HTTPS and are not pinned to a
specific revision. The deadsnakes, Kitware and LLVM apt sources stay configured after the install; the Ubuntu
section below shows how to remove them.

The game, the finished port and the work folder stay on your PC. Never share them.

## Windows

Run this in PowerShell. It downloads the script first, so arguments, `Read-Host` prompts and error messages work:

```powershell
$d = Join-Path $env:TEMP "leonida-install.ps1"; irm https://raw.githubusercontent.com/ItsNotRexed/Leonida/main/install.ps1 -OutFile $d; powershell -NoProfile -ExecutionPolicy Bypass -File $d
```

If PowerShell blocks the download, use `-ExecutionPolicy Bypass` as above, or run the script from a clone with
`powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1`.

Common options:

```powershell
.\install.ps1 -Game "D:\GTA VI"                  # skip the search
.\install.ps1 -Out "E:\Leonida" -Work "E:\work"  # keep 350 GB off the system drive
.\install.ps1 -Stages shaders,assets             # skip the runtime and pipeline stages
.\install.ps1 -Check                             # check the game copy, then stop
.\install.ps1 -SkipDeps                          # use the tools already on PATH
.\install.ps1 -Plain                             # no animation, for logs and CI
.\install.ps1 -DryRun                            # print the plan, change nothing
.\install.ps1 -Yes                               # skip the install confirmation
```

The installer uses `winget` for:

| Package | Why |
| --- | --- |
| `Git.Git` | clone and submodule |
| `Python.Python.3.12` | `make_game.py` and the tools |
| `Microsoft.DotNet.SDK.8` | `tools/rpf9dump` |
| `KhronosGroup.VulkanSDK` | `spirv-as`, `spirv-val`, `vulkaninfo` |
| `Kitware.CMake`, `Ninja-build.Ninja` | the runtime and pipeline checker |
| `Microsoft.VisualStudio.2022.BuildTools` | the C++ x64 tools for the runtime |
| `Microsoft.WindowsSDK.10.0.26100` | root signatures the runtime uses |
| `Gyan.FFmpeg` | only with `-Wav` |

Visual Studio Build Tools and the Windows SDK are large downloads. They are skipped when the selected stages do not
include `runtime`, or when `-RuntimeBuild` supplies a prebuilt runtime. `winget` changes PATH for new processes only,
so the installer rebuilds PATH in its own process after every install.

The installer checks for CMake and Ninja inside Visual Studio first. When VS 2022 already has them, it adds those
folders to PATH and does not install a second copy.

The official Windows LLVM has no AMDGPU target. If no suitable `llvm-mc` is found, the installer runs
`scripts\build_llvm_mc.ps1`, which downloads the LLVM sources and builds `llvm-mc.exe` for the AMDGPU target. That
takes 20 to 40 minutes and about 2 GB of temporary space. The result goes to `%USERPROFILE%\.leonida\llvm-mc` and
`LLVM_MC` is set for your user. A working `LLVM_MC` already in the environment, including the WSL form, is left
alone.

`-RuntimeBuild DIR` passes a prebuilt Windows `runtime` folder to `make_game.py` instead of building it.

## Ubuntu

```bash
curl -fsSL https://raw.githubusercontent.com/ItsNotRexed/Leonida/main/install.sh -o /tmp/leonida-install.sh && bash /tmp/leonida-install.sh
```

Running the downloaded file with `bash` matters: `curl ... | bash` leaves no terminal for the confirmation, the
game-folder prompt and `sudo`. If you use a pipe anyway, pass `--game` and `--yes` and make sure `sudo` is already
validated.

Common options:

```bash
bash install.sh --game /path/to/package
bash install.sh --out ~/Games/Leonida --work /mnt/games/leonida-work
bash install.sh --stages shaders,assets
bash install.sh --check
bash install.sh --skip-deps
bash install.sh --runtime --yes        # clang-cl + xwin cross-build, accepts Microsoft's licence
bash install.sh --plain
bash install.sh --dry-run
bash install.sh --yes                  # skip the install confirmation
```

The installer supports Ubuntu and Debian systems with `apt-get`; the automatic Python 3.12 and CMake 3.24
fallbacks use Ubuntu repositories. It installs:

- `git`, `curl`, `ca-certificates`, `python3`, `gnupg`
- `spirv-tools`, `libvulkan-dev`, `vulkan-tools` for the shader and pipeline stages
- `cmake` 3.24 or newer, `ninja-build`, `build-essential` for the runtime and pipeline checker
- the .NET 8 SDK for `tools/rpf9dump`, from apt or `dot.net/v1/dotnet-install.sh`
- LLVM 21 from `apt.llvm.org` when `llvm-mc` is missing or has no AMDGPU target
- Python 3.12 from the deadsnakes PPA on Ubuntu releases that still ship an older Python
- CMake 3.24 or newer from the Kitware repository when the distribution's CMake is too old
- `ffmpeg` only with `--wav`

The runtime stage is skipped by default on Linux. `--runtime` installs `clang`, `lld`, `cargo` and `xwin`, downloads
the MSVC CRT and Windows SDK, and needs `--yes` because that accepts Microsoft's licence. Without it, build the
runtime on Windows and pass `--runtime-build`.

The deadsnakes PPA, the Kitware repository and the LLVM apt repository are left configured after the install. Remove
them if you do not want them:

```bash
sudo add-apt-repository --remove ppa:deadsnakes/ppa
sudo rm -f /etc/apt/sources.list.d/kitware.list /usr/share/keyrings/kitware-archive-keyring.gpg
sudo rm -f /etc/apt/sources.list.d/llvm.list /etc/apt/trusted.gpg.d/apt.llvm.org.asc
```

The LLVM repository is added as `/etc/apt/sources.list.d/llvm.list` with the key in
`/etc/apt/trusted.gpg.d/apt.llvm.org.asc`.

## Logs and stopping

Both installers keep the output of the current command in a temporary log, show its last line in the progress line,
and print the last lines when something stops. A build that runs writes its full log to `install.log` in the clone.
The animated line shows the step, a percentage, elapsed time and an ETA as well.

Ctrl-C stops the installer and the build. Run the same command again to continue: `make_game.py` keeps its work
folder and skips stages that are already done.

## When it stops

- **`winget` was not found.** Update Windows or install App Installer from the Microsoft Store. With the tools
  already installed, `-SkipDeps` avoids winget entirely.
- **The game was not found.** Put version 1.0.2.1 on the PC, or pass `-Game` / `--game`. The folder is the one that
  holds `MicrosoftGame.Config` and the `.rpf` archives as plain files.
- **A tool is still missing after the install.** Open a new terminal so PATH changes apply, then run with
  `-SkipDeps` / `--skip-deps` to see the full list in one message.
- **`llvm-mc` has no AMDGPU target.** On Windows, delete `%USERPROFILE%\.leonida\llvm-mc` and run the installer
  again, or run `scripts\build_llvm_mc.ps1` by hand. On Linux, install `llvm-21` or set `LLVM_MC`.
- **Vulkan SDK is installed but not found.** Reopen the terminal. The installer also looks in `C:\VulkanSDK` and
  adds its `Bin` folder to PATH.
- **The animation looks wrong.** Use `-Plain` / `--plain`, or set `NO_COLOR=1`. Animation is disabled when the
  output is not a terminal.

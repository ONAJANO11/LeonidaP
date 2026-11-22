"""Run local repository checks matching CI workflows.

Usage:
    python -I scripts/run_checks.py [--list] [--only CHECKS] [--skip CHECKS]
                                    [--runtime] [--lint] [--strict]
                                    [--fail-fast] [-v]

Standard library only; works on Windows, Linux, and macOS without bash.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent

NO_GAME_DATA_RE = re.compile(
    r"\.(rpf|ttpf|xvd|dxbc|fssm|spv|spvasm|dds|bk2|awc|exe|dll)$",
    re.IGNORECASE,
)
PYTEST_FILE_MARKERS_RE = re.compile(
    r"^import pytest|^ *HAVE_NUMPY =",
    re.MULTILINE,
)
TEX_HEADERS_REL = "tests/test_tex_headers.py"
TEX_HEADERS_EXTRA_NODE = (
    "tests/test_tex_headers.py::TestTextureDescriptorParsing"
    "::test_stdlib_qword_scanner_edge_cases_and_parity"
)
JACTIONLINT_URL = "https://github.com/jdx/jactionlint"
JACTIONLINT_PROFILE_RE = re.compile(r"^\s*profile:\s*(\S+)", re.MULTILINE)


@dataclass(frozen=True)
class RunOptions:
    repo_root: Path = REPO_ROOT
    strict: bool = False
    fail_fast: bool = False
    verbose: bool = False


@dataclass
class CheckResult:
    name: str
    status: str  # "PASS", "SKIP", "FAIL"
    elapsed: float
    detail: str = ""
    output: str = ""


@dataclass(frozen=True)
class CheckSpec:
    name: str
    description: str
    default: bool
    runner: Callable[[RunOptions], CheckResult]


def _posix_rel(path: Path, repo_root: Path) -> str:
    return path.relative_to(repo_root).as_posix()


def find_game_data_files(paths: Iterable[str]) -> list[str]:
    """Return paths matching the forbidden game/binary file extensions."""
    return [p for p in paths if NO_GAME_DATA_RE.search(p)]


def discover_python_tests(repo_root: Path = REPO_ROOT) -> list[Path]:
    """Discover all tests/**/test_*.py excluding tests/semantic/*."""
    tests_dir = repo_root / "tests"
    if not tests_dir.is_dir():
        return []
    found: list[Path] = []
    for path in tests_dir.rglob("test_*.py"):
        if not path.is_file():
            continue
        rel_parts = path.relative_to(tests_dir).parts
        if rel_parts and rel_parts[0] == "semantic":
            continue
        found.append(path)
    found.sort(key=lambda p: _posix_rel(p, repo_root))
    return found


def is_pytest_file(path: Path) -> bool:
    """Return True if a test file belongs to the pytest job instead of stdlib-tests."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if PYTEST_FILE_MARKERS_RE.search(text):
        return True
    return "__main__" not in text


def partition_python_tests(repo_root: Path = REPO_ROOT) -> tuple[list[Path], list[Path]]:
    """Partition discovered tests/**/test_*.py into (stdlib_files, pytest_files)."""
    stdlib_files: list[Path] = []
    pytest_files: list[Path] = []
    for path in discover_python_tests(repo_root):
        if is_pytest_file(path):
            pytest_files.append(path)
        else:
            stdlib_files.append(path)
    return stdlib_files, pytest_files


def pytest_targets(repo_root: Path = REPO_ROOT) -> list[str]:
    """Build the exact pytest target list matching .github/workflows/ci.yml."""
    stdlib_files, pytest_files = partition_python_tests(repo_root)
    targets = [_posix_rel(p, repo_root) for p in pytest_files]
    stdlib_rels = {_posix_rel(p, repo_root) for p in stdlib_files}
    if TEX_HEADERS_REL in stdlib_rels:
        targets.append(TEX_HEADERS_EXTRA_NODE)
    return targets


def discover_viewer_tests(repo_root: Path = REPO_ROOT) -> list[Path]:
    """Discover all tests/test_*.cjs files sorted by path."""
    tests_dir = repo_root / "tests"
    if not tests_dir.is_dir():
        return []
    found = [p for p in tests_dir.glob("test_*.cjs") if p.is_file()]
    found.sort(key=lambda p: _posix_rel(p, repo_root))
    return found


def _skip_or_fail(name: str, started: float, reason: str, options: RunOptions) -> CheckResult:
    status = "FAIL" if options.strict else "SKIP"
    return CheckResult(name=name, status=status, elapsed=time.monotonic() - started, detail=reason)


def _run_cmd(
    cmd: list[str],
    cwd: Path,
    verbose: bool,
) -> tuple[int, str]:
    """Run a subprocess; stream output when verbose, otherwise capture combined output."""
    if verbose:
        print(f"  $ {' '.join(cmd)}", flush=True)
        proc = subprocess.run(cmd, cwd=str(cwd))
        return proc.returncode, ""
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout or ""


def _run_steps(
    name: str,
    started: float,
    steps: list[tuple[list[str], Path]],
    options: RunOptions,
    pass_detail: str = "",
) -> CheckResult:
    outputs: list[str] = []
    for cmd, cwd in steps:
        rc, out = _run_cmd(cmd, cwd=cwd, verbose=options.verbose)
        if out:
            outputs.append(f"$ {' '.join(cmd)}\n{out}")
        if rc != 0:
            return CheckResult(
                name=name,
                status="FAIL",
                elapsed=time.monotonic() - started,
                detail=f"exit {rc}: {' '.join(cmd)}",
                output="\n".join(outputs).rstrip(),
            )
    return CheckResult(
        name=name,
        status="PASS",
        elapsed=time.monotonic() - started,
        detail=pass_detail,
        output="\n".join(outputs).rstrip(),
    )


def check_no_game_data(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    git = shutil.which("git")
    if not git:
        return CheckResult(
            name="no-game-data",
            status="FAIL",
            elapsed=time.monotonic() - started,
            detail="git not found on PATH",
        )
    proc = subprocess.run(
        [git, "ls-files", "-z"],
        cwd=str(options.repo_root),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        return CheckResult(
            name="no-game-data",
            status="FAIL",
            elapsed=time.monotonic() - started,
            detail=f"git ls-files failed (exit {proc.returncode})",
            output=err,
        )
    tracked = [
        entry
        for entry in proc.stdout.decode("utf-8", errors="replace").split("\0")
        if entry
    ]
    bad = find_game_data_files(tracked)
    if bad:
        return CheckResult(
            name="no-game-data",
            status="FAIL",
            elapsed=time.monotonic() - started,
            detail=f"{len(bad)} forbidden file(s) tracked",
            output="Game or binary files tracked:\n" + "\n".join(bad),
        )
    return CheckResult(
        name="no-game-data",
        status="PASS",
        elapsed=time.monotonic() - started,
        detail=f"{len(tracked)} tracked files checked",
    )


def check_compileall(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    cmd = [sys.executable, "-m", "compileall", "-q", "make_game.py", "tools", "tests", "scripts"]
    return _run_steps("compileall", started, [(cmd, options.repo_root)], options)


def check_help(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    cmd = [sys.executable, "-I", "make_game.py", "--help"]
    return _run_steps("help", started, [(cmd, options.repo_root)], options)


def check_stdlib_tests(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    stdlib_files, _ = partition_python_tests(options.repo_root)
    if not stdlib_files:
        return CheckResult(
            name="stdlib-tests",
            status="PASS",
            elapsed=time.monotonic() - started,
            detail="0 files",
        )
    failed: list[str] = []
    outputs: list[str] = []
    for path in stdlib_files:
        rel = _posix_rel(path, options.repo_root)
        cmd = [sys.executable, "-I", str(path)]
        if options.verbose:
            print(f"  == {rel}", flush=True)
        rc, out = _run_cmd(cmd, cwd=options.repo_root, verbose=options.verbose)
        if rc != 0:
            failed.append(rel)
            header = f"== {rel} (exit {rc})"
            outputs.append(f"{header}\n{out}".rstrip() if out else header)
            if options.fail_fast:
                break
    if failed:
        return CheckResult(
            name="stdlib-tests",
            status="FAIL",
            elapsed=time.monotonic() - started,
            detail=f"{len(failed)}/{len(stdlib_files)} failed ({', '.join(failed[:3])}"
            + ("..." if len(failed) > 3 else "")
            + ")",
            output="\n\n".join(outputs),
        )
    return CheckResult(
        name="stdlib-tests",
        status="PASS",
        elapsed=time.monotonic() - started,
        detail=f"{len(stdlib_files)} files",
    )


def check_pytest(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    probe = subprocess.run(
        [sys.executable, "-m", "pytest", "--version"],
        cwd=str(options.repo_root),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if probe.returncode != 0:
        return _skip_or_fail(
            "pytest",
            started,
            "pytest not installed (hint: pip install -r tests/requirements.txt)",
            options,
        )
    targets = pytest_targets(options.repo_root)
    if not targets:
        return CheckResult(
            name="pytest",
            status="PASS",
            elapsed=time.monotonic() - started,
            detail="0 targets",
        )
    cmd = [sys.executable, "-m", "pytest", *targets]
    if not options.verbose:
        cmd.append("-q")
    return _run_steps(
        "pytest",
        started,
        [(cmd, options.repo_root)],
        options,
        pass_detail=f"{len(targets)} targets",
    )


def check_semantic(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    cmd = [sys.executable, "-m", "unittest", "discover", "-s", "tests/semantic"]
    if options.verbose:
        cmd.append("-v")
    return _run_steps("semantic", started, [(cmd, options.repo_root)], options)


def check_viewer(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    node = shutil.which("node")
    if not node:
        return _skip_or_fail(
            "viewer",
            started,
            "node not found on PATH (install Node.js 22+ to run viewer tests)",
            options,
        )
    files = discover_viewer_tests(options.repo_root)
    if not files:
        return CheckResult(
            name="viewer",
            status="PASS",
            elapsed=time.monotonic() - started,
            detail="0 files",
        )
    cmd = [node, "--test", *(_posix_rel(p, options.repo_root) for p in files)]
    return _run_steps(
        "viewer",
        started,
        [(cmd, options.repo_root)],
        options,
        pass_detail=f"{len(files)} file(s)",
    )


def check_dotnet(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    dotnet = shutil.which("dotnet")
    if not dotnet:
        return _skip_or_fail(
            "dotnet",
            started,
            "dotnet not found on PATH (install .NET 8 SDK; see global.json)",
            options,
        )
    submodule = options.repo_root / "third_party" / "rpf9-explorer"
    if not submodule.is_dir() or not any(submodule.iterdir()):
        return _skip_or_fail(
            "dotnet",
            started,
            "third_party/rpf9-explorer not populated (hint: git submodule update --init)",
            options,
        )
    steps = [
        ([dotnet, "build", "tools/rpf9dump/rpf9dump.csproj", "-c", "Release"], options.repo_root),
        (
            [dotnet, "run", "--project", "tests/Rpf9DumpDiagnostics/Rpf9DumpDiagnostics.csproj", "-c", "Release"],
            options.repo_root,
        ),
    ]
    return _run_steps("dotnet", started, steps, options)


def _find_cmake_tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    if os.name == "nt":
        prog_x86 = os.environ.get("ProgramFiles(x86)")
        if prog_x86:
            vswhere = Path(prog_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            if vswhere.is_file():
                proc = subprocess.run(
                    [
                        str(vswhere),
                        "-latest",
                        "-products",
                        "*",
                        "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-property",
                        "installationPath",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                install = proc.stdout.strip()
                if install:
                    cand = (
                        Path(install)
                        / "Common7"
                        / "IDE"
                        / "CommonExtensions"
                        / "Microsoft"
                        / "CMake"
                        / "CMake"
                        / "bin"
                        / f"{name}.exe"
                    )
                    if cand.is_file():
                        return str(cand)
    return None


def _find_ctest_executable() -> str | None:
    return _find_cmake_tool("ctest")


def select_cmake_preset(presets_data: dict, platform: str | None = None) -> str | None:
    """Return the matching configurePreset name for the host platform, or None."""
    plat = sys.platform if platform is None else platform
    if plat == "win32":
        candidate = "windows-ninja"
    elif plat.startswith("linux"):
        candidate = "linux-ninja"
    else:
        return None
    configure_names = {
        entry.get("name")
        for entry in presets_data.get("configurePresets", [])
        if isinstance(entry, dict)
    }
    return candidate if candidate in configure_names else None


def _preset_binary_dir(presets_data: dict, preset_name: str, runtime_dir: Path) -> Path:
    for entry in presets_data.get("configurePresets", []):
        if isinstance(entry, dict) and entry.get("name") == preset_name and entry.get("binaryDir"):
            raw = str(entry["binaryDir"]).replace("${sourceDir}", str(runtime_dir))
            return Path(raw)
    return runtime_dir / "build"


def _run_cmake_preset(
    started: float,
    cmake: str,
    ctest: str,
    preset: str,
    presets_data: dict,
    runtime_dir: Path,
    options: RunOptions,
) -> CheckResult:
    build_names = {
        p.get("name") for p in presets_data.get("buildPresets", []) if isinstance(p, dict)
    }
    test_names = {
        p.get("name") for p in presets_data.get("testPresets", []) if isinstance(p, dict)
    }
    binary_dir = _preset_binary_dir(presets_data, preset, runtime_dir)

    steps: list[tuple[list[str], Path]] = [([cmake, "--preset", preset], runtime_dir)]
    if preset in build_names:
        steps.append(([cmake, "--build", "--preset", preset], runtime_dir))
    else:
        steps.append(([cmake, "--build", str(binary_dir)], options.repo_root))
    if preset in test_names:
        steps.append(([ctest, "--preset", preset, "--output-on-failure", "--no-tests=error"], runtime_dir))
    else:
        steps.append(([ctest, "--test-dir", str(binary_dir), "--output-on-failure", "--no-tests=error"], options.repo_root))
    return _run_steps("runtime", started, steps, options, pass_detail=f"preset {preset}")


def check_runtime(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    runtime_dir = options.repo_root / "runtime"
    presets_path = runtime_dir / "CMakePresets.json"

    presets_data: dict = {}
    preset: str | None = None
    if presets_path.is_file():
        try:
            loaded = json.loads(presets_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                presets_data = loaded
        except (OSError, ValueError):
            presets_data = {}
        preset = select_cmake_preset(presets_data)

    cmake = _find_cmake_tool("cmake")
    ctest = _find_ctest_executable()
    if not cmake or not ctest:
        return _skip_or_fail(
            "runtime",
            started,
            "cmake/ctest not found on PATH",
            options,
        )

    if sys.platform == "win32":
        has_cl = shutil.which("cl") is not None
        if preset is not None:
            if has_cl:
                return _run_cmake_preset(started, cmake, ctest, preset, presets_data, runtime_dir, options)
            build_script = options.repo_root / "scripts" / "build_runtime.ps1"
            ps_exe = shutil.which("pwsh") or shutil.which("powershell")
            if build_script.is_file() and ps_exe:
                binary_dir = _preset_binary_dir(presets_data, preset, runtime_dir)
                steps: list[tuple[list[str], Path]] = [
                    (
                        [ps_exe, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(build_script)],
                        options.repo_root,
                    ),
                    (
                        [ctest, "--test-dir", str(binary_dir), "--output-on-failure", "--no-tests=error"],
                        options.repo_root,
                    ),
                ]
                return _run_steps("runtime", started, steps, options, pass_detail=f"build_runtime.ps1 + {preset}")
    elif preset is not None:
        return _run_cmake_preset(started, cmake, ctest, preset, presets_data, runtime_dir, options)

    use_ninja = shutil.which("ninja") is not None and (sys.platform != "win32" or shutil.which("cl") is not None)
    configure_cmd = [cmake, "-S", "runtime", "-B", "runtime/build"]
    if use_ninja:
        configure_cmd.extend(["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"])
    elif sys.platform != "win32":
        configure_cmd.append("-DCMAKE_BUILD_TYPE=RelWithDebInfo")
    if sys.platform == "win32":
        configure_cmd.append("-DLEONIDA_D3D12_SHIM_TESTS=ON")
    build_cmd = [cmake, "--build", "runtime/build"]
    ctest_cmd = [ctest, "--test-dir", "runtime/build", "--output-on-failure", "--no-tests=error"]
    if sys.platform == "win32" and not use_ninja:
        build_cmd.extend(["--config", "RelWithDebInfo"])
        ctest_cmd.extend(["-C", "RelWithDebInfo"])
    steps = [
        (configure_cmd, options.repo_root),
        (build_cmd, options.repo_root),
        (ctest_cmd, options.repo_root),
    ]
    return _run_steps("runtime", started, steps, options)


def jactionlint_profile(repo_root: Path = REPO_ROOT) -> str:
    """Read jactionlint profile from .github/workflows/jactionlint.yml, defaulting to 'default'."""
    workflow = repo_root / ".github" / "workflows" / "jactionlint.yml"
    try:
        text = workflow.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "default"
    match = JACTIONLINT_PROFILE_RE.search(text)
    return match.group(1) if match else "default"


def check_lint(options: RunOptions) -> CheckResult:
    started = time.monotonic()
    exe = shutil.which("jactionlint")
    if not exe:
        return _skip_or_fail(
            "lint",
            started,
            f"jactionlint not found on PATH (install: {JACTIONLINT_URL})",
            options,
        )
    profile = jactionlint_profile(options.repo_root)
    return _run_steps(
        "lint",
        started,
        [([exe, "--profile", profile], options.repo_root)],
        options,
        pass_detail=f"profile {profile}",
    )


CHECKS: tuple[CheckSpec, ...] = (
    CheckSpec(
        name="no-game-data",
        description="Verify no game data or binary files are tracked by git",
        default=True,
        runner=check_no_game_data,
    ),
    CheckSpec(
        name="compileall",
        description="Byte-compile make_game.py, tools, tests, and scripts",
        default=True,
        runner=check_compileall,
    ),
    CheckSpec(
        name="help",
        description="Run python -I make_game.py --help",
        default=True,
        runner=check_help,
    ),
    CheckSpec(
        name="stdlib-tests",
        description="Run standard-library Python unit tests under python -I",
        default=True,
        runner=check_stdlib_tests,
    ),
    CheckSpec(
        name="pytest",
        description="Run pytest-marked Python tests (plus tex_headers parity case)",
        default=True,
        runner=check_pytest,
    ),
    CheckSpec(
        name="semantic",
        description="Run translator semantic tests (unittest discover -s tests/semantic)",
        default=True,
        runner=check_semantic,
    ),
    CheckSpec(
        name="viewer",
        description="Run Node.js viewer streaming tests (tests/test_*.cjs)",
        default=True,
        runner=check_viewer,
    ),
    CheckSpec(
        name="dotnet",
        description="Build tools/rpf9dump and run Rpf9DumpDiagnostics",
        default=True,
        runner=check_dotnet,
    ),
    CheckSpec(
        name="runtime",
        description="Build C++ runtime and run ctest (opt-in)",
        default=False,
        runner=check_runtime,
    ),
    CheckSpec(
        name="lint",
        description="Lint GitHub Actions workflows with jactionlint (opt-in)",
        default=False,
        runner=check_lint,
    ),
)

CHECKS_BY_NAME: dict[str, CheckSpec] = {c.name: c for c in CHECKS}


def _parse_check_list(values: list[str] | None) -> list[str]:
    if not values:
        return []
    names: list[str] = []
    for raw in values:
        for part in raw.split(","):
            name = part.strip()
            if not name:
                raise ValueError(f"empty check name in {raw!r}")
            names.append(name)
    return names


def resolve_selected_checks(
    only: list[str] | None = None,
    skip: list[str] | None = None,
    include_runtime: bool = False,
    include_lint: bool = False,
) -> list[CheckSpec]:
    """Resolve the ordered list of checks to run from CLI flags."""
    only_names = _parse_check_list(only)
    skip_names = _parse_check_list(skip)

    unknown = [n for n in (*only_names, *skip_names) if n not in CHECKS_BY_NAME]
    if unknown:
        valid = ", ".join(c.name for c in CHECKS)
        raise ValueError(f"unknown check(s): {', '.join(unknown)} (valid: {valid})")

    if only_names:
        chosen: list[str] = []
        for name in only_names:
            if name not in chosen:
                chosen.append(name)
        if include_runtime and "runtime" not in chosen:
            chosen.append("runtime")
        if include_lint and "lint" not in chosen:
            chosen.append("lint")
    else:
        chosen = [c.name for c in CHECKS if c.default]
        if include_runtime:
            chosen.append("runtime")
        if include_lint:
            chosen.append("lint")

    skip_set = set(skip_names)
    return [CHECKS_BY_NAME[name] for name in chosen if name not in skip_set]


def format_check_list() -> str:
    width = max(len(c.name) for c in CHECKS)
    lines = []
    for c in CHECKS:
        kind = "default" if c.default else "opt-in"
        lines.append(f"  {c.name:<{width}}  [{kind:<7}]  {c.description}")
    return "\n".join(lines)


def format_summary(results: list[CheckResult]) -> str:
    if not results:
        return "No checks selected."
    width = max(len(r.name) for r in results)
    lines = ["", "Summary:"]
    for r in results:
        detail = f"  {r.detail}" if r.detail else ""
        lines.append(f"  {r.status:<4}  {r.name:<{width}}  {r.elapsed:6.2f}s{detail}")
    passed = sum(1 for r in results if r.status == "PASS")
    skipped = sum(1 for r in results if r.status == "SKIP")
    failed = sum(1 for r in results if r.status == "FAIL")
    total = sum(r.elapsed for r in results)
    lines.append(f"Total: {passed} passed, {skipped} skipped, {failed} failed ({total:.2f}s)")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run local repository checks matching CI workflows.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available checks and exit.",
    )
    parser.add_argument(
        "--only",
        action="append",
        metavar="CHECKS",
        help="Comma-separated check names to run instead of defaults (repeatable).",
    )
    parser.add_argument(
        "--skip",
        action="append",
        metavar="CHECKS",
        help="Comma-separated check names to skip (repeatable).",
    )
    parser.add_argument(
        "--runtime",
        action="store_true",
        help="Include the optional C++ runtime build and ctest check.",
    )
    parser.add_argument(
        "--lint",
        action="store_true",
        help="Include the optional jactionlint workflow check.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Treat missing optional tools or uninitialized submodule as failures.",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="Stop after the first failing check.",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Stream full output for passing checks too.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.list:
        print(format_check_list())
        return 0

    try:
        selected = resolve_selected_checks(
            only=args.only,
            skip=args.skip,
            include_runtime=args.runtime,
            include_lint=args.lint,
        )
    except ValueError as exc:
        parser.error(str(exc))

    options = RunOptions(
        repo_root=REPO_ROOT,
        strict=args.strict,
        fail_fast=args.fail_fast,
        verbose=args.verbose,
    )

    results: list[CheckResult] = []
    for spec in selected:
        if options.verbose:
            print(f"==> {spec.name}", flush=True)
        res = spec.runner(options)
        results.append(res)
        detail = f" ({res.detail})" if res.detail else ""
        print(f"[{res.status}] {res.name} ({res.elapsed:.2f}s){detail}", flush=True)
        if res.status == "FAIL" and res.output and not options.verbose:
            print(res.output, flush=True)
        if res.status == "FAIL" and options.fail_fast:
            break

    print(format_summary(results), flush=True)
    return 1 if any(r.status == "FAIL" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())

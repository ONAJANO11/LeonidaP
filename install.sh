#!/usr/bin/env bash
# install.sh - bootstraps the Leonida toolchain on Linux, clones the repository, and starts the build.
# It never downloads the game, keys, or any game data.
set -euo pipefail

DIR="${LEONIDA_DIR:-$HOME/Leonida}"
REPO="${LEONIDA_REPO:-https://github.com/ItsNotRexed/Leonida.git}"
GAME=""
OUT=""
WORK=""
STAGES=""
JOBS=""
WAV=0
CHECK=0
SKIP_DEPS=0
DRY_RUN=0
RUNTIME=0
RUNTIME_BUILD=""
PLAIN=0
YES=0
SELFTEST=0

usage() {
  cat <<'EOF'
Leonida installer (Linux)

Usage: install.sh [options]

  --dir DIR            clone here (default: this checkout, or ~/Leonida)
  --game DIR           your Xbox Series package (found automatically if omitted)
  --out DIR            finished port (default: ~/Documents/Leonida)
  --work DIR           intermediate files, about 350 GB (default: <out>/work)
  --stages LIST        comma list: check,shaders,assets,runtime,pipelines
  --jobs N             worker processes (default: half the CPU threads)
  --wav                also decode Opus to PCM WAV (large)
  --runtime            install the clang-cl + xwin cross-build tools (accepts Microsoft's licence)
  --runtime-build DIR  use a Windows build of runtime/ instead of cross-building
  --check              check the game copy and stop
  --skip-deps          install nothing; use the tools already on PATH
  --plain              no animation or color
  --dry-run            print what would run, change nothing
  --yes                skip the install confirmation (required when there is no terminal)
  --self-test          animate every step briefly and exit (used by CI)
  -h, --help           this text

The installer never downloads the game, keys, or game data.
EOF
}

die() {
  FAIL_MSG=$*
  if [ "${TRAP_READY:-0}" -eq 1 ]; then exit 1; fi
  printf 'error: %s\n' "$*" >&2
  exit 1
}

need_value() {
  if [ "$#" -lt 2 ]; then die "option $1 needs a value"; fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dir) need_value "$@"; DIR=$2; shift 2 ;;
    --game) need_value "$@"; GAME=$2; shift 2 ;;
    --out) need_value "$@"; OUT=$2; shift 2 ;;
    --work) need_value "$@"; WORK=$2; shift 2 ;;
    --stages) need_value "$@"; STAGES=$2; shift 2 ;;
    --jobs) need_value "$@"; JOBS=$2; shift 2 ;;
    --runtime-build) need_value "$@"; RUNTIME_BUILD=$2; shift 2 ;;
    --wav) WAV=1; shift ;;
    --check) CHECK=1; shift ;;
    --skip-deps) SKIP_DEPS=1; shift ;;
    --runtime) RUNTIME=1; shift ;;
    --plain) PLAIN=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --yes) YES=1; shift ;;
    --self-test) SELFTEST=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

if [ -z "${LEONIDA_DIR:-}" ]; then
  self_dir=$(cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || true)
  if [ -n "$self_dir" ] && [ -f "$self_dir/make_game.py" ]; then DIR=$self_dir; fi
fi

want_stage() {
  if [ "$CHECK" -eq 1 ]; then return 1; fi
  local list=${STAGES:-check,shaders,assets,runtime,pipelines}
  list=${list//[[:space:]]/}
  case ",$list," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}

cmake_ok() {
  command -v cmake >/dev/null 2>&1 || return 1
  local v
  v=$(cmake --version 2>/dev/null | head -n 1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n 1 || true)
  [ -n "$v" ] || return 1
  [ "$(printf '%s\n' "3.24" "$v" | sort -V | head -n 1)" = "3.24" ]
}

find_llvm_mc() {
  local mc=""
  if [ -n "${LLVM_MC:-}" ] && [ ! -x "$LLVM_MC" ]; then return 1; fi
  if [ -n "${LLVM_MC:-}" ]; then mc=$LLVM_MC; fi
  if [ -z "$mc" ] && command -v llvm-mc >/dev/null 2>&1; then mc=$(command -v llvm-mc); fi
  if [ -z "$mc" ] && command -v llvm-mc-21 >/dev/null 2>&1; then mc=$(command -v llvm-mc-21); fi
  if [ -z "$mc" ] && [ -x /usr/lib/llvm-21/bin/llvm-mc ]; then mc=/usr/lib/llvm-21/bin/llvm-mc; fi
  [ -n "$mc" ] || return 1
  printf '%s\n' "$mc"
}

llvm_ok() {
  if [ -n "${LLVM_MC:-}" ] && [ ! -x "$LLVM_MC" ]; then return 0; fi
  local mc
  mc=$(find_llvm_mc) || return 1
  "$mc" --version 2>/dev/null | grep -q amdgcn
}

python_ok() {
  command -v "$1" >/dev/null 2>&1 || return 1
  "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3,12) else 1)' >/dev/null 2>&1
}

dotnet_sdk_ok() {
  command -v dotnet >/dev/null 2>&1 || return 1
  dotnet --list-sdks 2>/dev/null | awk -F. '$1 + 0 >= 8 { found = 1 } END { exit found ? 0 : 1 }'
}

pick_python() {
  local c
  for c in python3 python3.12 python3.13 python3.14; do
    if python_ok "$c"; then PY=$c; return 0; fi
  done
  return 1
}

plan_missing() {
  local missing=()
  if ! command -v git >/dev/null 2>&1; then missing+=(git); fi
  if ! command -v curl >/dev/null 2>&1; then missing+=(curl); fi
  if ! pick_python; then missing+=("Python 3.12+"); fi
  if want_stage shaders || want_stage pipelines; then
    if ! command -v spirv-as >/dev/null 2>&1 || ! command -v spirv-val >/dev/null 2>&1; then
      missing+=(spirv-tools)
    fi
  fi
  if want_stage runtime || want_stage pipelines; then
    if ! cmake_ok; then missing+=("CMake 3.24+"); fi
    if ! command -v ninja >/dev/null 2>&1; then missing+=(ninja-build); fi
    if ! command -v make >/dev/null 2>&1; then missing+=(build-essential); fi
  fi
  if want_stage assets && ! dotnet_sdk_ok; then
    missing+=(".NET 8 SDK")
  fi
  if [ "$WAV" -eq 1 ] && want_stage assets && ! command -v ffmpeg >/dev/null 2>&1; then
    missing+=(ffmpeg)
  fi
  if want_stage shaders && ! llvm_ok; then
    missing+=("LLVM 21 with AMDGPU")
  fi
  if [ "${#missing[@]}" -gt 0 ]; then printf '%s\n' "${missing[@]}"; fi
}

if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
  printf 'Run this as your normal user, not with sudo. The script uses sudo for packages.\n' >&2
  exit 1
fi

show_plan() {
  printf 'Leonida installer plan\n'
  printf '  clone:  %s\n' "$DIR"
  if [ "$SKIP_DEPS" -eq 0 ]; then
    local missing
    missing=$(plan_missing | awk '!seen[$0]++ { if (n++) printf ", "; printf "%s", $0 }')
    if [ -z "$missing" ]; then
      printf '  install: nothing (all prerequisites present)\n'
    else
      printf '  install: %s\n' "$missing"
    fi
  else
    printf '  install: skipped (--skip-deps)\n'
  fi
  if [ "$RUNTIME" -eq 1 ]; then
    printf '  runtime: clang lld cargo, xwin (accepts Microsoft licence)\n'
  fi
  if [ -n "$RUNTIME_BUILD" ]; then
    printf '  runtime: using %s\n' "$RUNTIME_BUILD"
  fi
  printf '  build:  %s/make_game.py\n' "$DIR"
  if [ "$CHECK" -eq 1 ]; then printf '  mode:   check only\n'; fi
  if [ -n "$GAME" ]; then printf '  game:   %s\n' "$GAME"; fi
  printf 'The installer never downloads the game, keys, or game data.\n'
}

if [ "$DRY_RUN" -eq 1 ]; then
  show_plan
  exit 0
fi

if [ "$SKIP_DEPS" -eq 0 ] && [ "$SELFTEST" -eq 0 ]; then
  if ! command -v apt-get >/dev/null 2>&1; then
    die "This installer supports Debian/Ubuntu. Install the prerequisites by hand and use --skip-deps."
  fi
  if ! command -v curl >/dev/null 2>&1; then
    die "curl is required to download LLVM. Install curl and run again."
  fi
  if [ "$(id -u)" -ne 0 ]; then
    printf 'sudo may ask for your password once.\n'
    sudo -v || die "sudo is required for the packages"
  fi
fi

TMP="${TMPDIR:-/tmp}"
PY=python3
RUN_DIR=$(mktemp -d "${TMP%/}/leonida-install.XXXXXX")
STATE="$RUN_DIR/state"
MSGS="$RUN_DIR/messages"
LOG="$RUN_DIR/current.log"
: > "$STATE"
: > "$MSGS"
: > "$LOG"
TOTAL=6
STEP=1
PERMILLE=0
LABEL="checking prerequisites"
FAIL_MSG=""
SECONDS=0

ANIM=0
COLOR=0
UTF8=0
FILL='#'
EMPTY='-'
BAR_ON='##########'
BAR_OFF='----------'
if [ -t 1 ] && [ "${TERM:-}" != "dumb" ] && [ "$PLAIN" -eq 0 ] && [ -z "${NO_COLOR:-}" ]; then ANIM=1; fi
if [ "$ANIM" -eq 1 ] && [ -z "${NO_COLOR:-}" ]; then COLOR=1; fi
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
  *UTF-8*|*utf-8*|*UTF8*|*utf8*) UTF8=1 ;;
esac
if [ "$UTF8" -eq 1 ] && [ "$ANIM" -eq 1 ]; then FILL='█'; EMPTY='░'; fi
SPIN='|/-\'
if [ "$UTF8" -eq 1 ] && [ "$ANIM" -eq 1 ]; then SPIN='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'; fi
COLS=$(tput cols 2>/dev/null || echo 100)
case "$COLS" in ''|*[!0-9]*) COLS=100 ;; esac
if [ "$COLS" -lt 50 ]; then COLS=50; fi
if [ "$COLS" -gt 120 ]; then COLS=120; fi

state() { printf '%s %s %s %s\n' "$STEP" "$TOTAL" "$PERMILLE" "$LABEL" > "$STATE"; }
set_step() {
  STEP=$1
  PERMILLE=0
  LABEL=$2
  if [ "$ANIM" -eq 1 ]; then
    state
  else
    local filled=$((10 * STEP / TOTAL))
    if [ "$filled" -gt 10 ]; then filled=10; fi
    printf '[%s%s] %s/%s %s\n' "${BAR_ON:0:filled}" "${BAR_OFF:0:$((10 - filled))}" "$STEP" "$TOTAL" "$LABEL"
  fi
}
set_percent() {
  PERMILLE=$1
  if [ "$ANIM" -eq 1 ]; then state; fi
}
set_label() {
  LABEL=$1
  if [ "$ANIM" -eq 1 ]; then
    state
  else
    local filled=$((10 * STEP / TOTAL))
    if [ "$filled" -gt 10 ]; then filled=10; fi
    printf '[%s%s] %s/%s %s\n' "${BAR_ON:0:filled}" "${BAR_OFF:0:$((10 - filled))}" "$STEP" "$TOTAL" "$LABEL"
  fi
}
log() {
  if [ "$ANIM" -eq 1 ]; then printf '%s\n' "$*" >> "$MSGS"; else printf '%s\n' "$*"; fi
}
flush_messages() {
  if [ -s "$MSGS" ]; then cat "$MSGS"; : > "$MSGS"; fi
}
fmt_time() {
  local s=${1:-0}
  if [ "$s" -lt 0 ]; then s=0; fi
  printf '%02d:%02d' $((s / 60)) $((s % 60))
}

pause_anim() {
  if [ "$ANIM" -eq 1 ]; then
    : > "$RUN_DIR/pause"
    sleep 0.15
  fi
}

resume_anim() {
  if [ "$ANIM" -eq 1 ]; then rm -f "$RUN_DIR/pause"; fi
}

animator() {
  local frame=0
  local step=1 total=6 permille=0 label=""
  local last detail head pad pct elapsed eta bar_width head_max i on pos span t r g b ch
  local paused=0
  while :; do
    if [ -f "$RUN_DIR/pause" ]; then
      if [ "$paused" -eq 0 ]; then printf '\r\033[K'; paused=1; fi
      sleep 0.09
      continue
    fi
    paused=0
    if [ -s "$MSGS" ]; then
      printf '\r\033[K'
      cat "$MSGS"
      : > "$MSGS"
    fi
    if ! IFS=' ' read -r step total permille label < "$STATE" 2>/dev/null; then
      step=1; total=6; permille=0; label="starting"
    fi
    last=$(tail -n 1 "$LOG" 2>/dev/null | tr '\r' '\n' | tail -n 1 \
      | sed -e 's/\x1b\[[0-9;?]*[A-Za-z]//g' -e 's/\x1b[()][A-Za-z0-9]//g' \
      | tr -cd '[:print:]' || true)
    if [ "$step" -eq 6 ] && [ -n "$last" ]; then
      case "$last" in
        *"check:"*) permille=20 ;;
        *"shaders:"*) permille=400 ;;
        *"assets:"*) permille=700 ;;
        *"runtime:"*) permille=950 ;;
        *"pipelines:"*) permille=980 ;;
        *"finished in"*) permille=1000 ;;
      esac
    fi
    if [ -z "$last" ]; then last=$label; fi
    detail=$last
    bar_width=$((COLS - 52))
    if [ "$bar_width" -lt 10 ]; then bar_width=10; fi
    if [ "$bar_width" -gt 40 ]; then bar_width=40; fi
    head_max=$((COLS - bar_width - 26))
    if [ "$head_max" -lt 12 ]; then head_max=12; fi
    head="${SPIN:$((frame % ${#SPIN})):1} [$step/$total] $detail"
    if [ "${#head}" -gt "$head_max" ]; then
      head="${head:0:$((head_max - 1))}~"
    fi
    if [ "${#head}" -lt "$head_max" ]; then
      printf -v pad '%*s' $((head_max - ${#head})) ''
      head="$head$pad"
    fi
    if [ "$permille" -lt 0 ]; then
      pct=' --%'
      eta='--:--'
    else
      printf -v pct '%3d%%' $((permille / 10))
      elapsed=$SECONDS
      if [ "$permille" -gt 0 ]; then
        printf -v eta '%02d:%02d' $((elapsed * (1000 - permille) / permille / 60)) $((elapsed * (1000 - permille) / permille % 60))
      else
        eta='--:--'
      fi
    fi
    elapsed=$SECONDS
    printf '\r\033[K'
    if [ "$COLOR" -eq 1 ]; then printf '\033[1;36m'; fi
    printf '%s' "$head"
    if [ "$COLOR" -eq 1 ]; then printf '\033[0m'; fi
    printf ' '
    if [ "$permille" -lt 0 ]; then
      span=$((bar_width / 3))
      if [ "$span" -lt 4 ]; then span=4; fi
      pos=$(( (frame * 2) % (bar_width + span) ))
    else
      span=0
      pos=0
    fi
    i=0
    while [ "$i" -lt "$bar_width" ]; do
      on=0
      if [ "$permille" -lt 0 ]; then
        if [ "$i" -ge $((pos - span)) ] && [ "$i" -le "$pos" ]; then on=1; fi
      else
        if [ $((i * 1000)) -lt $((permille * bar_width)) ]; then on=1; fi
      fi
      if [ "$on" -eq 1 ]; then ch=$FILL; else ch=$EMPTY; fi
      if [ "$COLOR" -eq 1 ]; then
        if [ "$on" -eq 1 ]; then
          t=$(( i * 255 / (bar_width - 1) ))
          r=$t
          g=$(( 210 - 170 * t / 255 ))
          b=$(( 255 - 55 * t / 255 ))
          if [ $(( (i + frame) % 9 )) -eq 0 ]; then r=255; g=255; b=255; fi
          printf '\033[38;2;%d;%d;%dm%s' "$r" "$g" "$b" "$ch"
        else
          printf '\033[38;2;70;70;95m%s' "$ch"
        fi
      else
        printf '%s' "$ch"
      fi
      i=$((i + 1))
    done
    if [ "$COLOR" -eq 1 ]; then printf '\033[0m'; fi
    if [ "$COLOR" -eq 1 ]; then
      printf ' \033[1;37m%s\033[0m  %s ETA %s' "$pct" "$(fmt_time "$elapsed")" "$eta"
    else
      printf ' %s  %s ETA %s' "$pct" "$(fmt_time "$elapsed")" "$eta"
    fi
    frame=$((frame + 1))
    sleep 0.09
  done
}

cleanup() {
  local rc=$?
  if [ -n "${ANIM_PID:-}" ]; then
    kill "$ANIM_PID" 2>/dev/null || true
    wait "$ANIM_PID" 2>/dev/null || true
  fi
  if [ "$rc" -ne 0 ]; then
    printf '\r\033[K' >&2
    if [ -s "$MSGS" ]; then cat "$MSGS" >&2; fi
    if [ "$COLOR" -eq 1 ]; then
      printf '\033[1;31m[FAIL] %s\033[0m\n' "${FAIL_MSG:-installer stopped}" >&2
    else
      printf '[FAIL] %s\n' "${FAIL_MSG:-installer stopped}" >&2
    fi
    if [ -s "$LOG" ]; then
      printf '%s\n' '--- last lines ---' >&2
      tail -n 25 "$LOG" >&2 || true
    fi
    if [ -s "$LOG" ] && [ -d "$DIR" ]; then
      cp "$LOG" "$DIR/install.log" 2>/dev/null || true
    fi
    if [ -f "$DIR/install.log" ]; then printf 'log: %s\n' "$DIR/install.log" >&2; fi
  fi
  rm -rf "$RUN_DIR"
}
trap cleanup EXIT
TRAP_READY=1

finish() {
  if [ "$ANIM" -eq 1 ]; then
    PERMILLE=1000
    state
    sleep 0.15
    if [ -n "${ANIM_PID:-}" ]; then
      kill "$ANIM_PID" 2>/dev/null || true
      wait "$ANIM_PID" 2>/dev/null || true
    fi
    printf '\r\033[K'
    flush_messages
  fi
  if [ "$COLOR" -eq 1 ]; then
    printf '\033[1;32m[OK] %s (%s)\033[0m\n' "$1" "$(fmt_time "$SECONDS")"
  else
    printf '[OK] %s (%s)\n' "$1" "$(fmt_time "$SECONDS")"
  fi
}

run_capture() {
  local logfile=$1
  shift
  : > "$logfile"
  if [ "$DRY_RUN" -eq 1 ]; then log "would run: $*"; return 0; fi
  local rc=0
  if "$@" >> "$logfile" 2>&1; then rc=0; else rc=$?; fi
  return "$rc"
}

SUDO=()
if [ "$(id -u)" -ne 0 ]; then SUDO=(sudo -n); fi
APT=("${SUDO[@]}" apt-get)

install_python() {
  pick_python && return 0
  . /etc/os-release
  if [ "${ID:-}" != "ubuntu" ]; then return 1; fi
  set_label "installing Python 3.12"
  refresh_sudo
  run_capture "$LOG" "${APT[@]}" install -y software-properties-common || return 1
  run_capture "$LOG" "${SUDO[@]}" add-apt-repository -y ppa:deadsnakes/ppa || return 1
  run_capture "$LOG" "${APT[@]}" update || return 1
  run_capture "$LOG" "${APT[@]}" install -y python3.12 || return 1
  PY=python3.12
  python_ok "$PY"
}

refresh_sudo() {
  if [ "${#SUDO[@]}" -gt 0 ]; then
    pause_anim
    if ! sudo -v; then
      resume_anim
      die "sudo is required for the packages"
    fi
    resume_anim
  fi
}

confirm_install() {
  local what=$1
  local answer=""
  if [ "$YES" -eq 1 ]; then return 0; fi
  if [ ! -t 0 ]; then
    die "no terminal to confirm the package install. Read --help, then re-run with --yes."
  fi
  pause_anim
  printf 'The installer will install:\n  %s\nContinue? [Y/n] ' "$what"
  if ! read -r answer; then
    resume_anim
    die "cancelled"
  fi
  resume_anim
  case "$answer" in
    ''|y|Y|yes|YES) return 0 ;;
    *) die "cancelled" ;;
  esac
}

install_dotnet() {
  if dotnet_sdk_ok; then return 0; fi
  if run_capture "$LOG" "${APT[@]}" install -y dotnet-sdk-8.0; then return 0; fi
  set_label "installing the .NET 8 SDK"
  curl -fsSL https://dot.net/v1/dotnet-install.sh -o "$RUN_DIR/dotnet-install.sh" \
    || die "could not download dotnet-install.sh"
  run_capture "$LOG" bash "$RUN_DIR/dotnet-install.sh" --channel 8.0 --install-dir "$HOME/.dotnet" \
    || die ".NET 8 SDK install failed"
  export DOTNET_ROOT="$HOME/.dotnet"
  export PATH="$DOTNET_ROOT:$PATH"
}

install_llvm_repo() {
  . /etc/os-release
  local codename="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
  [ -n "$codename" ] || return 1
  curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o "$RUN_DIR/llvm.key" || return 1
  "${SUDO[@]}" install -m 0644 "$RUN_DIR/llvm.key" /etc/apt/trusted.gpg.d/apt.llvm.org.asc || return 1
  printf 'deb https://apt.llvm.org/%s/ llvm-toolchain-%s-21 main\n' "$codename" "$codename" \
    | "${SUDO[@]}" tee /etc/apt/sources.list.d/llvm.list >/dev/null || return 1
  run_capture "$LOG" "${APT[@]}" update || return 1
  run_capture "$LOG" "${APT[@]}" install -y llvm-21 || return 1
}

ensure_llvm() {
  if llvm_ok; then
    if [ -z "${LLVM_MC:-}" ]; then
      local mc
      mc=$(find_llvm_mc) || true
      if [ -n "$mc" ]; then export LLVM_MC=$mc; fi
    fi
    return 0
  fi
  set_label "installing LLVM 21"
  refresh_sudo
  install_llvm_repo || die "LLVM 21 install failed. Install llvm-21 from apt.llvm.org by hand and run again."
  if [ -x /usr/lib/llvm-21/bin/llvm-mc ]; then
    export LLVM_MC=/usr/lib/llvm-21/bin/llvm-mc
  else
    die "LLVM 21 installed but llvm-mc was not found at /usr/lib/llvm-21/bin/llvm-mc."
  fi
}

install_cmake_repo() {
  . /etc/os-release
  [ "${ID:-}" = "ubuntu" ] || return 1
  local codename="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
  [ -n "$codename" ] || return 1
  curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc -o "$RUN_DIR/kitware.asc" || return 1
  gpg --dearmor < "$RUN_DIR/kitware.asc" \
    | "${SUDO[@]}" tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null || return 1
  printf 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ %s main\n' "$codename" \
    | "${SUDO[@]}" tee /etc/apt/sources.list.d/kitware.list >/dev/null || return 1
  run_capture "$LOG" "${APT[@]}" update || return 1
  run_capture "$LOG" "${APT[@]}" install -y cmake || return 1
  cmake_ok
}

verify_tools() {
  set_label "verifying tools"
  local missing
  missing=$(plan_missing | awk '!seen[$0]++ { if (n++) printf ", "; printf "%s", $0 }')
  if [ -n "$missing" ]; then
    die "still missing: $missing. Open a new terminal and run again, or install them by hand."
  fi
  if ! python_ok "$PY"; then
    die "Python 3.12 or newer is required (found $("$PY" -V 2>&1))."
  fi
  set_percent 1000
}

install_packages() {
  set_step 2 "installing packages"
  local missing
  missing=$(plan_missing | awk '!seen[$0]++ { if (n++) printf ", "; printf "%s", $0 }')
  if [ -z "$missing" ]; then
    log "all prerequisites are already installed"
    set_percent 1000
    return 0
  fi
  local pkgs=(git curl ca-certificates gnupg python3)
  if want_stage shaders || want_stage pipelines; then
    pkgs+=(spirv-tools libvulkan-dev vulkan-tools)
  fi
  if want_stage runtime || want_stage pipelines; then
    pkgs+=(cmake ninja-build build-essential)
  fi
  if [ "$WAV" -eq 1 ] && want_stage assets; then
    pkgs+=(ffmpeg)
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    log "would install: $missing"
    log "would run: ${APT[*]} update"
    log "would run: ${APT[*]} install -y ${pkgs[*]}"
    return 0
  fi
  log "missing: $missing"
  confirm_install "$missing"
  run_capture "$LOG" "${APT[@]}" update || die "apt-get update failed"
  run_capture "$LOG" "${APT[@]}" install -y "${pkgs[@]}" || die "apt-get install failed"
  set_percent 300
  install_python || die "Python 3.12 or newer is required. Install it by hand and run again."
  set_percent 400
  if want_stage assets; then install_dotnet; fi
  if want_stage shaders; then ensure_llvm; fi
  if want_stage runtime || want_stage pipelines; then
    if ! cmake_ok; then
      set_label "installing CMake 3.24 or newer"
      install_cmake_repo || die "CMake 3.24 or newer is required. Install it from cmake.org and run again."
    fi
  fi
  set_percent 1000
}

install_runtime_deps() {
  set_label "installing the runtime cross-build tools"
  if [ "$DRY_RUN" -eq 1 ]; then
    log "would run: ${APT[*]} install -y clang lld cargo"
    log "would run: cargo install xwin"
    log "would run: xwin --accept-license ..."
    return 0
  fi
  if [ "$YES" -ne 1 ]; then
    log "The runtime cross-build downloads the MSVC CRT and Windows SDK from Microsoft."
    log "That accepts Microsoft's licence. Re-run with --yes to accept."
    die "the runtime cross-build needs --yes"
  fi
  refresh_sudo
  run_capture "$LOG" "${APT[@]}" install -y clang lld cargo || die "could not install clang, lld and cargo"
  export PATH="$HOME/.cargo/bin:$PATH"
  if ! command -v xwin >/dev/null 2>&1; then
    run_capture "$LOG" cargo install xwin || die "cargo install xwin failed"
  fi
  if [ ! -d "$HOME/.local/share/xwin-sdk/VC" ]; then
    run_capture "$LOG" xwin --accept-license --sdk-version 10.0.26100 splat \
      --use-winsysroot-style --preserve-ms-arch-notation --output "$HOME/.local/share/xwin-sdk" \
      || die "xwin splat failed"
  fi
  set_percent 1000
}

clone_repo() {
  set_step 3 "cloning or updating"
  if [ -d "$DIR/.git" ]; then
    if [ -n "$(git -C "$DIR" status --porcelain 2>/dev/null || true)" ]; then
      log "local changes in $DIR; not updating"
    else
      run_capture "$LOG" git -C "$DIR" pull --ff-only || die "git pull failed; resolve it in the clone"
      run_capture "$LOG" git -C "$DIR" submodule update --init --recursive || die "git submodule update failed"
    fi
  else
    if [ -e "$DIR" ]; then die "$DIR exists but is not a git clone. Move it or pass --dir."; fi
    run_capture "$LOG" git clone --recursive "$REPO" "$DIR" || die "git clone failed"
  fi
  set_percent 1000
}

find_game() {
  set_step 4 "finding your game copy"
  if [ -n "$GAME" ]; then
    set_percent 1000
    return 0
  fi
  set_label "scanning Downloads, Desktop, Documents, Games, /mnt and /media"
  local found count
  found=$("$PY" -I "$DIR/make_game.py" --find 2>>"$LOG" | grep -v '^[0-9][0-9]* found$' | sed 's/  (.*)$//' | sed '/^$/d' || true)
  count=$(printf '%s\n' "$found" | grep -c . || true)
  if [ "$count" -eq 0 ]; then
    die "no supported copy found. Put version 1.0.2.1 on this PC, or pass --game /path/to/package."
  fi
  if [ "$count" -gt 1 ]; then
    log "more than one copy found:"
    printf '%s\n' "$found" | while IFS= read -r line; do log "  $line"; done
    die "choose one with --game /path/to/package"
  fi
  GAME=$found
  log "game: $GAME"
  set_percent 1000
}

setup_llvm() {
  set_step 5 "checking llvm-mc"
  if [ -n "${LLVM_MC:-}" ] && [ ! -x "$LLVM_MC" ]; then
    log "llvm-mc: using LLVM_MC from the environment"
    set_percent 1000
    return 0
  fi
  local mc
  mc=$(find_llvm_mc) || die "llvm-mc is missing. Install LLVM 21 (llvm-21), or set LLVM_MC."
  if ! llvm_ok; then
    die "llvm-mc at $mc has no AMDGPU target. Install LLVM 21, or set LLVM_MC."
  fi
  export LLVM_MC=$mc
  log "llvm-mc: $mc"
  set_percent 1000
}

build_port() {
  set_step 6 "building the port"
  local args=(-I "$DIR/make_game.py" --game "$GAME")
  if [ -n "$OUT" ]; then args+=(--out "$OUT"); fi
  if [ -n "$WORK" ]; then args+=(--work "$WORK"); fi
  if [ -n "$STAGES" ]; then args+=(--stages "$STAGES"); fi
  if [ -n "$JOBS" ]; then args+=(--jobs "$JOBS"); fi
  if [ "$WAV" -eq 1 ]; then args+=(--wav); fi
  if [ -n "$RUNTIME_BUILD" ]; then args+=(--runtime-build "$RUNTIME_BUILD"); fi
  if ! run_capture "$LOG" "$PY" "${args[@]}"; then
    cp "$LOG" "$DIR/install.log" 2>/dev/null || true
    die "the build failed"
  fi
  cp "$LOG" "$DIR/install.log" 2>/dev/null || true
  finish "build finished"
  local out_path
  if [ -n "$OUT" ]; then out_path=$OUT; else out_path="$HOME/Documents/Leonida"; fi
  printf 'port: %s\n' "$out_path"
  printf 'log:  %s\n' "$DIR/install.log"
}

if [ "$ANIM" -eq 1 ]; then
  animator &
  ANIM_PID=$!
fi

if [ "$SELFTEST" -eq 1 ]; then
  labels=("checking prerequisites" "installing packages" "cloning or updating"
          "finding your game copy" "checking llvm-mc" "building the port")
  for i in 1 2 3 4 5 6; do
    set_step "$i" "${labels[$((i - 1))]}"
    set_percent 450
    sleep 0.2
    set_percent 1000
    sleep 0.2
  done
  finish "self test finished"
  exit 0
fi

set_step 1 "checking prerequisites"
if [ "$SKIP_DEPS" -eq 1 ]; then
  set_step 2 "using the tools already installed"
  verify_tools
  set_percent 1000
else
  install_packages
  verify_tools
fi
if [ "$RUNTIME" -eq 1 ] && [ "$CHECK" -eq 0 ] && [ -z "$RUNTIME_BUILD" ]; then
  install_runtime_deps
fi

clone_repo
find_game
if [ "$CHECK" -eq 1 ]; then
  set_step 6 "checking your game copy"
  run_capture "$LOG" "$PY" -I "$DIR/make_game.py" --game "$GAME" --check \
    || die "the game copy did not pass the check"
  set_percent 1000
  cp "$LOG" "$DIR/install.log" 2>/dev/null || true
  finish "check finished"
  printf 'game: %s\n' "$GAME"
  exit 0
fi

setup_llvm
build_port

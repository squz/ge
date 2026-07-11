#!/usr/bin/env bash
#
# matrix-cell.sh — run exactly one matrix cell's sub-checks end-to-end.
#
# Each cell is a combination of platform, runtime, form-factor, and mode.
# Module.mk has one make rule per cell (e.g. cell.desktop-dist) that calls
# this script with the cell name.
#
# For cells that target a mobile device or simulator/emulator, the script
# automatically acquires a spyder reservation before doing any device work
# and releases it on exit. This makes concurrent cell execution safe: two
# cells targeting different devices run in parallel; two cells racing for
# the same device serialize via spyder's reservation queue.
#
# Usage:
#   ge/tools/matrix-cell.sh <cell-name> [options]
#
# Required:
#   <cell-name>       One of the 24 canonical cell names (see list below).
#
# Options:
#   --app <dir>           App root dir (default: current dir).
#   --timeout <s>         Cold-launch timeout override (default 8s).
#   --soak-timeout <s>    Soak phase duration override (default 10s; use 60 for
#                         the long-soak cell).  Debug cells always use 10s
#                         regardless of this value.
#   --capture-refs        Record reference screenshots instead of comparing.
#   --rms <f>             Image-diff RMS threshold (default 0.08).
#   --verbose             Echo sub-command output.
#   --device <alias>      Pin a specific device alias (skip spyder reserve --on).
#                         Normally passed internally by the self-wrap; callers can
#                         also use it to target a known inventory alias directly.
#   --skip-spyder-run     Internal flag. Means this invocation is already running
#                         inside a `spyder run` wrapper — skip the self-wrap.
#
# Canonical cell names:
#   desktop-dist  desktop-player
#   ios-sim-phone-dist  ios-sim-phone-player
#   ios-sim-tablet-dist  ios-sim-tablet-player
#   ios-device-phone-dist  ios-device-phone-player
#   ios-device-tablet-dist  ios-device-tablet-player
#   android-emu-phone-dist  android-emu-phone-player
#   android-emu-tablet-dist  android-emu-tablet-player
#   android-device-phone-dist  android-device-phone-player
#   android-device-tablet-dist  android-device-tablet-player
#   desktop-debug-dist  desktop-debug-player
#   ios-debug-dist  ios-debug-player
#   android-debug-dist  android-debug-player
#
# Parallelism story (🎯T33.2):
#   Cells that need a device are wrapped in `spyder run --device <alias>`.
#   Two cells targeting different devices run concurrently. Two cells
#   targeting the same device serialize — the second one retries up to 3×
#   (10 s backoff) before failing (exit code 13 surfaced as a setup error).
#   Desktop cells never need a reservation and always run concurrently.
#
# Environment variables:
#   GE_IOS_SIM_PHONE_DEVICE   Inventory alias or UDID for the phone iOS sim.
#   GE_IOS_SIM_TABLET_DEVICE  Inventory alias or UDID for the tablet iOS sim.
#   GE_IOS_PHONE_DEVICE       Inventory alias or UDID for physical iOS phone.
#   GE_IOS_TABLET_DEVICE      Inventory alias or UDID for physical iOS tablet.
#   GE_ANDROID_EMU_PHONE_DEVICE  Inventory alias for Android phone emulator.
#   GE_ANDROID_EMU_TABLET_DEVICE Inventory alias for Android tablet emulator.
#   GE_ANDROID_PHONE_DEVICE   Inventory alias for USB-connected Android phone.
#   GE_ANDROID_TABLET_DEVICE  Inventory alias for USB-connected Android tablet.
#
#   When set, these bypass spyder reserve --on selector dispatch and use
#   the given alias directly for that cell's device. Useful for CI where
#   device layout is fixed and you want deterministic pinning.
#
# Exit codes:
#   0 — cell passed all applicable sub-checks
#   1 — one or more sub-checks failed
#   2 — setup error (cell name invalid, APP_ID auto-detect failed, prereq missing)
#   13 — reservation conflict: another cell holds this device (all retries exhausted)
#   50 — cell sub-checks passed but post-run device_power_state was display_off or
#        asleep — the soak result is unreliable (device may have been sleeping most
#        of the run). Distinct from 1 so CI can flag "device fell asleep" separately
#        from "cell logic failed". spyder exit codes 10–42 are reserved; 50 is safe.

set -euo pipefail

# ── Locate self / ge ────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Visual regression helpers ────────────────────────────────────────
# Source visual-regression.sh so visual_regression_check / visual_regression_update
# are available to cell runners. The file is optional (graceful no-op if absent).
# shellcheck source=tools/visual-regression.sh
if [[ -f "$SCRIPT_DIR/visual-regression.sh" ]]; then
    # shellcheck disable=SC1090
    source "$SCRIPT_DIR/visual-regression.sh"
fi

# Resolve VR_ARTIFACTS_DIR to the cell's artifact directory once it is set up.
# The actual assignment happens after ARTIFACTS is defined (see "Artifacts dir" block).

# ── Parse args ──────────────────────────────────────────────────────

CELL=""
APP_DIR="$(pwd)"
COLD_LAUNCH_TIMEOUT=8
SOAK_TIMEOUT=10
DEBUG_SOAK_TIMEOUT=10
CAPTURE_REFS=0
RMS=0.08
VERBOSE=0
STREAM_PORT=3030
PLAYER_BUNDLE_ID="com.squz.player"
PLAYER_ANDROID_PKG="com.squz.player"
PLAYER_ANDROID_ACTIVITY="com.squz.player/.GeActivity"
# Pinned device alias (set by self-wrap; can also be passed by caller).
PINNED_DEVICE=""
# Skip the spyder-run self-wrap (we're already inside one).
SKIP_SPYDER_RUN=0

if [[ $# -eq 0 ]]; then
    sed -n '2,/^$/{ s/^# \{0,1\}//; p; }' "$0"
    exit 2
fi

CELL="$1"; shift

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)             APP_DIR="$(cd "$2" && pwd)"; shift 2 ;;
        --timeout)         COLD_LAUNCH_TIMEOUT="$2"; shift 2 ;;
        --soak-timeout)    SOAK_TIMEOUT="$2"; shift 2 ;;
        --capture-refs)    CAPTURE_REFS=1; shift ;;
        --rms)             RMS="$2"; shift 2 ;;
        --verbose)         VERBOSE=1; shift ;;
        --device)          PINNED_DEVICE="$2"; shift 2 ;;
        --skip-spyder-run) SKIP_SPYDER_RUN=1; shift ;;
        -h|--help)
            sed -n '2,/^$/{ s/^# \{0,1\}//; p; }' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ── Validate cell name ───────────────────────────────────────────────

ALL_CELLS=(
    desktop-dist desktop-player
    ios-sim-phone-dist ios-sim-phone-player
    ios-sim-tablet-dist ios-sim-tablet-player
    ios-device-phone-dist ios-device-phone-player
    ios-device-tablet-dist ios-device-tablet-player
    android-emu-phone-dist android-emu-phone-player
    android-emu-tablet-dist android-emu-tablet-player
    android-device-phone-dist android-device-phone-player
    android-device-tablet-dist android-device-tablet-player
    desktop-debug-dist desktop-debug-player
    ios-debug-dist ios-debug-player
    android-debug-dist android-debug-player
)
valid_cell=0
for c in "${ALL_CELLS[@]}"; do [[ "$c" == "$CELL" ]] && { valid_cell=1; break; }; done
if [[ $valid_cell -eq 0 ]]; then
    echo "Unknown cell: $CELL" >&2
    echo "Valid cells: ${ALL_CELLS[*]}" >&2
    exit 2
fi

# ── Decompose cell name ──────────────────────────────────────────────
# Fields: PLATFORM, RUNTIME, FORM_FACTOR, MODE
# desktop-dist          → platform=desktop runtime=- ff=- mode=dist
# desktop-player        → platform=desktop runtime=- ff=- mode=player
# ios-sim-phone-dist    → platform=ios runtime=sim ff=phone mode=dist
# android-debug-player  → platform=android runtime=debug ff=- mode=player
# desktop-debug-dist    → platform=desktop runtime=debug ff=- mode=dist

PLATFORM=""
RUNTIME=""
FORM_FACTOR=""
MODE=""

case "$CELL" in
    desktop-dist)         PLATFORM=desktop;  RUNTIME=release; FORM_FACTOR=none; MODE=dist   ;;
    desktop-player)       PLATFORM=desktop;  RUNTIME=release; FORM_FACTOR=none; MODE=player ;;
    desktop-debug-dist)   PLATFORM=desktop;  RUNTIME=debug;   FORM_FACTOR=none; MODE=dist   ;;
    desktop-debug-player) PLATFORM=desktop;  RUNTIME=debug;   FORM_FACTOR=none; MODE=player ;;
    ios-sim-phone-dist)   PLATFORM=ios;      RUNTIME=sim;     FORM_FACTOR=phone; MODE=dist   ;;
    ios-sim-phone-player) PLATFORM=ios;      RUNTIME=sim;     FORM_FACTOR=phone; MODE=player ;;
    ios-sim-tablet-dist)  PLATFORM=ios;      RUNTIME=sim;     FORM_FACTOR=tablet; MODE=dist  ;;
    ios-sim-tablet-player)PLATFORM=ios;      RUNTIME=sim;     FORM_FACTOR=tablet; MODE=player;;
    ios-device-phone-dist)   PLATFORM=ios;   RUNTIME=device;  FORM_FACTOR=phone; MODE=dist   ;;
    ios-device-phone-player) PLATFORM=ios;   RUNTIME=device;  FORM_FACTOR=phone; MODE=player ;;
    ios-device-tablet-dist)  PLATFORM=ios;   RUNTIME=device;  FORM_FACTOR=tablet; MODE=dist  ;;
    ios-device-tablet-player)PLATFORM=ios;   RUNTIME=device;  FORM_FACTOR=tablet; MODE=player;;
    ios-debug-dist)       PLATFORM=ios;      RUNTIME=debug;   FORM_FACTOR=none; MODE=dist   ;;
    ios-debug-player)     PLATFORM=ios;      RUNTIME=debug;   FORM_FACTOR=none; MODE=player ;;
    android-emu-phone-dist)   PLATFORM=android; RUNTIME=emu;  FORM_FACTOR=phone; MODE=dist   ;;
    android-emu-phone-player) PLATFORM=android; RUNTIME=emu;  FORM_FACTOR=phone; MODE=player ;;
    android-emu-tablet-dist)  PLATFORM=android; RUNTIME=emu;  FORM_FACTOR=tablet; MODE=dist  ;;
    android-emu-tablet-player)PLATFORM=android; RUNTIME=emu;  FORM_FACTOR=tablet; MODE=player;;
    android-device-phone-dist)   PLATFORM=android; RUNTIME=device; FORM_FACTOR=phone; MODE=dist   ;;
    android-device-phone-player) PLATFORM=android; RUNTIME=device; FORM_FACTOR=phone; MODE=player ;;
    android-device-tablet-dist)  PLATFORM=android; RUNTIME=device; FORM_FACTOR=tablet; MODE=dist  ;;
    android-device-tablet-player)PLATFORM=android; RUNTIME=device; FORM_FACTOR=tablet; MODE=player;;
    android-debug-dist)   PLATFORM=android;  RUNTIME=debug;   FORM_FACTOR=none; MODE=dist   ;;
    android-debug-player) PLATFORM=android;  RUNTIME=debug;   FORM_FACTOR=none; MODE=player ;;
esac

IS_DEBUG=0; [[ "$RUNTIME" == "debug" ]] && IS_DEBUG=1
IS_SIM=0;   [[ "$RUNTIME" == "sim" ]]   && IS_SIM=1
IS_EMU=0;   [[ "$RUNTIME" == "emu" ]]   && IS_EMU=1
IS_DEVICE=0;[[ "$RUNTIME" == "device" ]] && IS_DEVICE=1
IS_MOBILE=0; [[ "$PLATFORM" == "ios" || "$PLATFORM" == "android" ]] && IS_MOBILE=1
IS_PLAYER=0; [[ "$MODE" == "player" ]] && IS_PLAYER=1

SOAK_TIME=$SOAK_TIMEOUT
[[ $IS_DEBUG -eq 1 ]] && SOAK_TIME=$DEBUG_SOAK_TIMEOUT

# ── Spyder: cell device selector ────────────────────────────────────
#
# Each mobile/sim/emu cell maps to a spyder selector (for reserve --on)
# and an optional env-var that lets callers pin a specific alias.
#
# Desktop cells never need a device reservation. They set CELL_SELECTOR=""
# to indicate "no reservation required".
#
# Returns:
#   CELL_SELECTOR  — spyder reserve --on predicate (empty for desktop)
#   CELL_PIN_VAR   — name of the env var that pins a specific alias

CELL_SELECTOR=""
CELL_PIN_VAR=""

case "$CELL" in
    ios-sim-phone-dist|ios-sim-phone-player|ios-debug-dist|ios-debug-player)
        CELL_SELECTOR="platform=ios-sim,model=iphone"
        CELL_PIN_VAR="GE_IOS_SIM_PHONE_DEVICE"
        ;;
    ios-sim-tablet-dist|ios-sim-tablet-player)
        CELL_SELECTOR="platform=ios-sim,model=ipad"
        CELL_PIN_VAR="GE_IOS_SIM_TABLET_DEVICE"
        ;;
    ios-device-phone-dist|ios-device-phone-player)
        CELL_SELECTOR="platform=ios,model=iphone"
        CELL_PIN_VAR="GE_IOS_PHONE_DEVICE"
        ;;
    ios-device-tablet-dist|ios-device-tablet-player)
        CELL_SELECTOR="platform=ios,model=ipad"
        CELL_PIN_VAR="GE_IOS_TABLET_DEVICE"
        ;;
    android-emu-phone-dist|android-emu-phone-player)
        CELL_SELECTOR="platform=android-emu,model=phone"
        CELL_PIN_VAR="GE_ANDROID_EMU_PHONE_DEVICE"
        ;;
    android-emu-tablet-dist|android-emu-tablet-player|android-debug-dist|android-debug-player)
        CELL_SELECTOR="platform=android-emu,model=tablet"
        CELL_PIN_VAR="GE_ANDROID_EMU_TABLET_DEVICE"
        ;;
    android-device-phone-dist|android-device-phone-player)
        CELL_SELECTOR="platform=android,model=phone"
        CELL_PIN_VAR="GE_ANDROID_PHONE_DEVICE"
        ;;
    android-device-tablet-dist|android-device-tablet-player)
        CELL_SELECTOR="platform=android,model=tablet"
        CELL_PIN_VAR="GE_ANDROID_TABLET_DEVICE"
        ;;
    desktop-*)
        # Desktop cells never need a device reservation.
        CELL_SELECTOR=""
        CELL_PIN_VAR=""
        ;;
esac

# ── Spyder self-wrap ─────────────────────────────────────────────────
#
# For cells that need a device: if we're not already inside a `spyder run`
# wrapper (SKIP_SPYDER_RUN=0), resolve a device and re-exec under
# `spyder run --device <alias> --as cell-<cell-id>`.
#
# This makes parallel `make cell.*` invocations race-safe: two cells
# targeting different devices run concurrently; two cells racing for the
# same device get exit code 13 from `spyder reserve` and retry.
#
# Re-exec passes --skip-spyder-run so the child knows the reservation is
# already held, and --device <alias> so the resolved alias is available to
# device helpers that previously read GE_IOS_SIM_UDID / GE_ANDROID_EMU_SERIAL.

# retry_spyder_run <alias> <owner>
# Wraps `spyder run --device <alias> --as <owner> -- "$0" [forwarded args]`
# with up to 3 retries on exit code 13 (reservation-conflict), 10 s backoff.
# Exits with the child's exit code on success, or 13 after all retries fail.
retry_spyder_run() {
    local alias="$1"; local owner="$2"; shift 2
    local attempt max_attempts=3 backoff=10
    for attempt in $(seq 1 $max_attempts); do
        spyder run --device "$alias" --as "$owner" -- "$@"
        local rc=$?
        if [[ $rc -eq 13 ]]; then
            if [[ $attempt -lt $max_attempts ]]; then
                echo "[$CELL] reservation conflict for '$alias' (attempt $attempt/$max_attempts); retrying in ${backoff}s …" >&2
                sleep $backoff
            else
                echo "[$CELL] reservation conflict for '$alias': all $max_attempts attempts failed" >&2
                exit 13
            fi
        else
            exit $rc
        fi
    done
}

# resolve_and_wrap: pick a device via `spyder reserve --on <selector>` and
# re-exec under `spyder run`. We acquire-then-release to learn the alias
# spyder selected, then hand that alias to `spyder run` which re-acquires.
# The release→re-acquire window is tiny (~1 ms); in practice parallel cells
# targeting different selectors have no overlap, so this is safe.
resolve_and_wrap() {
    local selector="$1"; local owner="$2"
    # Attempt reserve --on to resolve the alias.
    local reserve_out reserve_rc
    reserve_out=$(spyder reserve --on "$selector" --as "$owner" 2>&1)
    reserve_rc=$?
    case $reserve_rc in
        0)  ;;
        10) echo "[$CELL] spyder daemon unreachable — is 'spyder serve' running?" >&2; exit 2 ;;
        11) echo "[$CELL] no device matched selector '$selector' — check inventory" >&2; exit 2 ;;
        12) echo "[$CELL] device matched selector '$selector' but is not connected" >&2; exit 2 ;;
        13) exit 13 ;;  # conflict; let caller retry
        30) echo "[$CELL] spyder reserve timed out waiting for '$selector'" >&2; exit 2 ;;
        40) echo "[$CELL] iOS trust not granted — accept the trust dialog on the device" >&2; exit 2 ;;
        41) echo "[$CELL] iOS Developer Mode is disabled — enable it in Settings" >&2; exit 2 ;;
        42) echo "[$CELL] device is locked — unlock it before running the matrix" >&2; exit 2 ;;
        *)  echo "[$CELL] spyder reserve failed (exit $reserve_rc): $reserve_out" >&2; exit 2 ;;
    esac
    # Extract the resolved device alias from the JSON output.
    local alias
    alias=$(printf '%s' "$reserve_out" | python3 -c "import sys,json; print(json.load(sys.stdin)['device'])" 2>/dev/null || true)
    if [[ -z "$alias" ]]; then
        echo "[$CELL] could not parse device alias from spyder reserve output" >&2
        echo "$reserve_out" >&2
        exit 2
    fi
    # Release immediately so spyder run can re-acquire.
    spyder release "$alias" --as "$owner" >/dev/null 2>&1 || true
    echo "[$CELL] resolved device '$alias' via selector '$selector'" >&2
    # Re-exec under spyder run with the resolved alias.
    retry_spyder_run "$alias" "$owner" "$0" "$CELL" \
        --app "$APP_DIR" \
        --timeout "$COLD_LAUNCH_TIMEOUT" \
        --soak-timeout "$SOAK_TIMEOUT" \
        --rms "$RMS" \
        --device "$alias" \
        --skip-spyder-run \
        $([ "$CAPTURE_REFS" -eq 1 ] && printf '%s' --capture-refs) \
        $([ "$VERBOSE" -eq 1 ] && printf '%s' --verbose)
}

# Determine if this invocation needs the spyder-run wrapper.
NEEDS_DEVICE=0
[[ -n "$CELL_SELECTOR" ]] && NEEDS_DEVICE=1

if [[ $NEEDS_DEVICE -eq 1 && $SKIP_SPYDER_RUN -eq 0 ]]; then
    owner="cell-$CELL"
    # Check if a device alias is pinned (caller-supplied or env var).
    resolved_alias="${PINNED_DEVICE:-}"
    if [[ -z "$resolved_alias" && -n "$CELL_PIN_VAR" ]]; then
        resolved_alias="${!CELL_PIN_VAR:-}"
    fi
    if [[ -n "$resolved_alias" ]]; then
        # Pinned alias: go straight to spyder run (no reserve --on needed).
        echo "[$CELL] using pinned device '$resolved_alias'" >&2
        retry_spyder_run "$resolved_alias" "$owner" "$0" "$CELL" \
            --app "$APP_DIR" \
            --timeout "$COLD_LAUNCH_TIMEOUT" \
            --soak-timeout "$SOAK_TIMEOUT" \
            --rms "$RMS" \
            --device "$resolved_alias" \
            --skip-spyder-run \
            $([ "$CAPTURE_REFS" -eq 1 ] && printf '%s' --capture-refs) \
            $([ "$VERBOSE" -eq 1 ] && printf '%s' --verbose)
        # retry_spyder_run calls exit internally; unreachable.
    else
        # No pin: use selector dispatch to resolve a device, then wrap.
        resolve_and_wrap "$CELL_SELECTOR" "$owner"
        # resolve_and_wrap calls exit internally; unreachable.
    fi
fi

# ── From here on we either own a spyder reservation (mobile/sim/emu cells)
# ── or are a desktop cell that needs no reservation.
# ── PINNED_DEVICE is the resolved alias available to device helpers below.

# ── Auto-detect app vars ─────────────────────────────────────────────

APP_NAME=""
APP_ID=""
APP_DISPLAY_NAME=""

if [[ -f "$APP_DIR/Makefile" ]]; then
    APP_NAME=$(sed -nE 's/^[[:space:]]*APP_NAME[[:space:]]*:?=[[:space:]]*([^[:space:]#]+).*/\1/p' \
        "$APP_DIR/Makefile" | head -1)
    APP_ID=$(sed -nE 's/^[[:space:]]*APP_ID[[:space:]]*:?=[[:space:]]*([^[:space:]#]+).*/\1/p' \
        "$APP_DIR/Makefile" | head -1)
    APP_DISPLAY_NAME=$(sed -nE 's/^[[:space:]]*APP_DISPLAY_NAME[[:space:]]*:?=[[:space:]]*([^[:space:]#]+).*/\1/p' \
        "$APP_DIR/Makefile" | head -1)
fi

[[ -z "$APP_NAME" ]] && APP_NAME="$(basename "$APP_DIR")"
[[ -z "$APP_DISPLAY_NAME" ]] && APP_DISPLAY_NAME="$APP_NAME"
if [[ -z "$APP_ID" && ("$PLATFORM" == "ios" || "$PLATFORM" == "android") ]]; then
    echo "FAIL [$CELL]: APP_ID not found in Makefile and is required for platform=$PLATFORM" >&2
    exit 2
fi

# ── Artifacts dir ───────────────────────────────────────────────────

ARTIFACTS_ROOT="${TMPDIR:-/tmp}/ge-matrix-$$"
ARTIFACTS="$ARTIFACTS_ROOT/$CELL"
mkdir -p "$ARTIFACTS"
REFS_DIR="$APP_DIR/test/refs"
mkdir -p "$REFS_DIR"

# Point visual-regression helpers at the same artifact directory so screenshots
# and diff reports land alongside the rest of the cell's artifacts.
VR_ARTIFACTS_DIR="$ARTIFACTS"

echo "matrix-cell: $CELL  app=$APP_NAME  dir=$APP_DIR"
[[ -n "$PINNED_DEVICE" ]] && echo "device:      $PINNED_DEVICE"
echo "artifacts:   $ARTIFACTS"
echo

# ── Core utilities ───────────────────────────────────────────────────

SUBCHECKS_PASSED=()
SUBCHECKS_FAILED=()

pass_check() {
    local name="$1"; local notes="${2:-}"
    SUBCHECKS_PASSED+=("$name")
    printf "  ✓ %-22s PASS%s\n" "$name" "${notes:+  ($notes)}"
}

fail_check() {
    local name="$1"; local msg="${2:-}"
    SUBCHECKS_FAILED+=("$name")
    printf "  ✗ %-22s FAIL%s\n" "$name" "${msg:+  — $msg}"
}

# warn_check: passes (not counted as a failure) but prints a warning.
# Use when the check itself couldn't run due to infra issues (capture failure,
# missing tool data, etc.) — i.e. absence of evidence, not evidence of absence.
warn_check() {
    local name="$1"; local msg="${2:-}"
    SUBCHECKS_PASSED+=("$name")
    printf "  ~ %-22s WARN%s\n" "$name" "${msg:+  — $msg}"
}

# ── Post-run device power state check (🎯T33.4) ─────────────────────
# check_device_awake <spyder-device-id>
#
# Calls `spyder device_power_state <id>` (35s timeout — spyder's recommended
# budget for the DVT-screenshot probe). Interprets the JSON {"state": "..."}.
#
#   awake       → return 0 (pass — device was on during the soak)
#   unknown     → warn and return 0 (tunneld unavailable, dev mode off, etc.;
#                 treat as no-signal; don't flip a passing cell to fail)
#   display_off → print diagnostic and exit 50
#   asleep      → print diagnostic and exit 50
#
# Exit 50 is outside spyder's reserved 10–42 range. It means "cell logic passed
# but the device fell asleep, so the soak result is unreliable."
#
# If spyder is not installed or the call itself times out, warn and return 0
# (absence of evidence is not evidence of absence).
#
# Only call this after a soak sub-check has passed; skip when the cell already
# failed (caller should guard with: if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]).
DEVICE_ASLEEP_EXIT=50

check_device_awake() {
    local device_id="$1"
    local subcheck="device-power-state"

    if ! command -v spyder >/dev/null 2>&1; then
        warn_check "$subcheck" "spyder not installed — skipping power-state probe"
        return 0
    fi

    local raw
    if ! raw=$(spyder device_power_state "$device_id" --timeout 35 2>/dev/null); then
        warn_check "$subcheck" "spyder device_power_state call failed — skipping"
        return 0
    fi

    local state
    state=$(python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('state','unknown'))" \
        <<< "$raw" 2>/dev/null || echo "unknown")

    case "$state" in
        awake)
            pass_check "$subcheck" "device awake post-soak"
            return 0
            ;;
        unknown)
            warn_check "$subcheck" "state=unknown (tunneld unavailable or dev mode off) — no signal"
            return 0
            ;;
        display_off|asleep)
            printf "  ✗ %-22s FAIL  — device state=%s post-soak; soak result is unreliable\n" \
                "$subcheck" "$state"
            SUBCHECKS_FAILED+=("$subcheck")
            return "$DEVICE_ASLEEP_EXIT"
            ;;
        *)
            warn_check "$subcheck" "unrecognized state='$state' — treating as no signal"
            return 0
            ;;
    esac
}

# Run a command, respecting VERBOSE. Returns exit code.
run() {
    if [[ $VERBOSE -eq 1 ]]; then
        echo "+ $*" >&2
        "$@"
    else
        "$@" >/dev/null 2>&1
    fi
}

# Run and capture stdout; stderr to /dev/null unless verbose.
run_out() {
    if [[ $VERBOSE -eq 1 ]]; then
        echo "+ $*" >&2
        "$@"
    else
        "$@" 2>/dev/null
    fi
}

require_cmd() {
    local cmd="$1"; local msg="${2:-$cmd not found}"
    command -v "$cmd" >/dev/null 2>&1 || { echo "FAIL [$CELL]: $msg" >&2; exit 2; }
}

# ── stream relay lifecycle ────────────────────────────────────────────────────

# Stream relay is spyder (external). Cells never start a local broker.
ensure_stream_relay() {
    if curl -sf --max-time 2 "http://127.0.0.1:$STREAM_PORT/stream/servers" >/dev/null 2>&1; then
        return 0
    fi
    echo "spyder stream relay not reachable at :$STREAM_PORT — start with: brew services start spyder" >&2
    exit 2
}
ensure_ged() { ensure_stream_relay; }

stop_ged() { :; }  # spyder is external — do not kill

# Total player sessions across all registered stream servers (spyder JSON).
current_sessions() {
    curl -sf --max-time 1 "http://127.0.0.1:$STREAM_PORT/stream/servers" 2>/dev/null \
        | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(sum(s.get('sessions', 0) for s in d.get('servers', [])))
" 2>/dev/null || echo 0
}

# Alias — same metric under spyder's /stream/servers shape.
server_attached_sessions() {
    current_sessions
}

wait_for_sessions() {
    local min="$1"
    local deadline=$(($(date +%s) + COLD_LAUNCH_TIMEOUT))
    while [[ $(date +%s) -lt $deadline ]]; do
        local sess
        sess=$(current_sessions)
        if [[ "$sess" -ge "$min" ]]; then return 0; fi
        sleep 0.5
    done
    return 1
}

# Waits until at least $1 sessions are attached to a server (bridged).
# Used for the reconnect sub-check: during a server restart the player stays
# connected to stream relay (session count doesn't change) but sessions temporarily
# lose their server assignment, so current_sessions()/wait_for_sessions()
# would never fire. This polls server_attached_sessions() instead.
wait_for_server_sessions() {
    local min="$1"
    local deadline=$(($(date +%s) + COLD_LAUNCH_TIMEOUT))
    while [[ $(date +%s) -lt $deadline ]]; do
        local sess
        sess=$(server_attached_sessions)
        if [[ "$sess" -ge "$min" ]]; then return 0; fi
        sleep 0.5
    done
    return 1
}

# ── Process helpers ──────────────────────────────────────────────────

kill_bg() {
    local pid="${1:-}"
    [[ -n "$pid" ]] || return 0
    kill "$pid" 2>/dev/null || true
    local tries=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.2
        tries=$((tries + 1))
        [[ $tries -gt 15 ]] && { kill -9 "$pid" 2>/dev/null || true; break; }
    done
    wait "$pid" 2>/dev/null || true
}

# Screenshot the largest on-screen window owned by $pid.
shot_window_by_pid() {
    local pid="$1"; local out="$2"
    local wid
    wid=$(python3 - "$pid" <<'PY'
import sys, Quartz
pid = int(sys.argv[1])
wins = Quartz.CGWindowListCopyWindowInfo(
    Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID)
best = None; best_area = 0
for w in wins:
    if w.get(Quartz.kCGWindowOwnerPID, -1) != pid: continue
    if w.get(Quartz.kCGWindowLayer, 99) != 0: continue
    b = w.get(Quartz.kCGWindowBounds, None)
    if not b: continue
    area = b.get("Width", 0) * b.get("Height", 0)
    if area > best_area:
        best_area = area
        best = w.get(Quartz.kCGWindowNumber, None)
if best is not None:
    print(best)
PY
) 2>/dev/null || true
    if [[ -n "$wid" ]]; then
        screencapture -x -o -l "$wid" "$out" 2>/dev/null
    fi
    # Fall back to full-screen if window-specific capture failed or no window found.
    if [[ ! -s "$out" ]]; then
        screencapture -x "$out" 2>/dev/null
    fi
}

# ── Image diff ───────────────────────────────────────────────────────

imgdiff_compare() {
    local ref="$1"; local shot="$2"
    "$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null
}

check_or_capture_ref() {
    local subcheck="$1"; local shot="$2"; local ref_name="$3"
    local ref="$REFS_DIR/$ref_name"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        pass_check "$subcheck" "ref captured"
        return 0
    fi
    if [[ ! -f "$ref" ]]; then
        # No ref is a non-fatal informational result, not a test failure.
        pass_check "$subcheck" "no ref (run --capture-refs to establish baseline)"
        return 0
    fi
    local rms
    if rms=$(imgdiff_compare "$ref" "$shot"); then
        pass_check "$subcheck" "rms=$rms"
    else
        fail_check "$subcheck" "image differs from ref (rms=$rms > $RMS)"
        return 1
    fi
}

# ── iOS simulator helpers ────────────────────────────────────────────
#
# When PINNED_DEVICE is set (i.e. we're inside a spyder run wrapper), we
# ask spyder to resolve it to a UDID rather than calling xcrun simctl
# directly. This keeps device discovery going through spyder and avoids
# races with concurrent cells.

# Returns the UDID of the booted iOS sim for this cell.
# Uses PINNED_DEVICE alias via spyder resolve when available;
# falls back to xcrun simctl discovery when not.
ios_sim_get_udid() {
    local ff="${1:-}"

    # If spyder resolved a device alias for us, get the UDID from inventory.
    if [[ -n "$PINNED_DEVICE" ]]; then
        local resolved
        resolved=$(spyder resolve "$PINNED_DEVICE" --json 2>/dev/null \
            | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('ios_uuid',''))" \
            2>/dev/null || true)
        if [[ -n "$resolved" ]]; then
            echo "$resolved"
            return 0
        fi
        # Alias doesn't have ios_uuid — treat PINNED_DEVICE as a raw UDID.
        echo "$PINNED_DEVICE"
        return 0
    fi

    local filter=""
    case "$ff" in
        phone)  filter='test("iPhone"; "i")' ;;
        tablet) filter='test("iPad"; "i")' ;;
        *)      filter='true' ;;
    esac

    # Try booted first.
    local udid
    udid=$(xcrun simctl list devices booted -j 2>/dev/null \
        | python3 -c "
import sys, json, re
d = json.load(sys.stdin)
devs = []
for runtime, sims in d.get('devices', {}).items():
    for s in sims:
        if s.get('state','') == 'Booted':
            devs.append(s)
ff_filter = '$ff'
if ff_filter in ('phone', 'tablet'):
    pat = 'iPhone' if ff_filter == 'phone' else 'iPad'
    devs = [s for s in devs if pat in s.get('name','')]
if devs:
    print(devs[0]['udid'])
" 2>/dev/null || true)

    if [[ -n "$udid" ]]; then
        echo "$udid"
        return 0
    fi

    # None booted — boot a canonical sim.
    local canonical
    case "$ff" in
        phone)  canonical="iPhone 16" ;;
        tablet) canonical="iPad Air 11-inch (M4)" ;;
        *)      canonical="iPad Air 11-inch (M4)" ;;
    esac

    local target_udid
    target_udid=$(xcrun simctl list devices available -j 2>/dev/null \
        | python3 -c "
import sys, json
d = json.load(sys.stdin)
name = '$canonical'
for runtime, sims in d.get('devices', {}).items():
    for s in sims:
        if s.get('name','') == name and s.get('isAvailable', False):
            print(s['udid'])
            sys.exit(0)
" 2>/dev/null || true)

    if [[ -z "$target_udid" ]]; then
        echo "No '$canonical' simulator available; install via Xcode → Platforms." >&2
        return 1
    fi

    echo "Booting simulator: $canonical ($target_udid)" >&2
    xcrun simctl boot "$target_udid" 2>/dev/null || true
    open -a Simulator >/dev/null 2>&1 || true
    sleep 6
    echo "$target_udid"
}

ios_sim_is_running() {
    local udid="$1"; local bundle_id="$2"
    # iOS briefly drops the app from launchctl during orientation-
    # transition suspension windows and occasionally during other
    # transitions. Retry up to 5 times over ~2s to tolerate these
    # gaps. Use -F so special characters in bundle_id aren't
    # regex-interpreted.
    local attempt
    for attempt in 1 2 3 4 5; do
        if xcrun simctl spawn "$udid" launchctl list 2>/dev/null \
            | grep -qF "$bundle_id"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

ios_sim_crash_count() {
    local bundle_id="$1"
    ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null \
        | grep -c "$bundle_id" || true
}

# ── iOS physical device helpers ──────────────────────────────────────

# Returns the UDID of a connected iOS physical device for this cell.
# Uses PINNED_DEVICE alias via spyder resolve when available.
IOS_DEVICE_UDID=""
ios_device_get_udid() {
    local ff="${1:-}"

    if [[ -n "$PINNED_DEVICE" ]]; then
        local resolved
        resolved=$(spyder resolve "$PINNED_DEVICE" --json 2>/dev/null \
            | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('ios_coredevice','') or d.get('ios_uuid',''))" \
            2>/dev/null || true)
        if [[ -n "$resolved" ]]; then
            IOS_DEVICE_UDID="$resolved"
            echo "$resolved"
            return 0
        fi
        IOS_DEVICE_UDID="$PINNED_DEVICE"
        echo "$PINNED_DEVICE"
        return 0
    fi

    # Fallback: discover via devicectl (no spyder pin).
    local pref_device=""
    case "$ff" in
        tablet) pref_device="${GE_IOS_TABLET_DEVICE:-}" ;;
        phone)  pref_device="${GE_IOS_PHONE_DEVICE:-}" ;;
    esac

    local tmpfile
    tmpfile=$(mktemp)
    if ! xcrun devicectl list devices -j "$tmpfile" >/dev/null 2>&1; then
        rm -f "$tmpfile"
        echo "devicectl unavailable — is Xcode installed?" >&2
        return 1
    fi

    local udid
    udid=$(python3 - "$tmpfile" "$ff" "$pref_device" <<'PY'
import sys, json
datafile = sys.argv[1]
ff = sys.argv[2] if len(sys.argv) > 2 else ""
pref = sys.argv[3] if len(sys.argv) > 3 else ""
with open(datafile) as f:
    data = json.load(f)
devices = data.get("result", {}).get("devices", [])
connected = [d for d in devices if d.get("connectionProperties", {}).get("transportType") == "localNetwork"
             or d.get("connectionProperties", {}).get("tunnelState") == "connected"
             or "connected" in str(d.get("connectionProperties", {}))]
if not connected:
    connected = [d for d in devices if d.get("deviceProperties", {}).get("developerModeStatus") == "enabled"
                 or "connected" in str(d.get("connectionProperties", {}).get("pairingState", ""))]
if not ff:
    if connected:
        print(connected[0]["identifier"])
    sys.exit(0)
form_matches = []
for d in connected:
    model = d.get("hardwareProperties", {}).get("deviceType", "")
    if ff == "tablet" and "iPad" in model:
        form_matches.append(d)
    elif ff == "phone" and "iPhone" in model:
        form_matches.append(d)
if not form_matches:
    sys.exit(1)
if pref:
    for d in form_matches:
        name = d.get("deviceProperties", {}).get("name", "")
        udid_val = d.get("identifier", "")
        hw_udid = d.get("hardwareProperties", {}).get("udid", "")
        if pref.lower() in name.lower() or pref.lower() in udid_val.lower() or pref.lower() in hw_udid.lower():
            print(udid_val); sys.exit(0)
print(form_matches[0]["identifier"]); sys.exit(0)
PY
2>/dev/null || true)
    rm -f "$tmpfile"

    if [[ -n "$udid" ]]; then
        IOS_DEVICE_UDID="$udid"
        echo "$udid"
        return 0
    fi

    echo "No connected iOS physical device for form-factor '$ff'." >&2
    return 1
}

# Check if a bundle is running on a physical iOS device via devicectl.
ios_device_is_running() {
    local udid="$1"; local bundle_id="$2"
    local tmpfile
    tmpfile=$(mktemp)
    local running=1
    if xcrun devicectl device info processes -d "$udid" -j "$tmpfile" --quiet 2>/dev/null; then
        local pid
        pid=$(python3 -c "
import json
d = json.load(open('$tmpfile'))
procs = d.get('result', {}).get('runningProcesses', [])
bundle_id = '$bundle_id'
app_name = bundle_id.split('.')[-1].lower()
matches = [p for p in procs if bundle_id in str(p.get('executable', ''))
           or ('/' + app_name + '.app/') in str(p.get('executable', '')).lower()
           or str(p.get('executable', '')).lower().endswith('/' + app_name)]
print(matches[0]['processIdentifier'] if matches else '')
" 2>/dev/null || true)
        [[ -n "$pid" ]] && running=0
    fi
    rm -f "$tmpfile"
    return "$running"
}

ios_device_crash_count() {
    local bundle_id="$1"
    ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null \
        | grep -c "$bundle_id" || true
}

# Resolve the macOS host LAN IP for passing to physical devices.
host_lan_ip() {
    ipconfig getifaddr en0 2>/dev/null \
        || ipconfig getifaddr en1 2>/dev/null \
        || route -n get default 2>/dev/null | awk '/interface:/{print $2}' | xargs ipconfig getifaddr 2>/dev/null \
        || true
}

# ── Android helpers ──────────────────────────────────────────────────

android_get_serial() {
    local ff="${1:-}"

    # If spyder resolved a device alias, use it directly as the adb serial.
    if [[ -n "$PINNED_DEVICE" ]]; then
        local resolved
        resolved=$(spyder resolve "$PINNED_DEVICE" --json 2>/dev/null \
            | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('android_serial',''))" \
            2>/dev/null || true)
        if [[ -n "$resolved" ]]; then
            echo "$resolved"
            return 0
        fi
        # No android_serial in inventory — treat PINNED_DEVICE as raw serial.
        echo "$PINNED_DEVICE"
        return 0
    fi

    local serials
    serials=$(adb devices 2>/dev/null \
        | awk 'NR>1 && /emulator-[0-9]+[[:space:]]+device$/ {print $1}')

    if [[ -z "$serials" ]]; then
        echo "No running Android emulator found." >&2
        return 1
    fi

    if [[ -z "$ff" ]]; then
        echo "$serials" | head -1
        return 0
    fi

    if [[ "$ff" == "tablet" && -n "${GE_ANDROID_TABLET_AVD:-}" ]]; then
        while IFS= read -r serial; do
            local avd_name
            avd_name=$(adb -s "$serial" emu avd name 2>/dev/null | head -1 | tr -d '\r' || true)
            if [[ "$avd_name" == "$GE_ANDROID_TABLET_AVD" ]]; then
                echo "$serial"; return 0
            fi
        done <<< "$serials"
    fi

    while IFS= read -r serial; do
        local avd_name
        avd_name=$(adb -s "$serial" emu avd name 2>/dev/null | head -1 | tr -d '\r' || true)
        case "$ff" in
            phone)
                if ! echo "$avd_name" | grep -qi "tablet"; then
                    echo "$serial"; return 0
                fi
                ;;
            tablet)
                if echo "$avd_name" | grep -qi "tablet"; then
                    echo "$serial"; return 0
                fi
                ;;
        esac
    done <<< "$serials"

    echo "No running Android emulator matching form-factor '$ff'." >&2
    echo "Available emulators:" >&2
    while IFS= read -r serial; do
        local avd_name
        avd_name=$(adb -s "$serial" emu avd name 2>/dev/null | head -1 | tr -d '\r' || true)
        echo "  $serial ($avd_name)" >&2
    done <<< "$serials"
    return 1
}

android_device_get_serial() {
    local ff="${1:-}"

    if [[ -n "$PINNED_DEVICE" ]]; then
        local resolved
        resolved=$(spyder resolve "$PINNED_DEVICE" --json 2>/dev/null \
            | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('android_serial',''))" \
            2>/dev/null || true)
        if [[ -n "$resolved" ]]; then
            echo "$resolved"
            return 0
        fi
        echo "$PINNED_DEVICE"
        return 0
    fi

    local serials
    serials=$(adb devices 2>/dev/null \
        | awk 'NR>1 && !/emulator-/ && /[[:space:]]+device$/ {print $1}')

    if [[ -z "$serials" ]]; then
        echo "No USB-connected Android device found." >&2
        return 1
    fi

    if [[ -z "$ff" ]]; then
        echo "$serials" | head -1
        return 0
    fi

    while IFS= read -r serial; do
        local size
        size=$(adb -s "$serial" shell wm size 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | head -1 || true)
        local w h
        w=$(echo "$size" | cut -dx -f1)
        h=$(echo "$size" | cut -dx -f2)
        if [[ -n "$w" && -n "$h" && "$w" -gt 0 ]]; then
            local ratio
            ratio=$(python3 -c "print('phone' if max($h,$w)/min($h,$w) > 1.4 else 'tablet')" 2>/dev/null || echo unknown)
            if [[ "$ratio" == "$ff" ]]; then
                echo "$serial"; return 0
            fi
        fi
    done <<< "$serials"

    echo "No USB-connected Android device matching form-factor '$ff'." >&2
    return 1
}

android_get_activity() {
    local pkg="$1"
    local manifest="$APP_DIR/android/app/src/main/AndroidManifest.xml"
    local activity_raw=""
    if [[ -f "$manifest" ]]; then
        activity_raw=$(grep -oE 'android:name="[A-Za-z0-9_.]+Activity"' "$manifest" \
            | head -1 | sed -E 's/.*"([^"]+)".*/\1/' || true)
    fi
    [[ -z "$activity_raw" ]] && activity_raw=".${APP_DISPLAY_NAME}Activity"
    echo "${pkg}/${activity_raw}"
}

android_crash_count() {
    local serial="$1"; local since_epoch="$2"; local pkg="$3"
    adb -s "$serial" logcat -b crash -d 2>/dev/null \
        | grep -c "Process: $pkg" || true
}

android_is_running() {
    local serial="$1"; local pkg="$2"
    local attempt
    for attempt in 1 2 3 4 5; do
        if adb -s "$serial" shell pidof "$pkg" 2>/dev/null | grep -q '[0-9]'; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# ── Startup-flash check (mobile only) ────────────────────────────────

check_startup_flash_ios() {
    local subcheck="startup-flash"
    local udid="$1"; local bundle_id="$2"
    local out_dir="$ARTIFACTS/startup-flash"
    mkdir -p "$out_dir"

    if ! command -v ffmpeg >/dev/null 2>&1; then
        fail_check "$subcheck" "ffmpeg not found; cannot scan for magenta frames"
        return 1
    fi
    if ! command -v convert >/dev/null 2>&1; then
        fail_check "$subcheck" "ImageMagick convert not found; cannot scan frames"
        return 1
    fi

    local video="$out_dir/launch.mov"
    xcrun simctl terminate "$udid" "$bundle_id" 2>/dev/null || true
    sleep 0.5

    xcrun simctl io "$udid" recordVideo --codec=h264 --force "$video" &
    local rec_pid=$!
    sleep 0.5
    xcrun simctl launch "$udid" "$bundle_id" >/dev/null 2>&1 || true
    sleep 3
    kill -INT "$rec_pid" 2>/dev/null || true
    wait "$rec_pid" 2>/dev/null || true

    if [[ ! -s "$video" ]]; then
        fail_check "$subcheck" "video capture empty"
        return 1
    fi

    ffmpeg -y -i "$video" -vf fps=10 "$out_dir/frame_%04d.png" \
        >/dev/null 2>&1 || true

    local frame_count
    frame_count=$(ls "$out_dir"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$frame_count" -eq 0 ]]; then
        fail_check "$subcheck" "no frames extracted from video"
        return 1
    fi

    local magenta_frame=""
    for f in "$out_dir"/frame_*.png; do
        local pct
        pct=$(python3 - "$f" <<'PY'
import sys
try:
    import subprocess, re
    result = subprocess.run(
        ['convert', sys.argv[1],
         '-fx', 'r>0.78 && g<0.31 && b>0.78 ? 1 : 0',
         '-format', '%[fx:mean]', 'info:'],
        capture_output=True, text=True, timeout=10
    )
    val = float(result.stdout.strip()) if result.stdout.strip() else 0.0
    print(f'{val*100:.1f}')
except Exception as e:
    print('0.0')
PY
) 2>/dev/null || pct="0.0"
        local pct_int
        pct_int=$(python3 -c "print(int(float('$pct')))" 2>/dev/null || echo 0)
        if [[ "$pct_int" -gt 5 ]]; then
            magenta_frame="$(basename "$f") ($pct%)"
            break
        fi
    done

    if [[ -n "$magenta_frame" ]]; then
        fail_check "$subcheck" "magenta flash detected in $magenta_frame"
        return 1
    fi
    pass_check "$subcheck" "no magenta flash in $frame_count frames"
}

check_startup_flash_android() {
    local subcheck="startup-flash"
    local serial="$1"
    local pkg="$2"
    local activity="$3"
    local extra_am_args="${4:-}"
    local out_dir="$ARTIFACTS/startup-flash"
    mkdir -p "$out_dir"

    if ! command -v ffmpeg >/dev/null 2>&1; then
        fail_check "$subcheck" "ffmpeg not found; cannot scan for magenta frames"
        return 1
    fi
    if ! command -v convert >/dev/null 2>&1; then
        fail_check "$subcheck" "ImageMagick convert not found; cannot scan frames"
        return 1
    fi

    local video_device="/sdcard/matrix-launch.mp4"
    local video_local="$out_dir/launch.mp4"

    adb -s "$serial" shell am force-stop "$pkg" 2>/dev/null || true
    sleep 0.5

    adb -s "$serial" shell screenrecord --time-limit 3 --bit-rate 2000000 "$video_device" &
    local rec_pid=$!
    sleep 0.5
    # shellcheck disable=SC2086
    adb -s "$serial" shell am start -n "$activity" $extra_am_args >/dev/null 2>&1 || true

    local waited=0
    while kill -0 "$rec_pid" 2>/dev/null && [[ $waited -lt 8 ]]; do
        sleep 1
        (( waited++ )) || true
    done
    kill "$rec_pid" 2>/dev/null || true
    wait "$rec_pid" 2>/dev/null || true
    adb -s "$serial" pull "$video_device" "$video_local" >/dev/null 2>&1 || true
    adb -s "$serial" shell rm -f "$video_device" >/dev/null 2>&1 || true

    if [[ ! -s "$video_local" ]]; then
        warn_check "$subcheck" "video capture empty — screenrecord may have failed; skipping flash check"
        return 0
    fi

    ffmpeg -y -i "$video_local" -vf fps=10 "$out_dir/frame_%04d.png" \
        >/dev/null 2>&1 || true

    local frame_count
    frame_count=$(ls "$out_dir"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$frame_count" -eq 0 ]]; then
        warn_check "$subcheck" "no frames extracted from video (duration=0?); skipping flash check"
        return 0
    fi

    local magenta_frame=""
    for f in "$out_dir"/frame_*.png; do
        local pct
        pct=$(python3 - "$f" <<'PY'
import sys
try:
    import subprocess
    result = subprocess.run(
        ['convert', sys.argv[1],
         '-fx', 'r>0.78 && g<0.31 && b>0.78 ? 1 : 0',
         '-format', '%[fx:mean]', 'info:'],
        capture_output=True, text=True, timeout=10
    )
    val = float(result.stdout.strip()) if result.stdout.strip() else 0.0
    print(f'{val*100:.1f}')
except Exception:
    print('0.0')
PY
) 2>/dev/null || pct="0.0"
        local pct_int
        pct_int=$(python3 -c "print(int(float('$pct')))" 2>/dev/null || echo 0)
        if [[ "$pct_int" -gt 5 ]]; then
            magenta_frame="$(basename "$f") ($pct%)"
            break
        fi
    done

    if [[ -n "$magenta_frame" ]]; then
        fail_check "$subcheck" "magenta flash detected in $magenta_frame"
        return 1
    fi
    pass_check "$subcheck" "no magenta flash in $frame_count frames"
}

# ── Sub-check: cold-launch ───────────────────────────────────────────

LAUNCH_PID=""    # desktop only; sim/emu/device don't give us a local PID
SIM_UDID=""      # set by ios launch helpers
ANDROID_SERIAL=""  # set by android launch helpers
DESKTOP_BIN=""   # overridden by debug cells to point at bin/$APP_NAME-debug
SERVER_ARGS=""   # extra args passed to the desktop server; player cells set --brokered

cold_launch_desktop() {
    local subcheck="cold-launch"
    local bin="${DESKTOP_BIN:-$APP_DIR/bin/$APP_NAME}"
    [[ -x "$bin" ]] || { fail_check "$subcheck" "binary not found: $bin"; return 1; }

    "$bin" > "$ARTIFACTS/app.log" 2>&1 &
    LAUNCH_PID=$!

    local deadline=$(($(date +%s) + COLD_LAUNCH_TIMEOUT))
    while [[ $(date +%s) -lt $deadline ]]; do
        if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
            fail_check "$subcheck" "app exited immediately (log: $ARTIFACTS/app.log)"
            return 1
        fi
        sleep 0.5
    done
    if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
        fail_check "$subcheck" "app crashed (log: $ARTIFACTS/app.log)"
        return 1
    fi

    local shot="$ARTIFACTS/cold-launch.png"
    shot_window_by_pid "$LAUNCH_PID" "$shot"
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty"
        return 1
    fi

    check_or_capture_ref "$subcheck" "$shot" "$CELL-cold-launch.png" || return 1
}

cold_launch_player_desktop() {
    local subcheck="cold-launch"
    local player_bin="$APP_DIR/bin/player"
    [[ -x "$player_bin" ]] || { fail_check "$subcheck" "player binary not found: $player_bin"; return 1; }

    "$player_bin" > "$ARTIFACTS/player.log" 2>&1 &
    LAUNCH_PID=$!

    local baseline
    baseline=$(current_sessions)
    sleep 2
    if ! wait_for_sessions $((baseline + 1)); then
        fail_check "$subcheck" "player did not connect to stream relay within ${COLD_LAUNCH_TIMEOUT}s"
        kill_bg "$LAUNCH_PID"
        return 1
    fi

    local shot="$ARTIFACTS/cold-launch.png"
    shot_window_by_pid "$LAUNCH_PID" "$shot"
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty"
        return 1
    fi
    check_or_capture_ref "$subcheck" "$shot" "$CELL-cold-launch.png" || return 1
}

cold_launch_ios_sim() {
    local subcheck="cold-launch"
    local bundle_id="$1"; local app_path="$2"; local ff="$3"
    local extra_launch_args="${4:-}"

    SIM_UDID=$(ios_sim_get_udid "$ff") || { fail_check "$subcheck" "no booted iOS sim for $ff"; return 1; }

    xcrun simctl terminate "$SIM_UDID" "$bundle_id" 2>/dev/null || true
    run xcrun simctl install "$SIM_UDID" "$app_path" \
        || { fail_check "$subcheck" "simctl install failed"; return 1; }
    # shellcheck disable=SC2086
    run xcrun simctl launch "$SIM_UDID" "$bundle_id" $extra_launch_args \
        || { fail_check "$subcheck" "simctl launch failed"; return 1; }

    sleep "$COLD_LAUNCH_TIMEOUT"
    if ! ios_sim_is_running "$SIM_UDID" "$bundle_id"; then
        fail_check "$subcheck" "app not running after launch"
        return 1
    fi

    local shot="$ARTIFACTS/cold-launch.png"
    xcrun simctl io "$SIM_UDID" screenshot "$shot" >/dev/null 2>&1
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty"
        return 1
    fi
    check_or_capture_ref "$subcheck" "$shot" "$CELL-cold-launch.png" || return 1
}

cold_launch_android_emu() {
    local subcheck="cold-launch"
    local pkg="$1"; local apk="$2"; local ff="$3"; local activity="$4"
    local extra_am_args="${5:-}"

    ANDROID_SERIAL=$(android_get_serial "$ff") \
        || { fail_check "$subcheck" "no Android emulator for $ff"; return 1; }

    adb -s "$ANDROID_SERIAL" shell am force-stop "$pkg" 2>/dev/null || true
    run adb -s "$ANDROID_SERIAL" install -r "$apk" \
        || { fail_check "$subcheck" "adb install failed"; return 1; }
    # shellcheck disable=SC2086
    run adb -s "$ANDROID_SERIAL" shell am start -n "$activity" $extra_am_args \
        || { fail_check "$subcheck" "adb am start failed ($activity)"; return 1; }

    sleep "$COLD_LAUNCH_TIMEOUT"
    if ! android_is_running "$ANDROID_SERIAL" "$pkg"; then
        fail_check "$subcheck" "app not running after launch"
        return 1
    fi

    local shot="$ARTIFACTS/cold-launch.png"
    adb -s "$ANDROID_SERIAL" exec-out screencap -p > "$shot" 2>/dev/null
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty"
        return 1
    fi
    check_or_capture_ref "$subcheck" "$shot" "$CELL-cold-launch.png" || return 1
}

cold_launch_android_device() {
    local subcheck="cold-launch"
    local pkg="$1"; local apk="$2"; local ff="$3"; local activity="$4"
    local extra_am_args="${5:-}"

    ANDROID_SERIAL=$(android_device_get_serial "$ff") \
        || { fail_check "$subcheck" "no USB-connected Android device for $ff"; return 1; }

    adb -s "$ANDROID_SERIAL" shell am force-stop "$pkg" 2>/dev/null || true
    run adb -s "$ANDROID_SERIAL" install -r "$apk" \
        || { fail_check "$subcheck" "adb install failed"; return 1; }
    # shellcheck disable=SC2086
    run adb -s "$ANDROID_SERIAL" shell am start -n "$activity" $extra_am_args \
        || { fail_check "$subcheck" "adb am start failed ($activity)"; return 1; }

    sleep "$COLD_LAUNCH_TIMEOUT"
    if ! android_is_running "$ANDROID_SERIAL" "$pkg"; then
        fail_check "$subcheck" "app not running after launch"
        return 1
    fi

    local shot="$ARTIFACTS/cold-launch.png"
    adb -s "$ANDROID_SERIAL" exec-out screencap -p > "$shot" 2>/dev/null
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty"
        return 1
    fi
    check_or_capture_ref "$subcheck" "$shot" "$CELL-cold-launch.png" || return 1
}

cold_launch_ios_device() {
    local subcheck="cold-launch"
    local bundle_id="$1"; local app_path="$2"; local ff="$3"
    local extra_launch_args="${4:-}"

    IOS_DEVICE_UDID=$(ios_device_get_udid "$ff") \
        || { fail_check "$subcheck" "no connected iOS device for $ff"; return 1; }

    local tmpfile
    tmpfile=$(mktemp)
    if xcrun devicectl device info processes -d "$IOS_DEVICE_UDID" -j "$tmpfile" --quiet 2>/dev/null; then
        local old_pid
        old_pid=$(python3 -c "
import sys, json
d = json.load(open('$tmpfile'))
procs = d.get('result', {}).get('runningProcesses', [])
app_name = '$bundle_id'.split('.')[-1].lower()
matches = [p for p in procs if '$bundle_id' in str(p.get('executable', ''))
           or ('/' + app_name + '.app/') in str(p.get('executable', '')).lower()
           or str(p.get('executable', '')).lower().endswith('/' + app_name)]
print(matches[0]['processIdentifier'] if matches else '')
" 2>/dev/null || true)
        if [[ -n "$old_pid" ]]; then
            xcrun devicectl device process terminate -d "$IOS_DEVICE_UDID" --pid "$old_pid" --quiet 2>/dev/null || true
            sleep 0.5
        fi
    fi
    rm -f "$tmpfile"

    run xcrun devicectl device install app -d "$IOS_DEVICE_UDID" "$app_path" --quiet \
        || { fail_check "$subcheck" "devicectl install failed"; return 1; }

    local launch_tmp launch_err
    launch_tmp=$(mktemp)
    launch_err=$(mktemp)
    # shellcheck disable=SC2086
    if ! xcrun devicectl device process launch -d "$IOS_DEVICE_UDID" \
            -j "$launch_tmp" --quiet "$bundle_id" $extra_launch_args 2>"$launch_err"; then
        local err_msg
        err_msg=$(cat "$launch_err" 2>/dev/null || true)
        rm -f "$launch_tmp" "$launch_err"
        if echo "$err_msg" | grep -qi "locked\|Locked"; then
            fail_check "$subcheck" "device is locked — unlock the device and retry"
        else
            fail_check "$subcheck" "devicectl launch failed"
        fi
        return 1
    fi
    rm -f "$launch_tmp" "$launch_err"

    sleep "$COLD_LAUNCH_TIMEOUT"
    if ! ios_device_is_running "$IOS_DEVICE_UDID" "$bundle_id"; then
        fail_check "$subcheck" "app not running after launch"
        return 1
    fi

    local shot="$ARTIFACTS/cold-launch.png"
    python3 -m pymobiledevice3 screenshot "$shot" 2>/dev/null || true
    if [[ ! -s "$shot" ]]; then
        pass_check "$subcheck" "app running (screenshot unavailable — device may be locked)"
        return 0
    fi
    check_or_capture_ref "$subcheck" "$shot" "$CELL-cold-launch.png" || return 1
}

# ── Sub-check: soak ──────────────────────────────────────────────────

check_soak_desktop() {
    local subcheck="soak"
    local pid="$1"
    echo "  … soak ${SOAK_TIME}s …"
    sleep "$SOAK_TIME"
    if ! kill -0 "$pid" 2>/dev/null; then
        fail_check "$subcheck" "app exited during soak"
        return 1
    fi
    local crashes
    crashes=$(ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null \
        | grep -c "$APP_NAME" || true)
    if [[ "$crashes" -gt 0 ]]; then
        fail_check "$subcheck" "$crashes crash report(s) in ~/Library/Logs/DiagnosticReports/"
        return 1
    fi
    pass_check "$subcheck" "${SOAK_TIME}s — app alive, no crash reports"
}

# check_accelsynth_ios_sim <udid> <bundle_id>
#
# Verifies that AccelSynth is active (no real sensor) and produces non-zero
# sensor output on an iOS Simulator distribution build (🎯T28.3).
#
# Strategy: relaunch the app with GE_ACCELSYNTH_AUTODRIVE=1 via the
# SIMCTL_CHILD_ prefix mechanism. AccelSynth.h's update() method drives a
# synthetic 100 px X-axis tilt for the first 2 seconds when this env var is
# set, gated strictly on TARGET_OS_SIMULATOR. NSLog output from the app is
# captured via `xcrun simctl spawn <udid> log stream` filtered by process
# name. The check asserts:
#   1. The "Shift-mouse accelerometer synthesis enabled" line appears — synth
#      was constructed because no real sensor was found.
#   2. An "AccelSynth: emit g=" line with a non-zero gx value appears —
#      the autodrive actually fired and produced sensor events.
check_accelsynth_ios_sim() {
    local subcheck="accelsynth"
    local udid="$1"; local bundle_id="$2"

    local log_file="$ARTIFACTS/accelsynth-log.txt"

    # Terminate any running instance so we get a clean log window.
    xcrun simctl terminate "$udid" "$bundle_id" 2>/dev/null || true
    sleep 0.3

    # Start log stream, capturing all info-level output from the sim.
    # We grep for our AccelSynth strings afterwards — filtering by predicate
    # in the command is fragile due to quoting, and the log volume during a
    # 5 s window is manageable.
    local log_pid
    xcrun simctl spawn "$udid" log stream --level info \
        > "$log_file" 2>/dev/null &
    log_pid=$!

    # Launch with GE_ACCELSYNTH_AUTODRIVE=1. The SIMCTL_CHILD_ prefix injects
    # the variable into the sim process environment.
    SIMCTL_CHILD_GE_ACCELSYNTH_AUTODRIVE=1 \
        xcrun simctl launch "$udid" "$bundle_id" >/dev/null 2>&1 || true

    # Wait up to 5 s for the autodrive window plus a margin.
    local waited=0
    while [[ $waited -lt 5 ]]; do
        sleep 1
        waited=$((waited + 1))
        if grep -q "AccelSynth: emit g=" "$log_file" 2>/dev/null; then
            break
        fi
    done

    kill "$log_pid" 2>/dev/null || true
    wait "$log_pid" 2>/dev/null || true

    # Sub-check 1: synth was enabled (no real sensor found).
    if ! grep -q "accelerometer synthesis enabled" "$log_file" 2>/dev/null; then
        fail_check "$subcheck" "synth-enabled log line not found (real sensor may be present, or NSLog sink not wired)"
        return 1
    fi

    # Sub-check 2: autodrive produced a non-zero emit.
    # The emit line looks like: AccelSynth: emit g=(2.54,0.00)
    # We parse out gx and verify it is not zero.
    if ! grep -q "AccelSynth: emit g=" "$log_file" 2>/dev/null; then
        fail_check "$subcheck" "no AccelSynth emit line found in log"
        return 1
    fi
    local sample_line
    sample_line=$(grep "AccelSynth: emit g=" "$log_file" 2>/dev/null | head -1)
    local gx
    gx=$(echo "$sample_line" | sed -n 's/.*g=(\([^,]*\),.*/\1/p')
    if [[ -z "$gx" ]]; then
        warn_check "$subcheck" "could not parse gx from emit line: $sample_line"
        return 0
    fi
    # Any non-zero float passes. Zero appears as "0" or "0.0..." variants.
    if [[ "$gx" =~ ^-?0(\.0+)?$ ]]; then
        fail_check "$subcheck" "AccelSynth emitted zero gx — autodrive may not have fired (gx=$gx)"
        return 1
    fi
    pass_check "$subcheck" "synth active, non-zero emit confirmed (gx=$gx)"
}

# Helper: derive a process name from a bundle ID for log stream filtering.
# Tries the last component of the reverse-DNS ID (e.g. "tiltbuggy" from
# "com.squz.tiltbuggy"). Falls back to the full bundle ID.
app_name_from_bundle() {
    local bundle_id="$1"
    echo "$bundle_id" | awk -F. '{print $NF}'
}

check_soak_ios_sim() {
    local subcheck="soak"
    local udid="$1"; local bundle_id="$2"
    local crash_before
    crash_before=$(ios_sim_crash_count "$bundle_id")
    echo "  … soak ${SOAK_TIME}s …"
    sleep "$SOAK_TIME"
    if ! ios_sim_is_running "$udid" "$bundle_id"; then
        fail_check "$subcheck" "app exited during soak"
        return 1
    fi
    local crash_after
    crash_after=$(ios_sim_crash_count "$bundle_id")
    if [[ "$crash_after" -gt "$crash_before" ]]; then
        fail_check "$subcheck" "$((crash_after - crash_before)) new crash report(s) during soak"
        return 1
    fi
    pass_check "$subcheck" "${SOAK_TIME}s — app alive, no new crashes"
}

check_soak_ios_device() {
    local subcheck="soak"
    local udid="$1"; local bundle_id="$2"
    local crash_before
    crash_before=$(ios_device_crash_count "$bundle_id")
    echo "  … soak ${SOAK_TIME}s …"
    sleep "$SOAK_TIME"
    if ! ios_device_is_running "$udid" "$bundle_id"; then
        fail_check "$subcheck" "app exited during soak"
        return 1
    fi
    local crash_after
    crash_after=$(ios_device_crash_count "$bundle_id")
    if [[ "$crash_after" -gt "$crash_before" ]]; then
        fail_check "$subcheck" "$((crash_after - crash_before)) new crash report(s) during soak"
        return 1
    fi
    pass_check "$subcheck" "${SOAK_TIME}s — app alive, no new crashes"
}

check_soak_android() {
    local subcheck="soak"
    local serial="$1"; local pkg="$2"
    echo "  … soak ${SOAK_TIME}s …"
    sleep "$SOAK_TIME"
    if ! android_is_running "$serial" "$pkg"; then
        fail_check "$subcheck" "app exited during soak"
        return 1
    fi
    local crashes
    crashes=$(adb -s "$serial" logcat -b crash -d 2>/dev/null \
        | grep -c "Process: $pkg" || true)
    if [[ "$crashes" -gt 0 ]]; then
        fail_check "$subcheck" "$crashes crash log line(s) for $pkg"
        return 1
    fi
    pass_check "$subcheck" "${SOAK_TIME}s — app alive, no crash log entries"
}

# ── Sub-check: rotation round-trip (sim/emu only) ────────────────────

check_rotation_ios_sim() {
    local subcheck="rotation-round-trip"
    local udid="$1"; local bundle_id="$2"

    osascript -e 'tell application "Simulator" to activate' >/dev/null 2>&1 || true
    sleep 0.5

    osascript -e 'tell application "System Events" to keystroke (ASCII character 29) using command down' \
        >/dev/null 2>&1 || true
    sleep 1
    if ! ios_sim_is_running "$udid" "$bundle_id"; then
        fail_check "$subcheck" "app crashed after rotate-right"
        return 1
    fi

    osascript -e 'tell application "System Events" to keystroke (ASCII character 28) using command down' \
        >/dev/null 2>&1 || true
    sleep 1
    if ! ios_sim_is_running "$udid" "$bundle_id"; then
        fail_check "$subcheck" "app crashed after rotate-left"
        return 1
    fi

    pass_check "$subcheck" "app survived portrait→landscape→portrait"
}

check_rotation_android_emu() {
    local subcheck="rotation-round-trip"
    local serial="$1"; local pkg="$2"

    adb -s "$serial" shell settings put system accelerometer_rotation 0 >/dev/null 2>&1 || true

    adb -s "$serial" shell settings put system user_rotation 1 >/dev/null 2>&1 || true
    sleep 1
    if ! android_is_running "$serial" "$pkg"; then
        fail_check "$subcheck" "app crashed after rotate to landscape"
        return 1
    fi

    adb -s "$serial" shell settings put system user_rotation 3 >/dev/null 2>&1 || true
    sleep 1
    if ! android_is_running "$serial" "$pkg"; then
        fail_check "$subcheck" "app crashed after rotate to landscape-right"
        return 1
    fi

    adb -s "$serial" shell settings put system user_rotation 0 >/dev/null 2>&1 || true
    sleep 1
    if ! android_is_running "$serial" "$pkg"; then
        fail_check "$subcheck" "app crashed after rotate back to portrait"
        return 1
    fi

    pass_check "$subcheck" "app survived portrait→landscape×2→portrait"
}

# ── Sub-check: reconnect (player cells only) ─────────────────────────

check_reconnect_desktop_player() {
    local subcheck="reconnect"
    local srv_pid="$1"; local player_pid="$2"

    kill_bg "$srv_pid"
    sleep 5

    local serverlog="$ARTIFACTS/server-reconnect.log"
    local srv_bin="${DESKTOP_BIN:-$APP_DIR/bin/$APP_NAME}"
    # shellcheck disable=SC2086
    ( cd "$APP_DIR" && exec "$srv_bin" $SERVER_ARGS > "$serverlog" 2>&1 ) &
    local new_srv_pid=$!

    if ! wait_for_server_sessions 1; then
        fail_check "$subcheck" "player did not reconnect within ${COLD_LAUNCH_TIMEOUT}s"
        kill_bg "$new_srv_pid"
        return 1
    fi

    local shot="$ARTIFACTS/reconnect.png"
    shot_window_by_pid "$player_pid" "$shot"
    kill_bg "$new_srv_pid"

    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "post-reconnect screenshot empty"
        return 1
    fi
    pass_check "$subcheck" "player reconnected and rendered"
}

check_reconnect_ios_sim_player() {
    local subcheck="reconnect"
    local srv_pid="$1"; local udid="$2"

    kill_bg "$srv_pid"
    sleep 5

    local serverlog="$ARTIFACTS/server-reconnect.log"
    local srv_bin="${DESKTOP_BIN:-$APP_DIR/bin/$APP_NAME}"
    # shellcheck disable=SC2086
    ( cd "$APP_DIR" && exec "$srv_bin" $SERVER_ARGS > "$serverlog" 2>&1 ) &
    local new_srv_pid=$!

    if ! wait_for_server_sessions 1; then
        fail_check "$subcheck" "iOS player did not reconnect within ${COLD_LAUNCH_TIMEOUT}s"
        kill_bg "$new_srv_pid"
        return 1
    fi

    local shot="$ARTIFACTS/reconnect.png"
    xcrun simctl io "$udid" screenshot "$shot" >/dev/null 2>&1
    kill_bg "$new_srv_pid"

    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "post-reconnect screenshot empty"
        return 1
    fi
    pass_check "$subcheck" "iOS player reconnected and rendered"
}

check_reconnect_android_player() {
    local subcheck="reconnect"
    local srv_pid="$1"; local serial="$2"

    kill_bg "$srv_pid"
    sleep 5

    local serverlog="$ARTIFACTS/server-reconnect.log"
    local srv_bin="${DESKTOP_BIN:-$APP_DIR/bin/$APP_NAME}"
    # shellcheck disable=SC2086
    ( cd "$APP_DIR" && exec "$srv_bin" $SERVER_ARGS > "$serverlog" 2>&1 ) &
    local new_srv_pid=$!

    if ! wait_for_server_sessions 1; then
        fail_check "$subcheck" "Android player did not reconnect within ${COLD_LAUNCH_TIMEOUT}s"
        kill_bg "$new_srv_pid"
        return 1
    fi

    local shot="$ARTIFACTS/reconnect.png"
    adb -s "$serial" exec-out screencap -p > "$shot" 2>/dev/null
    kill_bg "$new_srv_pid"

    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "post-reconnect screenshot empty"
        return 1
    fi
    pass_check "$subcheck" "Android player reconnected and rendered"
}

# ── Sub-check: bg/fg (mobile only) ──────────────────────────────────

check_bgfg_ios() {
    local subcheck="bg/fg"
    local udid="$1"; local bundle_id="$2"

    xcrun simctl launch "$udid" com.apple.springboard >/dev/null 2>&1 || true
    sleep 5

    xcrun simctl launch "$udid" "$bundle_id" >/dev/null 2>&1 \
        || { fail_check "$subcheck" "failed to foreground app"; return 1; }
    sleep 2

    if ! ios_sim_is_running "$udid" "$bundle_id"; then
        fail_check "$subcheck" "app not running after foreground"
        return 1
    fi

    local shot="$ARTIFACTS/bgfg.png"
    xcrun simctl io "$udid" screenshot "$shot" >/dev/null 2>&1
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty after foreground"
        return 1
    fi
    pass_check "$subcheck" "app survived background→foreground"
}

check_bgfg_ios_device() {
    local subcheck="bg/fg"
    local udid="$1"; local bundle_id="$2"

    local launch_tmp
    launch_tmp=$(mktemp)
    if ! xcrun devicectl device process launch -d "$udid" \
            -j "$launch_tmp" --quiet "$bundle_id" 2>/dev/null; then
        rm -f "$launch_tmp"
        fail_check "$subcheck" "failed to re-launch app"
        return 1
    fi
    rm -f "$launch_tmp"
    sleep 2

    if ! ios_device_is_running "$udid" "$bundle_id"; then
        fail_check "$subcheck" "app not running after re-launch"
        return 1
    fi
    pass_check "$subcheck" "app survived terminate→relaunch"
}

check_bgfg_android() {
    local subcheck="bg/fg"
    local serial="$1"; local pkg="$2"; local activity="$3"

    adb -s "$serial" shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
    sleep 5

    adb -s "$serial" shell am start -n "$activity" >/dev/null 2>&1 \
        || { fail_check "$subcheck" "failed to foreground app"; return 1; }
    sleep 2

    if ! android_is_running "$serial" "$pkg"; then
        fail_check "$subcheck" "app not running after foreground"
        return 1
    fi

    local shot="$ARTIFACTS/bgfg.png"
    adb -s "$serial" exec-out screencap -p > "$shot" 2>/dev/null
    if [[ ! -s "$shot" ]]; then
        fail_check "$subcheck" "screenshot empty after foreground"
        return 1
    fi
    pass_check "$subcheck" "app survived background→foreground"
}

# ── Sub-check: clean-exit ────────────────────────────────────────────

check_clean_exit_desktop() {
    local subcheck="clean-exit"
    local pid="$1"
    local crash_before
    crash_before=$(ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null \
        | grep -c "$APP_NAME" || true)

    kill "$pid" 2>/dev/null || true
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
        sleep 0.5
    fi
    wait "$pid" 2>/dev/null || true

    local crash_after
    crash_after=$(ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null \
        | grep -c "$APP_NAME" || true)
    if [[ "$crash_after" -gt "$crash_before" ]]; then
        fail_check "$subcheck" "crash report generated on exit"
        return 1
    fi
    pass_check "$subcheck"
}

check_clean_exit_ios_sim() {
    local subcheck="clean-exit"
    local udid="$1"; local bundle_id="$2"
    local crash_before
    crash_before=$(ios_sim_crash_count "$bundle_id")

    xcrun simctl terminate "$udid" "$bundle_id" 2>/dev/null || true
    sleep 1

    local crash_after
    crash_after=$(ios_sim_crash_count "$bundle_id")
    if [[ "$crash_after" -gt "$crash_before" ]]; then
        fail_check "$subcheck" "crash report generated on exit"
        return 1
    fi
    pass_check "$subcheck"
}

check_clean_exit_ios_device() {
    local subcheck="clean-exit"
    local udid="$1"; local bundle_id="$2"
    local crash_before
    crash_before=$(ios_device_crash_count "$bundle_id")

    local tmpfile
    tmpfile=$(mktemp)
    if xcrun devicectl device info processes -d "$udid" -j "$tmpfile" --quiet 2>/dev/null; then
        local pid
        pid=$(python3 -c "
import sys, json
d = json.load(open('$tmpfile'))
procs = d.get('result', {}).get('runningProcesses', [])
app_name = '$bundle_id'.split('.')[-1].lower()
matches = [p for p in procs if '$bundle_id' in str(p.get('executable', ''))
           or ('/' + app_name + '.app/') in str(p.get('executable', '')).lower()
           or str(p.get('executable', '')).lower().endswith('/' + app_name)]
print(matches[0]['processIdentifier'] if matches else '')
" 2>/dev/null || true)
        if [[ -n "$pid" ]]; then
            xcrun devicectl device process terminate -d "$udid" --pid "$pid" --quiet 2>/dev/null || true
        fi
    fi
    rm -f "$tmpfile"
    sleep 1

    local crash_after
    crash_after=$(ios_device_crash_count "$bundle_id")
    if [[ "$crash_after" -gt "$crash_before" ]]; then
        fail_check "$subcheck" "crash report generated on exit"
        return 1
    fi
    pass_check "$subcheck"
}

check_reconnect_ios_device_player() {
    local subcheck="reconnect"
    local srv_pid="$1"; local udid="$2"

    kill_bg "$srv_pid"
    sleep 5

    local serverlog="$ARTIFACTS/server-reconnect.log"
    local baseline
    baseline=$(current_sessions)
    local srv_bin="${DESKTOP_BIN:-$APP_DIR/bin/$APP_NAME}"
    # shellcheck disable=SC2086
    ( cd "$APP_DIR" && exec "$srv_bin" $SERVER_ARGS > "$serverlog" 2>&1 ) &
    local new_srv_pid=$!

    if ! wait_for_sessions $((baseline + 1)); then
        fail_check "$subcheck" "iOS device player did not reconnect within ${COLD_LAUNCH_TIMEOUT}s"
        kill_bg "$new_srv_pid"
        return 1
    fi
    kill_bg "$new_srv_pid"
    pass_check "$subcheck" "iOS device player reconnected"
}

check_clean_exit_android() {
    local subcheck="clean-exit"
    local serial="$1"; local pkg="$2"

    adb -s "$serial" shell am force-stop "$pkg" 2>/dev/null || true
    sleep 1

    local fatals
    fatals=$(adb -s "$serial" logcat -b crash -d 2>/dev/null \
        | grep -c "Process: $pkg" || true)
    if [[ "$fatals" -gt 0 ]]; then
        fail_check "$subcheck" "$fatals crash entry/entries in logcat"
        return 1
    fi
    pass_check "$subcheck"
}

# ── Build helpers ────────────────────────────────────────────────────

build_desktop() {
    echo "  … building release (make) …"
    ( cd "$APP_DIR" && run make ) || return 1
}

build_desktop_debug() {
    echo "  … building debug (make ge/debug) …"
    ( cd "$APP_DIR" && run make ge/debug ) || return 1
}

build_player_desktop() {
    echo "  … building (make + make ge/player) …"
    ( cd "$APP_DIR" && run make ) || return 1
    ( cd "$APP_DIR" && run make ge/player ) || return 1
}

build_player_desktop_debug() {
    echo "  … building debug server + player (make ge/debug + make ge/player) …"
    ( cd "$APP_DIR" && run make ge/debug ) || return 1
    ( cd "$APP_DIR" && run make ge/player ) || return 1
}

build_ios_sim_debug() {
    echo "  … building iOS debug (make ge/ios) …"
    ( cd "$APP_DIR" && run make ge/ios ) || return 1
}

build_ios_sim_release() {
    echo "  … building iOS release (make ge/ios-release) …"
    ( cd "$APP_DIR" && run make ge/ios-release ) || return 1
}

build_ios_sim() {
    build_ios_sim_release
}

build_android_debug() {
    echo "  … building Android debug (make ge/android) …"
    ( cd "$APP_DIR" && run make ge/android ) || return 1
}

build_android_release() {
    echo "  … building Android release (make ge/android-release) …"
    ( cd "$APP_DIR" && run make ge/android-release ) || return 1
}

build_android() {
    build_android_release
}

find_ios_sim_app_debug() {
    find "$APP_DIR/ios/build" -name "*.app" -path "*Debug-iphonesimulator*" \
        2>/dev/null | head -1
}

find_ios_sim_app_release() {
    find "$APP_DIR/ios/build" -name "*.app" -path "*Release-iphonesimulator*" \
        2>/dev/null | head -1
}

find_ios_sim_app() {
    find_ios_sim_app_release
}

build_ios_device_release() {
    echo "  … building iOS device release (make ge/ios-device-release) …"
    ( cd "$APP_DIR" && run make ge/ios-device-release ) || return 1
}

build_ios_device_player() {
    echo "  … building ge iOS device player (make ge/player-ios-device) …"
    ( cd "$APP_DIR" && run make ge/player-ios-device ) || return 1
}

find_ios_device_app_release() {
    find "$APP_DIR/ios/build" -name "*.app" -path "*Release-iphoneos*" \
        2>/dev/null | head -1
}

find_ios_device_player_app() {
    find "$GE_ROOT/tools/ios/build/xcode" -name "Player.app" -path "*iphoneos*" \
        2>/dev/null | head -1
}

find_android_apk_debug() {
    echo "$APP_DIR/android/app/build/outputs/apk/debug/app-debug.apk"
}

find_android_apk_release() {
    local unsigned="$APP_DIR/android/app/build/outputs/apk/release/app-release-unsigned.apk"
    local signed="$APP_DIR/android/app/build/outputs/apk/release/app-release.apk"
    if [[ -f "$signed" ]]; then echo "$signed"; else echo "$unsigned"; fi
}

find_android_apk() {
    find_android_apk_release
}

# ── Server management for player cells ──────────────────────────────

SERVER_PID=""

start_server() {
    local logf="$ARTIFACTS/server.log"
    local srv_bin="${DESKTOP_BIN:-$APP_DIR/bin/$APP_NAME}"
    # shellcheck disable=SC2086
    ( cd "$APP_DIR" && exec "$srv_bin" $SERVER_ARGS > "$logf" 2>&1 ) &
    SERVER_PID=$!
    sleep 2
}

stop_server() {
    [[ -n "$SERVER_PID" ]] && kill_bg "$SERVER_PID"
    SERVER_PID=""
}

# ── Global cleanup trap ──────────────────────────────────────────────

cleanup() {
    stop_server 2>/dev/null || true
    stop_stream relay 2>/dev/null || true
    [[ -n "$LAUNCH_PID" ]] && kill_bg "$LAUNCH_PID" 2>/dev/null || true
}
trap cleanup EXIT

# ── Cell runners ─────────────────────────────────────────────────────

run_desktop_dist() {
    build_desktop || { fail_check "cold-launch" "build failed"; return; }
    cold_launch_desktop || return
    check_soak_desktop "$LAUNCH_PID" || true
    check_clean_exit_desktop "$LAUNCH_PID"
    LAUNCH_PID=""
}

run_desktop_player() {
    build_player_desktop || { fail_check "cold-launch" "build failed"; return; }
    ensure_ged
    SERVER_ARGS="--brokered"
    start_server
    cold_launch_player_desktop || { stop_server; return; }
    check_soak_desktop "$LAUNCH_PID" || true
    check_reconnect_desktop_player "$SERVER_PID" "$LAUNCH_PID" || true
    SERVER_PID=""  # reconnect killed old server and started a new one; that new one is local
    check_clean_exit_desktop "$LAUNCH_PID"
    LAUNCH_PID=""
}

run_ios_sim_dist() {
    require_cmd xcrun "xcrun not found — Xcode not installed"
    [[ -d "$APP_DIR/ios" ]] || { fail_check "cold-launch" "no ios/ project (run make ge/ios-init)"; return; }
    build_ios_sim_release || { fail_check "cold-launch" "make ge/ios-release failed"; return; }
    local app_path
    app_path=$(find_ios_sim_app_release)
    [[ -n "$app_path" ]] || { fail_check "cold-launch" ".app not found in ios/build/Release-iphonesimulator"; return; }

    cold_launch_ios_sim "$APP_ID" "$app_path" "$FORM_FACTOR" || return
    check_startup_flash_ios "$SIM_UDID" "$APP_ID" || true
    check_soak_ios_sim "$SIM_UDID" "$APP_ID" || true
    check_rotation_ios_sim "$SIM_UDID" "$APP_ID" || true
    check_bgfg_ios "$SIM_UDID" "$APP_ID" || true
    # AccelSynth verification (🎯T28.3): confirm no real sensor → synth
    # constructs, and the GE_ACCELSYNTH_AUTODRIVE path produces non-zero
    # sensor output. Relaunches the app with the autodrive env var via
    # SIMCTL_CHILD_ and reads NSLog output from xcrun simctl spawn log stream.
    check_accelsynth_ios_sim "$SIM_UDID" "$APP_ID" || true
    # Visual regression: capture at known-stable state (post-soak, post-bg/fg, pre-exit).
    # Uses spyder screenshot + diff against baseline in ~/.spyder/visualdiff/ge-tiltbuggy/<cell>/...
    # On mismatch, exits with code 51 (visual-regression-mismatch).
    # Run 'make update-baselines' in the app directory to refresh baselines.
    visual_regression_check "$SIM_UDID" "$CELL" || exit 51
    check_clean_exit_ios_sim "$SIM_UDID" "$APP_ID"
}

run_ios_sim_player() {
    require_cmd xcrun "xcrun not found — Xcode not installed"
    echo "  … building ge iOS player …"
    local ios_proj="$GE_ROOT/tools/ios"
    if [[ ! -d "$ios_proj/build/xcode" ]]; then
        ( cd "$APP_DIR" && run make ge/player-ios ) \
            || { fail_check "cold-launch" "make ge/player-ios failed"; return; }
    fi
    if ! ( cd "$ios_proj" && run xcodebuild \
            -project build/xcode/Player.xcodeproj -scheme Player \
            -configuration Debug -destination "generic/platform=iOS Simulator" \
            build ); then
        fail_check "cold-launch" "xcodebuild Player failed"
        return
    fi
    local app_path
    app_path=$(find "$ios_proj/build" -name "Player.app" -path "*iphonesimulator*" 2>/dev/null | head -1)
    [[ -n "$app_path" ]] || { fail_check "cold-launch" "Player.app not found in tools/ios/build"; return; }

    ( cd "$APP_DIR" && run make ) || { fail_check "cold-launch" "build (server) failed"; return; }
    ensure_ged
    SERVER_ARGS="--brokered"
    start_server

    cold_launch_ios_sim "$PLAYER_BUNDLE_ID" "$app_path" "$FORM_FACTOR" \
        "-stream_addr localhost:$STREAM_PORT" || { stop_server; return; }
    check_startup_flash_ios "$SIM_UDID" "$PLAYER_BUNDLE_ID" || true
    check_soak_ios_sim "$SIM_UDID" "$PLAYER_BUNDLE_ID" || true
    check_rotation_ios_sim "$SIM_UDID" "$PLAYER_BUNDLE_ID" || true
    check_bgfg_ios "$SIM_UDID" "$PLAYER_BUNDLE_ID" || true
    check_reconnect_ios_sim_player "$SERVER_PID" "$SIM_UDID" || true
    SERVER_PID=""
    check_clean_exit_ios_sim "$SIM_UDID" "$PLAYER_BUNDLE_ID"
    # Post-run device power state probe (🎯T33.4): verify the device stayed awake.
    # On a sim, spyder returns 'unknown' (no tunneld) — treated as no-signal (warn only).
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$SIM_UDID" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_ios_device_dist() {
    require_cmd xcrun "xcrun not found — Xcode not installed"
    [[ -d "$APP_DIR/ios" ]] || { fail_check "cold-launch" "no ios/ project (run make ge/ios-init)"; return; }
    build_ios_device_release || { fail_check "cold-launch" "make ge/ios-device-release failed"; return; }
    local app_path
    app_path=$(find_ios_device_app_release)
    [[ -n "$app_path" ]] || { fail_check "cold-launch" ".app not found in ios/build/Release-iphoneos"; return; }

    cold_launch_ios_device "$APP_ID" "$app_path" "$FORM_FACTOR" || return
    check_soak_ios_device "$IOS_DEVICE_UDID" "$APP_ID" || true
    check_bgfg_ios_device "$IOS_DEVICE_UDID" "$APP_ID" || true
    check_clean_exit_ios_device "$IOS_DEVICE_UDID" "$APP_ID"
    # Post-run device power state probe (🎯T33.4): physical iOS devices can auto-lock.
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$IOS_DEVICE_UDID" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_ios_device_player() {
    require_cmd xcrun "xcrun not found — Xcode not installed"
    build_ios_device_player || { fail_check "cold-launch" "make ge/player-ios-device failed"; return; }
    local app_path
    app_path=$(find_ios_device_player_app)
    [[ -n "$app_path" ]] || { fail_check "cold-launch" "Player.app not found in tools/ios/build/*iphoneos*"; return; }

    ( cd "$APP_DIR" && run make ) || { fail_check "cold-launch" "build (server) failed"; return; }
    ensure_ged
    SERVER_ARGS="--brokered"
    start_server

    local host_ip
    host_ip=$(host_lan_ip)
    if [[ -z "$host_ip" ]]; then
        stop_server
        fail_check "cold-launch" "could not determine host LAN IP for device→stream relay connection"
        return
    fi

    cold_launch_ios_device "$PLAYER_BUNDLE_ID" "$app_path" "$FORM_FACTOR" \
        "-stream_addr ${host_ip}:${STREAM_PORT}" || { stop_server; return; }
    check_soak_ios_device "$IOS_DEVICE_UDID" "$PLAYER_BUNDLE_ID" || true
    check_bgfg_ios_device "$IOS_DEVICE_UDID" "$PLAYER_BUNDLE_ID" || true
    warn_check "reconnect" "skipped on physical device (NSUserDefaults ged_addr cleared after first use)"
    stop_server
    SERVER_PID=""
    check_clean_exit_ios_device "$IOS_DEVICE_UDID" "$PLAYER_BUNDLE_ID"
    # Post-run device power state probe (🎯T33.4): verify the physical device stayed awake.
    # Physical iOS devices can auto-lock during long sessions; this catches that case.
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$IOS_DEVICE_UDID" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_android_emu_dist() {
    require_cmd adb "adb not found — install Android SDK platform-tools"
    [[ -d "$APP_DIR/android" ]] || { fail_check "cold-launch" "no android/ project (run make ge/android-init)"; return; }
    build_android_release || { fail_check "cold-launch" "make ge/android-release failed"; return; }
    local apk
    apk=$(find_android_apk_release)
    [[ -f "$apk" ]] || { fail_check "cold-launch" "release APK not found: $apk"; return; }
    local activity
    activity=$(android_get_activity "$APP_ID")

    cold_launch_android_emu "$APP_ID" "$apk" "$FORM_FACTOR" "$activity" || return
    check_startup_flash_android "$ANDROID_SERIAL" "$APP_ID" "$activity" || true
    check_soak_android "$ANDROID_SERIAL" "$APP_ID" || true
    check_rotation_android_emu "$ANDROID_SERIAL" "$APP_ID" || true
    check_bgfg_android "$ANDROID_SERIAL" "$APP_ID" "$activity" || true
    # Visual regression: capture at known-stable state (post-soak, post-bg/fg, pre-exit).
    # Uses spyder screenshot + diff against baseline in ~/.spyder/visualdiff/ge-tiltbuggy/<cell>/...
    # On mismatch, exits with code 51 (visual-regression-mismatch).
    # Run 'make update-baselines' in the app directory to refresh baselines.
    visual_regression_check "$ANDROID_SERIAL" "$CELL" || exit 51
    check_clean_exit_android "$ANDROID_SERIAL" "$APP_ID"
    # Post-run device power state probe (🎯T33.4 / 🎯T28.4 AccelSynth cell).
    # Emulators don't auto-lock; spyder returns 'unknown' — treated as no-signal.
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$ANDROID_SERIAL" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_android_emu_player() {
    require_cmd adb "adb not found — install Android SDK platform-tools"
    echo "  … building ge Android player …"
    local android_player_dir="$GE_ROOT/tools/android"
    if ! ( cd "$android_player_dir" && run ./gradlew assembleDebug ); then
        fail_check "cold-launch" "gradle assembleDebug failed"
        return
    fi
    local apk="$android_player_dir/app/build/outputs/apk/debug/app-debug.apk"
    [[ -f "$apk" ]] || { fail_check "cold-launch" "player APK not found: $apk"; return; }

    ( cd "$APP_DIR" && run make ) || { fail_check "cold-launch" "build (server) failed"; return; }
    ensure_ged
    SERVER_ARGS="--brokered"
    start_server

    cold_launch_android_emu "$PLAYER_ANDROID_PKG" "$apk" "$FORM_FACTOR" "$PLAYER_ANDROID_ACTIVITY" \
        "--es stream_addr 10.0.2.2:$STREAM_PORT" || { stop_server; return; }
    check_startup_flash_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" "$PLAYER_ANDROID_ACTIVITY" \
        "--es stream_addr 10.0.2.2:$STREAM_PORT" || true
    check_soak_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" || true
    check_rotation_android_emu "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" || true
    check_bgfg_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" "$PLAYER_ANDROID_ACTIVITY" || true
    check_reconnect_android_player "$SERVER_PID" "$ANDROID_SERIAL" || true
    SERVER_PID=""
    check_clean_exit_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG"
    # Post-run device power state probe (🎯T33.4): verify the emulator stayed awake.
    # Android emulators don't auto-lock but spyder returns 'unknown' — treated as no-signal.
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$ANDROID_SERIAL" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_android_device_dist() {
    require_cmd adb "adb not found — install Android SDK platform-tools"
    [[ -d "$APP_DIR/android" ]] || { fail_check "cold-launch" "no android/ project"; return; }
    build_android || { fail_check "cold-launch" "make ge/android failed"; return; }
    local apk
    apk=$(find_android_apk)
    [[ -f "$apk" ]] || { fail_check "cold-launch" "APK not found: $apk"; return; }
    local activity
    activity=$(android_get_activity "$APP_ID")

    cold_launch_android_device "$APP_ID" "$apk" "$FORM_FACTOR" "$activity" || return
    check_startup_flash_android "$ANDROID_SERIAL" "$APP_ID" "$activity" || true
    check_soak_android "$ANDROID_SERIAL" "$APP_ID" || true
    check_bgfg_android "$ANDROID_SERIAL" "$APP_ID" "$activity" || true
    check_clean_exit_android "$ANDROID_SERIAL" "$APP_ID"
    # Post-run device power state probe (🎯T33.4): physical Android devices can auto-lock.
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$ANDROID_SERIAL" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_android_device_player() {
    require_cmd adb "adb not found — install Android SDK platform-tools"
    local android_player_dir="$GE_ROOT/tools/android"
    echo "  … building ge Android player …"
    if ! ( cd "$android_player_dir" && run ./gradlew assembleDebug ); then
        fail_check "cold-launch" "gradle assembleDebug failed"
        return
    fi
    local apk="$android_player_dir/app/build/outputs/apk/debug/app-debug.apk"
    [[ -f "$apk" ]] || { fail_check "cold-launch" "player APK not found: $apk"; return; }

    ( cd "$APP_DIR" && run make ) || { fail_check "cold-launch" "build (server) failed"; return; }
    ensure_ged
    SERVER_ARGS="--brokered"
    start_server

    local host_ip
    host_ip=$(host_lan_ip)
    if [[ -z "$host_ip" ]]; then
        stop_server
        fail_check "cold-launch" "could not determine host LAN IP for device→stream relay connection"
        return
    fi
    cold_launch_android_device "$PLAYER_ANDROID_PKG" "$apk" "$FORM_FACTOR" "$PLAYER_ANDROID_ACTIVITY" \
        "--es stream_addr ${host_ip}:${STREAM_PORT}" || { stop_server; return; }
    check_startup_flash_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" "$PLAYER_ANDROID_ACTIVITY" \
        "--es stream_addr ${host_ip}:${STREAM_PORT}" || true
    check_soak_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" || true
    check_bgfg_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" "$PLAYER_ANDROID_ACTIVITY" || true
    warn_check "reconnect" "skipped on physical device (intent extra consumed at launch)"
    stop_server
    SERVER_PID=""
    check_clean_exit_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG"
    # Post-run device power state probe (🎯T33.4): physical Android devices can auto-lock.
    if [[ ${#SUBCHECKS_FAILED[@]} -eq 0 ]]; then
        check_device_awake "$ANDROID_SERIAL" || exit $DEVICE_ASLEEP_EXIT
    fi
}

run_debug_dist() {
    case "$PLATFORM" in
        desktop)
            build_desktop_debug || { fail_check "cold-launch" "debug build failed"; return; }
            DESKTOP_BIN="$APP_DIR/bin/$APP_NAME-debug"
            cold_launch_desktop || return
            check_soak_desktop "$LAUNCH_PID" || true
            check_clean_exit_desktop "$LAUNCH_PID"
            LAUNCH_PID=""
            DESKTOP_BIN=""
            ;;
        ios)
            require_cmd xcrun "xcrun not found — Xcode not installed"
            [[ -d "$APP_DIR/ios" ]] || { fail_check "cold-launch" "no ios/ project"; return; }
            build_ios_sim_debug || { fail_check "cold-launch" "make ge/ios failed"; return; }
            local app_path
            app_path=$(find_ios_sim_app_debug)
            [[ -n "$app_path" ]] || { fail_check "cold-launch" "debug .app not found in ios/build/Debug-iphonesimulator"; return; }
            cold_launch_ios_sim "$APP_ID" "$app_path" "phone" || return
            check_soak_ios_sim "$SIM_UDID" "$APP_ID" || true
            check_clean_exit_ios_sim "$SIM_UDID" "$APP_ID"
            ;;
        android)
            require_cmd adb "adb not found"
            [[ -d "$APP_DIR/android" ]] || { fail_check "cold-launch" "no android/ project"; return; }
            build_android_debug || { fail_check "cold-launch" "make ge/android failed"; return; }
            local apk
            apk=$(find_android_apk_debug)
            [[ -f "$apk" ]] || { fail_check "cold-launch" "debug APK not found: $apk"; return; }
            local activity
            activity=$(android_get_activity "$APP_ID")
            cold_launch_android_emu "$APP_ID" "$apk" "" "$activity" || return
            check_soak_android "$ANDROID_SERIAL" "$APP_ID" || true
            check_clean_exit_android "$ANDROID_SERIAL" "$APP_ID"
            ;;
    esac
}

run_debug_player() {
    case "$PLATFORM" in
        desktop)
            build_player_desktop_debug || { fail_check "cold-launch" "debug build failed"; return; }
            DESKTOP_BIN="$APP_DIR/bin/$APP_NAME-debug"
            ensure_ged
            SERVER_ARGS="--brokered"
            start_server
            cold_launch_player_desktop || { stop_server; DESKTOP_BIN=""; return; }
            check_soak_desktop "$LAUNCH_PID" || true
            check_clean_exit_desktop "$LAUNCH_PID"
            LAUNCH_PID=""
            DESKTOP_BIN=""
            ;;
        ios)
            require_cmd xcrun "xcrun not found — Xcode not installed"
            local ios_proj="$GE_ROOT/tools/ios"
            if [[ ! -d "$ios_proj/build/xcode" ]]; then
                ( cd "$APP_DIR" && run make ge/player-ios ) \
                    || { fail_check "cold-launch" "make ge/player-ios failed"; return; }
            fi
            if ! ( cd "$ios_proj" && run xcodebuild \
                    -project build/xcode/Player.xcodeproj -scheme Player \
                    -configuration Debug -destination "generic/platform=iOS Simulator" \
                    build ); then
                fail_check "cold-launch" "xcodebuild Player (Debug) failed"
                return
            fi
            local app_path
            app_path=$(find "$ios_proj/build" -name "Player.app" \
                -path "*Debug-iphonesimulator*" 2>/dev/null | head -1)
            [[ -n "$app_path" ]] || { fail_check "cold-launch" "Player.app not found in tools/ios/build/Debug-iphonesimulator"; return; }
            build_desktop_debug || { fail_check "cold-launch" "debug server build failed"; return; }
            DESKTOP_BIN="$APP_DIR/bin/$APP_NAME-debug"
            ensure_ged
            SERVER_ARGS="--brokered"
            start_server
            cold_launch_ios_sim "$PLAYER_BUNDLE_ID" "$app_path" "phone" \
                "-stream_addr localhost:$STREAM_PORT" || { stop_server; DESKTOP_BIN=""; return; }
            check_soak_ios_sim "$SIM_UDID" "$PLAYER_BUNDLE_ID" || true
            check_clean_exit_ios_sim "$SIM_UDID" "$PLAYER_BUNDLE_ID"
            DESKTOP_BIN=""
            ;;
        android)
            require_cmd adb "adb not found"
            local android_player_dir="$GE_ROOT/tools/android"
            if ! ( cd "$android_player_dir" && run ./gradlew assembleDebug ); then
                fail_check "cold-launch" "gradle assembleDebug (player) failed"
                return
            fi
            local apk="$android_player_dir/app/build/outputs/apk/debug/app-debug.apk"
            [[ -f "$apk" ]] || { fail_check "cold-launch" "player debug APK not found"; return; }
            build_desktop_debug || { fail_check "cold-launch" "debug server build failed"; return; }
            DESKTOP_BIN="$APP_DIR/bin/$APP_NAME-debug"
            ensure_ged
            SERVER_ARGS="--brokered"
            start_server
            cold_launch_android_emu "$PLAYER_ANDROID_PKG" "$apk" "" "$PLAYER_ANDROID_ACTIVITY" \
                "--es stream_addr 10.0.2.2:$STREAM_PORT" || { stop_server; DESKTOP_BIN=""; return; }
            check_soak_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG" || true
            check_clean_exit_android "$ANDROID_SERIAL" "$PLAYER_ANDROID_PKG"
            DESKTOP_BIN=""
            ;;
    esac
}

# ── Pre-flight: imgdiff ──────────────────────────────────────────────

if [[ ! -x "$APP_DIR/bin/imgdiff" ]]; then
    echo "  … building imgdiff …"
    ( cd "$APP_DIR" && run make ge/imgdiff ) \
        || { echo "FAIL [$CELL]: failed to build imgdiff" >&2; exit 2; }
fi

# ── Dispatch ─────────────────────────────────────────────────────────

case "$CELL" in
    desktop-dist)              run_desktop_dist ;;
    desktop-player)            run_desktop_player ;;
    ios-sim-phone-dist)        run_ios_sim_dist ;;
    ios-sim-phone-player)      run_ios_sim_player ;;
    ios-sim-tablet-dist)       run_ios_sim_dist ;;
    ios-sim-tablet-player)     run_ios_sim_player ;;
    ios-device-phone-dist)     run_ios_device_dist ;;
    ios-device-phone-player)   run_ios_device_player ;;
    ios-device-tablet-dist)    run_ios_device_dist ;;
    ios-device-tablet-player)  run_ios_device_player ;;
    android-emu-phone-dist)    run_android_emu_dist ;;
    android-emu-phone-player)  run_android_emu_player ;;
    android-emu-tablet-dist)   run_android_emu_dist ;;
    android-emu-tablet-player) run_android_emu_player ;;
    android-device-phone-dist)    run_android_device_dist ;;
    android-device-phone-player)  run_android_device_player ;;
    android-device-tablet-dist)   run_android_device_dist ;;
    android-device-tablet-player) run_android_device_player ;;
    desktop-debug-dist)        run_debug_dist ;;
    desktop-debug-player)      run_debug_player ;;
    ios-debug-dist)            run_debug_dist ;;
    ios-debug-player)          run_debug_player ;;
    android-debug-dist)        run_debug_dist ;;
    android-debug-player)      run_debug_player ;;
esac

# ── Final summary ────────────────────────────────────────────────────
# Exit-code legend:
#   0  — all sub-checks passed (including device_power_state if applicable)
#   1  — one or more sub-checks failed
#  50  — all other sub-checks passed but device_power_state was display_off/asleep;
#         soak result is unreliable. Distinct from 1 so CI can report it separately.

echo
pass_count=${#SUBCHECKS_PASSED[@]}
fail_count=${#SUBCHECKS_FAILED[@]}
total=$((pass_count + fail_count))

# Detect whether the only failure is the device-power-state sub-check, which
# means the cell logic passed but the device fell asleep during the soak.
device_asleep=0
if [[ $fail_count -eq 1 && "${SUBCHECKS_FAILED[*]}" == "device-power-state" ]]; then
    device_asleep=1
fi

echo "── $CELL: $pass_count/$total passed ──"
if [[ $fail_count -gt 0 ]]; then
    if [[ $device_asleep -eq 1 ]]; then
        echo "  Failed: ${SUBCHECKS_FAILED[*]}  (device fell asleep — soak unreliable; exit $DEVICE_ASLEEP_EXIT)"
    else
        echo "  Failed: ${SUBCHECKS_FAILED[*]}"
    fi
    echo "  Artifacts: $ARTIFACTS"
    if [[ $device_asleep -eq 1 ]]; then
        exit "$DEVICE_ASLEEP_EXIT"
    fi
    exit 1
fi
echo "  Artifacts: $ARTIFACTS"
exit 0

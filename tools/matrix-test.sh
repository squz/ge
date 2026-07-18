#!/usr/bin/env bash
#
# matrix-test.sh — automated validation of a ge app across
# {desktop, iOS-simulator, Android-emulator} × {distribution, brokered-player}.
#
# Physical devices (iOS device, Android device) are intentionally out of
# scope: they require human-in-the-loop steps (USB trust prompts, device
# tilting) that can't be reliably automated.
#
# Usage:
#   ge/tools/matrix-test.sh [options]
#
# Run this from the app root (e.g. sample/tiltbuggy/). The script finds
# ge at the directory containing this script's parent (../) and uses the
# app's own Makefile to build.
#
# Options:
#   --cells <a,b,...>   Run only named cells (default: all automatable)
#   --capture-refs      Save screenshots as reference images (overwrites)
#   --timeout <secs>    Per-cell soak time (default: 5)
#   --rms <f>           Image-diff RMS threshold (default: 0.08)
#   --app <path>        App root (default: current directory)
#   --app-name <name>   App binary name (default: derived from --app basename)
#   --app-id <id>       iOS bundle-id / Android package (for *-dist cells)
#   --server-name <n>   Server name registered with stream relay (default: app-name)
#   --stream-port <port>   stream-relay port (spyder) (default: 3030)
#   --no-stream relay            Assume stream relay is already running; don't start/stop it
#   --verbose           Echo sub-command stdout/stderr
#
# Cells (all IDs):
#   desktop-dist         Desktop distribution build
#   desktop-player       Desktop player + desktop server (brokered)
#   ios-sim-dist         iOS simulator distribution
#   ios-sim-player       iOS simulator player + desktop server
#   android-emu-dist     Android emulator distribution
#   android-emu-player   Android emulator player + desktop server
#
# Reference images are stored at: <app>/test/refs/<cell>-untilted.png
# Capture new refs with --capture-refs after a manual visual check.

set -euo pipefail

# ── Locate self / ge ────────────────────────────────────────────────

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GE_ROOT="$(cd "$SCRIPT_PATH/.." && pwd)"

# ── Defaults / args ─────────────────────────────────────────────────

CELLS=""
CAPTURE_REFS=0
TIMEOUT=5
RMS=0.08
APP_DIR="$(pwd)"
APP_NAME=""
APP_ID=""
SERVER_NAME=""
STREAM_PORT=3030
NO_GED=0
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cells)         CELLS="$2"; shift 2 ;;
        --capture-refs)  CAPTURE_REFS=1; shift ;;
        --timeout)       TIMEOUT="$2"; shift 2 ;;
        --rms)           RMS="$2"; shift 2 ;;
        --app)           APP_DIR="$(cd "$2" && pwd)"; shift 2 ;;
        --app-name)      APP_NAME="$2"; shift 2 ;;
        --app-id)        APP_ID="$2"; shift 2 ;;
        --server-name)   SERVER_NAME="$2"; shift 2 ;;
        --stream-port)      STREAM_PORT="$2"; shift 2 ;;
        --no-ged|--no-stream-relay)        NO_GED=1; shift ;;
        --verbose)       VERBOSE=1; shift ;;
        -h|--help)
            sed -n '2,/^$/{ s/^# \{0,1\}//; p; }' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

[[ -z "$APP_NAME" ]]    && APP_NAME="$(basename "$APP_DIR")"
[[ -z "$SERVER_NAME" ]] && SERVER_NAME="$APP_NAME"

# Auto-detect APP_ID from the app's Makefile if not passed.
if [[ -z "$APP_ID" && -f "$APP_DIR/Makefile" ]]; then
    APP_ID=$(sed -nE 's/^[[:space:]]*APP_ID[[:space:]]*:?=[[:space:]]*([^[:space:]#]+).*/\1/p' \
        "$APP_DIR/Makefile" | head -1)
fi

# Default cell list (all automatable).
ALL_CELLS="desktop-dist desktop-player ios-sim-dist ios-sim-player android-emu-dist android-emu-player"
if [[ -z "$CELLS" ]]; then
    SELECTED_CELLS="$ALL_CELLS"
else
    SELECTED_CELLS="$(echo "$CELLS" | tr ',' ' ')"
fi

# ── Working state ───────────────────────────────────────────────────

TMPDIR_ROOT="$(mktemp -d -t ge-matrix-XXXXXX)"
REFS_DIR="$APP_DIR/test/refs"
mkdir -p "$REFS_DIR"

# Per-cell status strings, accumulated as `<cell>|<status>|<notes>`.
STATUSES=()

run() {
    if [[ $VERBOSE -eq 1 ]]; then
        echo "+ $*" >&2
        "$@"
    else
        "$@" >/dev/null 2>&1
    fi
}

run_out() {
    # Run with stdout captured, stderr to /dev/null (or shown in verbose).
    if [[ $VERBOSE -eq 1 ]]; then
        echo "+ $*" >&2
        "$@"
    else
        "$@" 2>/dev/null
    fi
}

record() {
    # record <cell> <status> [notes]
    local cell="$1"; local status="$2"; local notes="${3:-}"
    STATUSES+=("$cell|$status|$notes")
    local symbol
    case "$status" in
        PASS) symbol="✓" ;;
        FAIL) symbol="✗" ;;
        SKIP) symbol="—" ;;
        *)    symbol="?" ;;
    esac
    printf "  [%s] %-22s %s%s\n" "$symbol" "$cell" "$status" \
        "${notes:+ ($notes)}"
}

# ── stream relay lifecycle ───────────────────────────────────────────────────

# Stream relay is spyder (external). Matrix never starts a local broker.
ensure_stream_relay() {
    if curl -sf --max-time 2 "http://127.0.0.1:$STREAM_PORT/stream/servers" >/dev/null 2>&1; then
        return 0
    fi
    echo "spyder stream relay not reachable at :$STREAM_PORT — start with: brew services start spyder  (or spyder serve --addr 127.0.0.1:$STREAM_PORT)" >&2
    exit 2
}
# Back-compat alias for call sites not yet renamed.
ensure_ged() { ensure_stream_relay; }

cleanup_ged() { :; }  # spyder is external — do not kill
trap 'cleanup_ged' EXIT

# ── Helpers: process lifecycle + screenshot ─────────────────────────

kill_bg() {
    local pid="${1:-}"
    [[ -n "$pid" ]] || return 0
    kill "$pid" 2>/dev/null || true
    # Give it a moment to clean up.
    local tries=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.2
        tries=$((tries + 1))
        if [[ $tries -gt 10 ]]; then
            kill -9 "$pid" 2>/dev/null || true
            break
        fi
    done
    wait "$pid" 2>/dev/null || true
    return 0
}

current_sessions() {
    curl -sf --max-time 1 "http://127.0.0.1:$STREAM_PORT/stream/servers" 2>/dev/null \
        | jq -r '[(.servers // [])[].sessions // 0] | add // 0' || echo 0
}

# wait_for_sessions <min-absolute>
wait_for_sessions() {
    local min="$1"
    local deadline=$(($(date +%s) + TIMEOUT))
    while [[ $(date +%s) -lt $deadline ]]; do
        local sess=$(current_sessions)
        if [[ "$sess" -ge "$min" ]]; then return 0; fi
        sleep 0.5
    done
    return 1
}

# Screenshot the largest on-screen window owned by $1 (pid) into $2.
# Falls back to fullscreen if the PID owns no visible window.
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
    if [[ -z "$wid" ]]; then
        screencapture -x "$out"
        return
    fi
    screencapture -x -o -l "$wid" "$out"
}

# ── Cell: desktop-dist ───────────────────────────────────────────────

cell_desktop_dist() {
    local cell="desktop-dist"
    echo "── $cell ──"

    ( cd "$APP_DIR" && run make ) || { record "$cell" FAIL "build"; return; }
    local bin="$APP_DIR/bin/$APP_NAME"
    [[ -x "$bin" ]] || { record "$cell" FAIL "no binary"; return; }

    local logf="$TMPDIR_ROOT/$cell.log"
    "$bin" > "$logf" 2>&1 &
    local pid=$!
    sleep "$TIMEOUT"

    if ! kill -0 "$pid" 2>/dev/null; then
        record "$cell" FAIL "crashed (log: $logf)"
        return
    fi

    local shot="$TMPDIR_ROOT/$cell-untilted.png"
    shot_window_by_pid "$pid" "$shot"
    kill_bg "$pid"

    if [[ ! -s "$shot" ]]; then
        record "$cell" FAIL "no screenshot"
        return
    fi

    local ref="$REFS_DIR/$cell-untilted.png"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        record "$cell" PASS "ref captured"
        return
    fi
    if [[ ! -f "$ref" ]]; then
        record "$cell" SKIP "no reference (run with --capture-refs)"
        return
    fi
    local rms
    if rms=$("$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null); then
        record "$cell" PASS "rms=$rms"
    else
        record "$cell" FAIL "rms=$rms > $RMS"
    fi
}

# ── Cell: desktop-player ─────────────────────────────────────────────

cell_desktop_player() {
    local cell="desktop-player"
    echo "── $cell ──"

    ( cd "$APP_DIR" && run make ) || { record "$cell" FAIL "build (server)"; return; }
    ( cd "$APP_DIR" && run make ge/player ) \
        || { record "$cell" FAIL "build (player)"; return; }

    local server_bin="$APP_DIR/bin/$APP_NAME"
    local player_bin="$APP_DIR/bin/player"
    [[ -x "$server_bin" && -x "$player_bin" ]] \
        || { record "$cell" FAIL "no binaries"; return; }

    local serverlog="$TMPDIR_ROOT/$cell-server.log"
    local playerlog="$TMPDIR_ROOT/$cell-player.log"
    local baseline=$(current_sessions)
    ( cd "$APP_DIR" && exec "$server_bin" --brokered > "$serverlog" 2>&1 ) &
    local srv_pid=$!
    sleep 2
    ( cd "$APP_DIR" && exec "$player_bin" --name "$SERVER_NAME" > "$playerlog" 2>&1 ) &
    local ply_pid=$!

    local ok=1
    if ! wait_for_sessions $((baseline + 1)); then
        record "$cell" FAIL "no new session (server log: $serverlog)"
        kill_bg "$ply_pid"; kill_bg "$srv_pid"
        return
    fi
    sleep "$TIMEOUT"

    # Verify frames decoded.
    if ! grep -qE 'decoder initialized with SPS/PPS|PlayerLog.*decoded' "$playerlog"; then
        ok=0
    fi

    local shot="$TMPDIR_ROOT/$cell-untilted.png"
    shot_window_by_pid "$ply_pid" "$shot"
    kill_bg "$ply_pid"; kill_bg "$srv_pid"

    if [[ $ok -eq 0 || ! -s "$shot" ]]; then
        record "$cell" FAIL "no frames or screenshot"
        return
    fi

    local ref="$REFS_DIR/$cell-untilted.png"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        record "$cell" PASS "ref captured"
        return
    fi
    if [[ ! -f "$ref" ]]; then
        record "$cell" SKIP "no reference (run with --capture-refs)"
        return
    fi
    local rms
    if rms=$("$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null); then
        record "$cell" PASS "rms=$rms"
    else
        record "$cell" FAIL "rms=$rms > $RMS"
    fi
}

# ── Cell: ios-sim-dist ───────────────────────────────────────────────

cell_ios_sim_dist() {
    local cell="ios-sim-dist"
    echo "── $cell ──"

    if ! command -v xcrun >/dev/null 2>&1; then
        record "$cell" SKIP "no xcrun (Xcode not installed)"
        return
    fi
    if [[ ! -d "$APP_DIR/ios" ]]; then
        record "$cell" SKIP "no ios/ project (run make ge/ios-init)"
        return
    fi
    if ! ensure_ios_sim; then
        record "$cell" SKIP "no iPad simulator available"
        return
    fi

    # Derive expected bundle id from APP_ID var or fall back to project name.
    local bundle_id="${APP_ID:-com.example.$APP_NAME}"

    ( cd "$APP_DIR" && run make ge/ios ) \
        || { record "$cell" FAIL "make ge/ios failed"; return; }

    local app_path
    app_path=$(find "$APP_DIR/ios/build" -name "*.app" -path "*iphonesimulator*" 2>/dev/null | head -1)
    if [[ -z "$app_path" ]]; then
        record "$cell" FAIL ".app not found in ios/build"
        return
    fi

    xcrun simctl terminate booted "$bundle_id" 2>/dev/null || true
    if ! run xcrun simctl install booted "$app_path"; then
        record "$cell" FAIL "simctl install"
        return
    fi
    if ! run xcrun simctl launch booted "$bundle_id"; then
        record "$cell" FAIL "simctl launch"
        return
    fi

    sleep "$TIMEOUT"
    local shot="$TMPDIR_ROOT/$cell-untilted.png"
    xcrun simctl io booted screenshot "$shot" >/dev/null 2>&1
    xcrun simctl terminate booted "$bundle_id" 2>/dev/null || true

    if [[ ! -s "$shot" ]]; then
        record "$cell" FAIL "no screenshot"
        return
    fi

    local ref="$REFS_DIR/$cell-untilted.png"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        record "$cell" PASS "ref captured"
        return
    fi
    if [[ ! -f "$ref" ]]; then
        record "$cell" SKIP "no reference (run with --capture-refs)"
        return
    fi
    local rms
    if rms=$("$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null); then
        record "$cell" PASS "rms=$rms"
    else
        record "$cell" FAIL "rms=$rms > $RMS"
    fi
}

# ── Cell: ios-sim-player ─────────────────────────────────────────────

# Boot a sim if none booted. Returns 0 if a sim is available.
ensure_ios_sim() {
    local count
    count=$(xcrun simctl list devices booted -j 2>/dev/null \
            | jq '[.devices | to_entries[].value[]] | length')
    [[ "$count" -gt 0 ]] && return 0
    # Boot the first "generic" iPad sim we can find.
    local udid
    udid=$(xcrun simctl list devices available -j 2>/dev/null \
           | jq -r '[.devices | to_entries[].value[] | select(.name | test("iPad"; "i"))][0].udid // empty')
    if [[ -n "$udid" ]]; then
        xcrun simctl boot "$udid" 2>/dev/null || true
        open -a Simulator >/dev/null 2>&1 || true
        sleep 5
        return 0
    fi
    return 1
}

cell_ios_sim_player() {
    local cell="ios-sim-player"
    echo "── $cell ──"

    if ! command -v xcrun >/dev/null 2>&1; then
        record "$cell" SKIP "no xcrun (Xcode not installed)"
        return
    fi
    if ! ensure_ios_sim; then
        record "$cell" SKIP "no iPad simulator available"
        return
    fi

    local ios_proj="$GE_ROOT/tools/ios/build/xcode"
    if [[ ! -d "$ios_proj" ]]; then
        ( cd "$APP_DIR" && run make ge/player-ios ) \
            || { record "$cell" FAIL "make ge/player-ios failed"; return; }
    fi
    # Build for simulator.
    if ! ( cd "$GE_ROOT/tools/ios" && run xcodebuild \
           -project build/xcode/Player.xcodeproj -scheme Player \
           -configuration Debug -destination "generic/platform=iOS Simulator" \
           build ); then
        record "$cell" FAIL "xcodebuild failed"
        return
    fi

    # Find the .app
    local app_path
    app_path=$(find "$GE_ROOT/tools/ios/build" -name "Player.app" -path "*iphonesimulator*" 2>/dev/null | head -1)
    if [[ -z "$app_path" ]]; then
        record "$cell" FAIL "Player.app not found"
        return
    fi

    # Start server.
    ( cd "$APP_DIR" && make ) >/dev/null 2>&1 || true
    local server_bin="$APP_DIR/bin/$APP_NAME"
    local serverlog="$TMPDIR_ROOT/$cell-server.log"
    local baseline=$(current_sessions)
    ( cd "$APP_DIR" && exec "$server_bin" --brokered > "$serverlog" 2>&1 ) &
    local srv_pid=$!
    sleep 2

    # Terminate → install → launch.
    xcrun simctl terminate booted com.spyder.player 2>/dev/null || true
    if ! run xcrun simctl install booted "$app_path"; then
        record "$cell" FAIL "simctl install"
        kill_bg "$srv_pid"; return
    fi
    if ! run xcrun simctl launch booted com.spyder.player; then
        record "$cell" FAIL "simctl launch"
        kill_bg "$srv_pid"; return
    fi

    if ! wait_for_sessions $((baseline + 1)); then
        record "$cell" FAIL "no new session after launch"
        xcrun simctl terminate booted com.spyder.player 2>/dev/null || true
        kill_bg "$srv_pid"; return
    fi
    sleep "$TIMEOUT"

    local shot="$TMPDIR_ROOT/$cell-untilted.png"
    xcrun simctl io booted screenshot "$shot" >/dev/null 2>&1

    xcrun simctl terminate booted com.spyder.player 2>/dev/null || true
    kill_bg "$srv_pid"

    if [[ ! -s "$shot" ]]; then
        record "$cell" FAIL "no screenshot"
        return
    fi

    local ref="$REFS_DIR/$cell-untilted.png"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        record "$cell" PASS "ref captured"
        return
    fi
    if [[ ! -f "$ref" ]]; then
        record "$cell" SKIP "no reference (run with --capture-refs)"
        return
    fi
    local rms
    if rms=$("$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null); then
        record "$cell" PASS "rms=$rms"
    else
        record "$cell" FAIL "rms=$rms > $RMS"
    fi
}

# ── Cell: android-emu-dist ───────────────────────────────────────────

cell_android_emu_dist() {
    local cell="android-emu-dist"
    echo "── $cell ──"
    if ! command -v adb >/dev/null 2>&1; then
        record "$cell" SKIP "no adb"
        return
    fi
    if [[ ! -d "$APP_DIR/android" ]]; then
        record "$cell" SKIP "no android/ project (run make ge/android-init)"
        return
    fi
    local online
    online=$(adb devices 2>/dev/null | awk 'NR>1 && /emulator.*device$/ {print $1}' | head -1)
    if [[ -z "$online" ]]; then
        record "$cell" SKIP "no running emulator"
        return
    fi
    local adb_cmd="adb -s $online"
    local pkg="${APP_ID:-com.example.$APP_NAME}"

    ( cd "$APP_DIR" && run make ge/android ) \
        || { record "$cell" FAIL "make ge/android failed"; return; }
    local apk="$APP_DIR/android/app/build/outputs/apk/debug/app-debug.apk"
    [[ -f "$apk" ]] || { record "$cell" FAIL "apk not found"; return; }

    $adb_cmd shell am force-stop "$pkg" 2>/dev/null || true
    run $adb_cmd install -r "$apk" \
        || { record "$cell" FAIL "adb install"; return; }
    # Derive activity name from the generated manifest — it's the
    # ground truth and follows APP_DISPLAY_NAME casing (e.g. TiltBuggy),
    # not the lowercase APP_NAME. Accept either short form ".Cls" or
    # fully-qualified "pkg.sub.Cls"; feed the extracted value through
    # `adb am start -n <pkg>/<value>` unchanstream relay (adb accepts both).
    local manifest="$APP_DIR/android/app/src/main/AndroidManifest.xml"
    local activity_raw=""
    if [[ -f "$manifest" ]]; then
        activity_raw=$(grep -oE 'android:name="[A-Za-z0-9_.]+Activity"' "$manifest" \
            | head -1 | sed -E 's/.*"([^"]+)".*/\1/' || true)
    fi
    [[ -z "$activity_raw" ]] && activity_raw=".${APP_NAME^}Activity"
    local activity="${pkg}/${activity_raw}"
    run $adb_cmd shell am start -n "$activity" \
        || { record "$cell" FAIL "adb am start $activity"; return; }

    sleep "$TIMEOUT"
    local shot="$TMPDIR_ROOT/$cell-untilted.png"
    $adb_cmd exec-out screencap -p > "$shot" 2>/dev/null
    $adb_cmd shell am force-stop "$pkg" 2>/dev/null || true

    if [[ ! -s "$shot" ]]; then
        record "$cell" FAIL "no screenshot"
        return
    fi

    local ref="$REFS_DIR/$cell-untilted.png"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        record "$cell" PASS "ref captured"
        return
    fi
    if [[ ! -f "$ref" ]]; then
        record "$cell" SKIP "no reference (run with --capture-refs)"
        return
    fi
    local rms
    if rms=$("$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null); then
        record "$cell" PASS "rms=$rms"
    else
        record "$cell" FAIL "rms=$rms > $RMS"
    fi
}

# ── Cell: android-emu-player ─────────────────────────────────────────

cell_android_emu_player() {
    local cell="android-emu-player"
    echo "── $cell ──"

    if ! command -v adb >/dev/null 2>&1; then
        record "$cell" SKIP "no adb"
        return
    fi

    # Need at least one online emulator device.
    local online
    online=$(adb devices 2>/dev/null | awk 'NR>1 && /emulator.*device$/ {print $1}' | head -1)
    if [[ -z "$online" ]]; then
        record "$cell" SKIP "no running emulator"
        return
    fi
    local adb_cmd="adb -s $online"

    # Build APK.
    if ! ( cd "$GE_ROOT/tools/android" && run ./gradlew assembleDebug ); then
        record "$cell" FAIL "gradle build"
        return
    fi
    local apk="$GE_ROOT/tools/android/app/build/outputs/apk/debug/app-debug.apk"
    [[ -f "$apk" ]] || { record "$cell" FAIL "apk not found"; return; }

    # Start desktop server.
    ( cd "$APP_DIR" && make ) >/dev/null 2>&1 || true
    local server_bin="$APP_DIR/bin/$APP_NAME"
    local serverlog="$TMPDIR_ROOT/$cell-server.log"
    local baseline=$(current_sessions)
    ( cd "$APP_DIR" && exec "$server_bin" --brokered > "$serverlog" 2>&1 ) &
    local srv_pid=$!
    sleep 2

    $adb_cmd shell am force-stop com.spyder.player 2>/dev/null || true
    run $adb_cmd install -r "$apk" \
        || { record "$cell" FAIL "adb install"; kill_bg "$srv_pid"; return; }
    run $adb_cmd shell am start -n com.spyder.player/.PlayerActivity \
        || { record "$cell" FAIL "adb am start"; kill_bg "$srv_pid"; return; }

    if ! wait_for_sessions $((baseline + 1)); then
        record "$cell" FAIL "no new session after launch"
        $adb_cmd shell am force-stop com.spyder.player 2>/dev/null || true
        kill_bg "$srv_pid"; return
    fi
    sleep "$TIMEOUT"

    local shot="$TMPDIR_ROOT/$cell-untilted.png"
    $adb_cmd exec-out screencap -p > "$shot" 2>/dev/null

    $adb_cmd shell am force-stop com.spyder.player 2>/dev/null || true
    kill_bg "$srv_pid"

    if [[ ! -s "$shot" ]]; then
        record "$cell" FAIL "no screenshot"
        return
    fi

    local ref="$REFS_DIR/$cell-untilted.png"
    if [[ $CAPTURE_REFS -eq 1 ]]; then
        cp "$shot" "$ref"
        record "$cell" PASS "ref captured"
        return
    fi
    if [[ ! -f "$ref" ]]; then
        record "$cell" SKIP "no reference (run with --capture-refs)"
        return
    fi
    local rms
    if rms=$("$APP_DIR/bin/imgdiff" "$ref" "$shot" --threshold="$RMS" 2>/dev/null); then
        record "$cell" PASS "rms=$rms"
    else
        record "$cell" FAIL "rms=$rms > $RMS"
    fi
}

# ── Dispatcher ───────────────────────────────────────────────────────

echo "matrix-test — app=$APP_NAME dir=$APP_DIR ge=$GE_ROOT"
echo "cells: $SELECTED_CELLS"
echo

# Pre-req: imgdiff CLI.
if [[ ! -x "$APP_DIR/bin/imgdiff" ]]; then
    echo "Building imgdiff..."
    ( cd "$APP_DIR" && run make ge/imgdiff ) \
        || { echo "Failed to build imgdiff" >&2; exit 2; }
fi

# Start stream relay (if brokered cells selected).
if echo " $SELECTED_CELLS " | grep -qE 'player'; then
    ensure_ged
fi

for cell in $SELECTED_CELLS; do
    case "$cell" in
        desktop-dist)        cell_desktop_dist ;;
        desktop-player)      cell_desktop_player ;;
        ios-sim-dist)        cell_ios_sim_dist ;;
        ios-sim-player)      cell_ios_sim_player ;;
        android-emu-dist)    cell_android_emu_dist ;;
        android-emu-player)  cell_android_emu_player ;;
        *) echo "Unknown cell: $cell" >&2; exit 2 ;;
    esac
done

# ── Summary ──────────────────────────────────────────────────────────

echo
echo "── Summary ──"
any_fail=0
for entry in "${STATUSES[@]}"; do
    IFS='|' read -r cell status notes <<< "$entry"
    local_symbol=""
    case "$status" in
        PASS) local_symbol="✓" ;;
        FAIL) local_symbol="✗"; any_fail=1 ;;
        SKIP) local_symbol="—" ;;
    esac
    printf "  %s  %-22s %-4s  %s\n" "$local_symbol" "$cell" "$status" "$notes"
done
echo
echo "Artifacts: $TMPDIR_ROOT"

exit $any_fail

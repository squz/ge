#!/usr/bin/env bash
#
# Smoke test for ge mobile/desktop deployments.
#
# Verifies that the full stack is healthy before asking a human to check
# visual output. Designed to be called by Claude Code after building and
# deploying to a device/simulator. Uses the spyder CLI for all device-side
# operations (discovery, install, launch, log retrieval).
#
# Usage:
#   ge/tools/smoke-test.sh [options]
#
# Options:
#   --device <selector>  Target device. Accepts any spyder selector:
#                          - An inventory alias:  Pippa
#                          - A selector predicate: platform=ios-sim,model=ipad
#                          - A raw UDID/serial
#                        Omit to use the sole connected device (errors if
#                        multiple are present).
#   --install <path>    Build artifact to deploy before testing. Performs an
#                       atomic terminate → install → launch → verify-pid via
#                       `spyder deploy`. Without --install, the script checks
#                       passively (is the app installed? is it running?).
#                         iOS:     path to .app bundle
#                         Android: path to .apk file
#   --bundle-id <id>    App bundle identifier / package name
#                       (default: com.squz.yourworld2)
#   --stream-port <port>   stream-relay port (spyder serve; default: 3030)
#   --timeout <secs>    Max seconds to wait for player connection (default: 15)
#   --server-pid <pid>  Game server PID to verify is running
#   --platform <p>      Retained for backwards compatibility; ignored. Device
#                       discovery is now handled by spyder regardless of
#                       platform type.
#
# Exit codes:
#   0  All checks passed
#   1  One or more checks failed (details in output)
#
# Spyder exit codes used for branching:
#   10  daemon-unreachable
#   11  device-not-found
#   12  device-not-connected
#   20  app-not-installed
#   21  install-failed
#   22  launch-failed
#   24  pid-verification-failed
#   30  timeout
#   40  trust-not-granted
#   41  developer-mode-disabled
#   42  device-locked

set -euo pipefail

# Defaults
DEVICE=""
INSTALL=""
BUNDLE_ID="com.squz.yourworld2"
STREAM_PORT=3030
TIMEOUT=15
SERVER_PID=""
PLATFORM=""  # accepted but ignored; kept for backwards compat

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)      DEVICE="$2"; shift 2 ;;
        --install)     INSTALL="$2"; shift 2 ;;
        --bundle-id)   BUNDLE_ID="$2"; shift 2 ;;
        --package)     BUNDLE_ID="$2"; shift 2 ;;  # Android alias
        --stream-port)    STREAM_PORT="$2"; shift 2 ;;
        --timeout)     TIMEOUT="$2"; shift 2 ;;
        --server-pid)  SERVER_PID="$2"; shift 2 ;;
        --platform)    PLATFORM="$2"; shift 2 ;;  # accepted, ignored
        -h|--help)
            sed -n '2,/^$/{ s/^# \{0,1\}//; p; }' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Output helpers ──────────────────────────────────────────────────

PASS=0
FAIL=0
WARN=0

pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
warn() { echo "  WARN  $1"; WARN=$((WARN + 1)); }
info() { echo "  ....  $1"; }

# ── Spyder availability check ───────────────────────────────────────

if ! command -v spyder &>/dev/null; then
    echo "Error: spyder CLI not found in PATH"
    echo "Install spyder from https://github.com/marcelocantos/spyder"
    exit 1
fi

# ── Check: stream relay reachable ────────────────────────────────────────────

check_stream_relay() {
    echo "── stream relay / spyder (port $STREAM_PORT) ──"

    # Is anything listening on the port?
    if ! lsof -iTCP:"$STREAM_PORT" -sTCP:LISTEN -P -n >/dev/null 2>&1; then
        fail "Nothing listening on port $STREAM_PORT — is spyder serve running?"
        return
    fi
    pass "Port $STREAM_PORT is listening"

    # Spyder stream relay: GET /stream/servers (JSON list of connected servers).
    local response
    if ! response=$(curl -sf --max-time 3 "http://127.0.0.1:$STREAM_PORT/stream/servers" 2>/dev/null); then
        fail "spyder /stream/servers unreachable (curl failed) — is this spyder, not something else?"
        return
    fi
    pass "spyder /stream/servers responded"

    local count
    count=$(echo "$response" | jq -r '(.servers // []) | length' 2>/dev/null || echo 0)

    if [[ "$count" -gt 0 ]]; then
        pass "Game server(s) registered with stream relay ($count)"
    else
        fail "No game server registered with stream relay (empty /stream/servers)"
    fi

    info "Servers:"
    echo "$response" | jq -r '
        (.servers // [])[]? |
        "  ....  Server: \(.name // "unknown") sessions=\(.sessions // 0)"
    ' 2>/dev/null || true
}

# ── Check: game server process ──────────────────────────────────────

check_server() {
    echo "── Game server ──"

    if [[ -n "$SERVER_PID" ]]; then
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            pass "Server process $SERVER_PID is running"
        else
            fail "Server process $SERVER_PID is not running"
        fi
    else
        info "No --server-pid given, skipping process check"
    fi
}

# ── Check: device reachable via spyder ─────────────────────────────
#
# Uses `spyder devices --json` to list all known devices, then resolves
# DEVICE (if given) via `spyder resolve`. Branches on spyder exit codes:
#   11  device-not-found
#   12  device-not-connected
#   10  daemon-unreachable

RESOLVED_DEVICE=""  # canonical alias or selector to pass to subsequent spyder calls

check_device() {
    echo "── Device ──"

    if [[ -z "$DEVICE" ]]; then
        # No selector — require exactly one connected device
        local devices_json rc=0
        devices_json=$(spyder devices --json 2>/dev/null) || rc=$?
        case $rc in
            0) ;;
            10)
                fail "spyder daemon unreachable — is spyder serve running?"
                return
                ;;
            *)
                fail "spyder devices failed (exit $rc)"
                return
                ;;
        esac

        local count
        count=$(echo "$devices_json" | jq 'if type == "array" then length else (.devices // [] | length) end' 2>/dev/null || echo 0)

        if [[ "$count" -eq 0 ]]; then
            fail "No devices known to spyder"
            return
        elif [[ "$count" -gt 1 ]]; then
            fail "Multiple devices known — specify --device <selector>:"
            echo "$devices_json" | jq -r '
                (if type == "array" then . else .devices end) // [] |
                .[] | "  ....  \(.name) — \(.model // "unknown") (\(.uuid)) [\(.platform)]"
            ' 2>/dev/null || true
            return
        fi

        # Exactly one device
        local device_name device_uuid
        device_name=$(echo "$devices_json" | jq -r '(if type == "array" then . else .devices end)[0].name' 2>/dev/null)
        device_uuid=$(echo "$devices_json" | jq -r '(if type == "array" then . else .devices end)[0].uuid' 2>/dev/null)
        info "$device_name ($device_uuid)"
        RESOLVED_DEVICE="$device_name"
        pass "Device found: $device_name"
        return
    fi

    # Selector given — try to resolve it
    local resolve_out rc=0
    resolve_out=$(spyder resolve "$DEVICE" 2>&1) || rc=$?
    case $rc in
        0)
            local alias
            alias=$(echo "$resolve_out" | jq -r '.alias // empty' 2>/dev/null || echo "$DEVICE")
            RESOLVED_DEVICE="${alias:-$DEVICE}"
            info "Resolved '$DEVICE' → $RESOLVED_DEVICE"
            pass "Device '$DEVICE' found"
            ;;
        10)
            fail "spyder daemon unreachable — is spyder serve running?"
            ;;
        11)
            fail "Device '$DEVICE' not found in spyder inventory"
            # List available devices to help the user
            local devices_json
            devices_json=$(spyder devices --json 2>/dev/null) || true
            if [[ -n "$devices_json" ]]; then
                echo "$devices_json" | jq -r '
                    (if type == "array" then . else .devices end) // [] |
                    .[] | "  ....  \(.name) — \(.model // "unknown") (\(.uuid)) [\(.platform)]"
                ' 2>/dev/null || true
            fi
            ;;
        12)
            fail "Device '$DEVICE' is known but not connected"
            ;;
        *)
            # resolve may exit 1 for selector predicates; fall back to using
            # DEVICE as-is and let subsequent spyder calls report the error
            info "spyder resolve '$DEVICE' exited $rc — using selector as-is"
            RESOLVED_DEVICE="$DEVICE"
            pass "Device selector set: $DEVICE"
            ;;
    esac
}

# ── Check: app deployed and running ────────────────────────────────
#
# With --install:  `spyder deploy <device> <path>` (atomic terminate→install→
#                  launch→verify-pid). Branches on exit codes 20–24, 30, 40–42.
# Without --install: `spyder list-apps` to confirm installation, then
#                    `spyder device-state` for foreground app check.

check_app() {
    echo "── App ($BUNDLE_ID) ──"

    if [[ -z "$RESOLVED_DEVICE" ]]; then
        fail "No device resolved — cannot check app"
        return
    fi

    if [[ -n "$INSTALL" ]]; then
        if [[ ! -e "$INSTALL" ]]; then
            fail "Build artifact not found: $INSTALL"
            return
        fi

        info "Deploying $INSTALL to $RESOLVED_DEVICE..."
        local deploy_out rc=0
        deploy_out=$(spyder deploy "$RESOLVED_DEVICE" "$INSTALL" --bundle-id "$BUNDLE_ID" 2>&1) || rc=$?
        case $rc in
            0)
                local pid
                pid=$(echo "$deploy_out" | jq -r '.pid // empty' 2>/dev/null || true)
                if [[ -n "$pid" ]]; then
                    pass "App deployed and launched (pid $pid)"
                else
                    pass "App deployed and launched"
                fi
                ;;
            10) fail "spyder daemon unreachable during deploy" ;;
            11) fail "Device '$RESOLVED_DEVICE' not found during deploy" ;;
            12) fail "Device '$RESOLVED_DEVICE' not connected during deploy" ;;
            20) fail "App not installed after deploy (bundle id mismatch?)" ;;
            21) fail "Install failed — check signing/profile (spyder exit 21)" ;;
            22) fail "Launch failed after install (spyder exit 22)" ;;
            24) fail "PID verification failed — app may have crashed at startup (spyder exit 24)" ;;
            30) fail "Deploy timed out (spyder exit 30)" ;;
            40) fail "Trust not granted — accept the pairing dialog on the device (spyder exit 40)" ;;
            41) fail "Developer Mode is disabled on the device (spyder exit 41)" ;;
            42) fail "Device is locked — unlock it and retry (spyder exit 42)" ;;
            *)  fail "Deploy failed (spyder exit $rc): $deploy_out" ;;
        esac
        return
    fi

    # Passive check: confirm the app is installed
    local apps_out rc=0
    apps_out=$(spyder list-apps "$RESOLVED_DEVICE" --json 2>&1) || rc=$?
    case $rc in
        0)
            local installed
            installed=$(echo "$apps_out" | jq --arg b "$BUNDLE_ID" \
                'map(select(.bundle_id == $b)) | length' 2>/dev/null || echo 0)
            if [[ "$installed" -gt 0 ]]; then
                pass "App is installed"
            else
                fail "App ($BUNDLE_ID) not installed on device"
                return
            fi
            ;;
        10) fail "spyder daemon unreachable when checking app list"; return ;;
        11) fail "Device not found when checking app list"; return ;;
        12) fail "Device not connected when checking app list"; return ;;
        40) fail "Trust not granted — accept pairing dialog (spyder exit 40)"; return ;;
        41) fail "Developer Mode disabled (spyder exit 41)"; return ;;
        42) fail "Device locked (spyder exit 42)"; return ;;
        *)
            warn "Could not list apps (spyder exit $rc) — skipping install check"
            ;;
    esac

    # Passive check: foreground app via device-state
    local state_out state_rc=0
    state_out=$(spyder device-state "$RESOLVED_DEVICE" --json 2>&1) || state_rc=$?
    if [[ $state_rc -eq 0 ]]; then
        local fg_app
        fg_app=$(echo "$state_out" | jq -r '.foreground_app // empty' 2>/dev/null || true)
        if [[ -n "$fg_app" ]]; then
            if [[ "$fg_app" == *"$BUNDLE_ID"* ]]; then
                pass "App is in the foreground ($fg_app)"
            else
                warn "Foreground app is '$fg_app', expected '$BUNDLE_ID'"
            fi
        else
            info "Foreground app detection not available on this device — skipping running check"
        fi
    else
        info "device-state unavailable (spyder exit $state_rc) — skipping running check"
    fi
}

# ── Check: player connection (wait with timeout) ───────────────────

check_player_connection() {
    echo "── Player connection ──"

    local elapsed=0
    local interval=2

    while [[ $elapsed -lt $TIMEOUT ]]; do
        local response
        response=$(curl -sf --max-time 2 "http://localhost:$STREAM_PORT/stream/servers" 2>/dev/null) || {
            sleep "$interval"
            elapsed=$((elapsed + interval))
            continue
        }

        local sessions
        sessions=$(echo "$response" | jq -r '[(.servers // [])[].sessions // 0] | add // 0' 2>/dev/null || echo 0)

        if [[ "$sessions" -gt 0 ]]; then
            pass "Player connected ($sessions active session(s))"
            return
        fi

        sleep "$interval"
        elapsed=$((elapsed + interval))
    done

    fail "No player connected after ${TIMEOUT}s — check player logs"
}

# ── Check: player-side logs for errors ──────────────────────────────
#
# Uses `spyder log <device>` for mobile devices; scans DiagnosticReports
# for crash files on iOS (crash reports are written locally by Xcode/iOS).
# Desktop logs go to stderr — nothing to check programmatically.

check_player_logs() {
    echo "── Player logs (recent errors) ──"

    if [[ -z "$RESOLVED_DEVICE" ]] || [[ "$PLATFORM" == "desktop" ]]; then
        if [[ "$PLATFORM" == "desktop" ]]; then
            info "Desktop player logs go to stderr — check the terminal"
        else
            info "No device resolved — skipping log check"
        fi
        return
    fi

    # Use spyder log to retrieve recent device logs filtered to error level
    local log_out rc=0
    log_out=$(spyder log "$RESOLVED_DEVICE" --process "$BUNDLE_ID" --json 2>/dev/null) || rc=$?
    case $rc in
        0)
            local errors
            errors=$(echo "$log_out" | jq -r \
                '[.[] | select(.level? and (.level | test("error|fatal|crash"; "i")))] | .[-5:] | .[].message' \
                2>/dev/null || true)
            if [[ -n "$errors" ]]; then
                fail "Errors in device logs:"
                while IFS= read -r line; do
                    info "  $line"
                done <<< "$errors"
            else
                pass "No recent errors in device logs"
            fi
            ;;
        10) info "spyder daemon unreachable — skipping log check" ;;
        11) info "Device not found — skipping log check" ;;
        12) info "Device not connected — skipping log check" ;;
        *)
            # Fall back to crash report scan (iOS crash reports sync locally)
            local crash_dir="$HOME/Library/Logs/DiagnosticReports"
            local app_name
            app_name=$(echo "$BUNDLE_ID" | awk -F. '{print $NF}')
            local recent_crashes
            recent_crashes=$(find "$crash_dir" -name "${app_name}*" -mmin -5 2>/dev/null | head -3)
            if [[ -n "$recent_crashes" ]]; then
                fail "Recent crash report(s) found:"
                while IFS= read -r f; do
                    info "  $f"
                done <<< "$recent_crashes"
            else
                pass "No recent crash reports"
            fi
            ;;
    esac
}

# ── Run checks ──────────────────────────────────────────────────────

echo "ge smoke test${DEVICE:+ — device: $DEVICE}"
echo

check_stream_relay
check_server

if [[ "$PLATFORM" == "desktop" ]]; then
    info "Desktop player — no device checks needed"
else
    check_device
    check_app
fi

check_player_connection
check_player_logs

# ── Summary ─────────────────────────────────────────────────────────

echo
echo "── Summary ──"
echo "  $PASS passed, $FAIL failed, $WARN warnings"

if [[ $FAIL -gt 0 ]]; then
    echo
    echo "Fix the failures above before asking the user about visual output."
    exit 1
fi

if [[ $WARN -gt 0 ]]; then
    echo
    echo "Warnings present — investigate if visual output is unexpected."
fi

echo
echo "All critical checks passed. If visual verification is still needed,"
echo "ask the user what they see — but state what you already verified."
exit 0

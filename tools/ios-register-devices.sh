#!/usr/bin/env bash
# tools/ios-register-devices.sh — 🎯T110 headless iOS device enrollment
#
# Registers physical iOS device UDIDs with the Apple Developer Portal via the
# App Store Connect API key + fastlane `register_devices`, so a never-before-
# seen device does NOT require an Xcode GUI "Run".
#
# Inputs (first match wins):
#   1. UDID=… [NAME=…]              single device (NAME defaults to UDID)
#   2. DEVICES_FILE=path            Apple-style file: "UDID<TAB>Name" per line
#                                   (optional header "Device ID\tDevice Name")
#   3. (none)                       auto-discover via spyder, else xcrun xctrace
#
# Flags:
#   --list-only   print devices and exit (no ASC call)
#   --dry-run     show what would be registered; do not call Apple
#
# Required env for a real registration (same contract as ship preflight):
#   APP_STORE_CONNECT_API_KEY_KEY_ID
#   APP_STORE_CONNECT_API_KEY_ISSUER_ID
#   APP_STORE_CONNECT_API_KEY_KEY_PATH   path to AuthKey_*.p8
# Or:
#   APP_STORE_CONNECT_API_KEY_PATH       path to api_key.json (auto-mapped)
#
# Optional:
#   APPLE_TEAM_ID / REGISTER_DEVICES_TEAM_ID
#
# Usage:
#   make ge/ios-register-devices
#   make ge/ios-register-devices UDID=00008110-… NAME="iPhone 13"
#   DEVICES_FILE=./devices.txt make ge/ios-register-devices
#   tools/ios-register-devices.sh --list-only
#
# Enrollment is once per device, ever — safe to re-run (idempotent add-only).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

list_only=0
dry_run=0
while [ $# -gt 0 ]; do
  case "$1" in
    --list-only) list_only=1; shift ;;
    --dry-run)   dry_run=1; shift ;;
    -h|--help)
      sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown arg: $1 (try --help)" >&2
      exit 2
      ;;
  esac
done

is_hardware_udid() {
  [[ "$1" =~ ^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}$ ]]
}

# Emit "UDID<TAB>Name" lines. Prefer spyder inventory; fall back to xctrace.
discover_devices() {
  if command -v spyder >/dev/null 2>&1; then
    if spyder devices 2>/dev/null | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
if not isinstance(data, list) or not data:
    sys.exit(1)
n = 0
for d in data:
    if not isinstance(d, dict):
        continue
    plat = (d.get("platform") or "ios").lower()
    if plat not in ("ios", "ipados"):
        continue
    udid = d.get("uuid") or d.get("udid") or ""
    name = d.get("alias") or d.get("name") or udid
    parts = udid.split("-")
    # Hardware UDID: 8 hex + 16 hex. CoreDevice UUIDs are 8-4-4-4-12 — skip.
    if len(parts) == 2 and len(parts[0]) == 8 and len(parts[1]) == 16:
        print(f"{udid}\t{name}")
        n += 1
sys.exit(0 if n else 1)
'; then
      return 0
    fi
  fi

  if command -v xcrun >/dev/null 2>&1; then
    xcrun xctrace list devices 2>/dev/null | python3 -c '
import re, sys
pat = re.compile(
    r"^(.+?)\s+\([^)]*\)\s+\(([0-9A-Fa-f]{8}-[0-9A-Fa-f]{16})\)\s*$"
)
seen = set()
for line in sys.stdin:
    line = line.rstrip("\n")
    if "Simulator" in line:
        continue
    m = pat.match(line)
    if not m:
        continue
    name, udid = m.group(1).strip(), m.group(2)
    if udid in seen:
        continue
    seen.add(udid)
    print(f"{udid}\t{name}")
sys.exit(0 if seen else 1)
'
    return $?
  fi
  return 1
}

load_asc_env_from_json() {
  local json_path="$1"
  eval "$(python3 - "$json_path" <<'PY'
import json, os, shlex, sys
from pathlib import Path
d = json.load(open(sys.argv[1]))
print(f"export APP_STORE_CONNECT_API_KEY_KEY_ID={shlex.quote(d['key_id'])}")
print(f"export APP_STORE_CONNECT_API_KEY_ISSUER_ID={shlex.quote(d['issuer_id'])}")
key_path = os.environ.get("APP_STORE_CONNECT_API_KEY_KEY_PATH", "")
if not key_path:
    p = Path.home() / ".appstoreconnect" / "private_keys" / f"AuthKey_{d['key_id']}.p8"
    if not p.is_file() and d.get("key"):
        p.parent.mkdir(parents=True, exist_ok=True)
        key = d["key"]
        if not key.startswith("-----"):
            # Spaceship accepts raw base64 key body wrapped as PEM.
            key = "-----BEGIN PRIVATE KEY-----\n" + key.strip() + "\n-----END PRIVATE KEY-----\n"
        p.write_text(key)
        p.chmod(0o600)
    if p.is_file():
        print(f"export APP_STORE_CONNECT_API_KEY_KEY_PATH={shlex.quote(str(p))}")
PY
)"
}

tmp_devices=""
owned_tmp=0
cleanup() {
  if [ "$owned_tmp" -eq 1 ] && [ -n "$tmp_devices" ]; then
    rm -f "$tmp_devices"
  fi
}
trap cleanup EXIT

if [ -n "${UDID:-}" ]; then
  if ! is_hardware_udid "$UDID"; then
    echo "ERROR: UDID='$UDID' is not a hardware UDID (8 hex - 16 hex)." >&2
    echo "       CoreDevice identifiers from devicectl are not accepted by the portal." >&2
    exit 1
  fi
  tmp_devices="$(mktemp)"
  owned_tmp=1
  printf '%s\t%s\n' "$UDID" "${NAME:-$UDID}" >"$tmp_devices"
elif [ -n "${DEVICES_FILE:-}" ]; then
  if [ ! -f "$DEVICES_FILE" ]; then
    echo "ERROR: DEVICES_FILE='$DEVICES_FILE' not found" >&2
    exit 1
  fi
  tmp_devices="$DEVICES_FILE"
else
  tmp_devices="$(mktemp)"
  owned_tmp=1
  if ! discover_devices >"$tmp_devices"; then
    echo "ERROR: no physical iOS devices discovered." >&2
    echo "  Connect+trust a device, or pass UDID=… NAME=…, or DEVICES_FILE=…" >&2
    exit 1
  fi
fi

echo "── Devices to register ──"
count=0
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    ""|"Device ID"*) continue ;;
  esac
  udid="${line%%	*}"
  name="${line#*	}"
  [ "$udid" = "$name" ] && name="(unnamed)"
  printf '  %s  %s\n' "$udid" "$name"
  count=$((count + 1))
done <"$tmp_devices"

if [ "$count" -eq 0 ]; then
  echo "ERROR: device list is empty" >&2
  exit 1
fi

if [ "$list_only" -eq 1 ]; then
  exit 0
fi

# Resolve ASC credentials
if [ -z "${APP_STORE_CONNECT_API_KEY_PATH:-}" ] && [ -f "$HOME/.appstoreconnect/api_key.json" ]; then
  export APP_STORE_CONNECT_API_KEY_PATH="$HOME/.appstoreconnect/api_key.json"
fi
if [ -n "${APP_STORE_CONNECT_API_KEY_PATH:-}" ] && [ -f "$APP_STORE_CONNECT_API_KEY_PATH" ]; then
  load_asc_env_from_json "$APP_STORE_CONNECT_API_KEY_PATH"
fi

missing=()
for v in APP_STORE_CONNECT_API_KEY_KEY_ID APP_STORE_CONNECT_API_KEY_ISSUER_ID APP_STORE_CONNECT_API_KEY_KEY_PATH; do
  if [ -z "${!v:-}" ]; then missing+=("$v"); fi
done
if [ "${#missing[@]}" -gt 0 ]; then
  echo "ERROR: missing ASC API key env: ${missing[*]}" >&2
  echo "  See ge/docs/release-setup.md or place ~/.appstoreconnect/api_key.json" >&2
  exit 1
fi
if [ ! -f "$APP_STORE_CONNECT_API_KEY_KEY_PATH" ]; then
  echo "ERROR: KEY_PATH='$APP_STORE_CONNECT_API_KEY_KEY_PATH' is not a file" >&2
  exit 1
fi

export REGISTER_DEVICES_TEAM_ID="${APPLE_TEAM_ID:-${REGISTER_DEVICES_TEAM_ID:-SWA3H3N7TW}}"

# Prefer consumer repo (imports ge Fastfile); fall back to ge.
REPO_ROOT="$(git rev-parse --show-superproject-working-tree 2>/dev/null || true)"
if [ -z "$REPO_ROOT" ]; then
  REPO_ROOT="$(git -C "$GE_ROOT" rev-parse --show-toplevel)"
fi
run_dir="$REPO_ROOT"
if [ ! -f "$run_dir/Gemfile" ] && [ -f "$GE_ROOT/Gemfile" ]; then
  run_dir="$GE_ROOT"
fi

echo "── Registering via ASC API (team ${REGISTER_DEVICES_TEAM_ID}) ──"
if [ "$dry_run" -eq 1 ]; then
  echo "(dry-run) cd $run_dir && bundle exec fastlane ios register_dev_devices devices_file:$tmp_devices"
  exit 0
fi

(
  cd "$run_dir"
  if [ -f fastlane/Fastfile ]; then
    bundle exec fastlane ios register_dev_devices "devices_file:$tmp_devices"
  else
    # Invoke ge's Fastfile directly when the cwd has no fastlane/ tree.
    bundle exec fastlane ios register_dev_devices "devices_file:$tmp_devices" \
      --verbose 2>&1 || \
    FASTLANE_SKIP_UPDATE_CHECK=1 bundle exec fastlane ios register_dev_devices \
      "devices_file:$tmp_devices" -c "$GE_ROOT/fastlane/Fastfile"
  fi
)

echo "OK: devices submitted to the Developer Portal (add-only; known UDIDs are no-ops)."
echo "    Next: make ge/ios-device   # Automatic signing includes newly-registered devices"

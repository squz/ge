#!/usr/bin/env bash
# Play upload key — macOS keychain is the persistent store.
#
# Organisation-agnostic. Service names the *kind* of secret (default
# `play-upload`). Account names the organisation (caller-supplied;
# typically the game's STUDIO slug). ge does not choose the account.
#
# The keychain item is a one-line JSON envelope:
#   {"v":1,"alias":"upload","password":"…","p12_b64":"…"}
# PKCS12 password is generated at mint; humans do not type it.
#
# Gradle cannot read the keychain. `export` writes a 0600 temp PKCS12
# and prints shell assignments; the caller wipes the file.
#
#   ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<org> \
#     ./ge/tools/android-upload-keychain.sh mint|export|status|fingerprint
#
# CI (no login keychain): set ANDROID_KEYSTORE_PATH and the password
# env vars instead — see docs/android-release.md.

set -euo pipefail

SERVICE="${ANDROID_KEYSTORE_KEYCHAIN_SERVICE:-play-upload}"
ACCOUNT="${ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT:-}"
ALIAS="${ANDROID_KEY_ALIAS:-upload}"
VALIDITY_DAYS="${ANDROID_KEYSTORE_VALIDITY_DAYS:-10000}"
CN="${ANDROID_KEYSTORE_CN:-Play Upload}"
OU="${ANDROID_KEYSTORE_OU:-}"
O="${ANDROID_KEYSTORE_O:-}"
L="${ANDROID_KEYSTORE_L:-}"
ST="${ANDROID_KEYSTORE_ST:-}"
C="${ANDROID_KEYSTORE_C:-}"

usage() {
  echo "usage: ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<org> $0 mint|export|status|fingerprint" >&2
  exit 2
}

require_account() {
  if [[ -z "$ACCOUNT" ]]; then
    echo "error: ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT is required" >&2
    echo "  ge does not choose the organisation. The game sets this" >&2
    echo "  from STUDIO (or equivalent metadata)." >&2
    exit 1
  fi
}

require_keytool() {
  if ! command -v keytool >/dev/null 2>&1; then
    echo "error: keytool not found (install a JDK; e.g. brew install openjdk)" >&2
    exit 1
  fi
}

require_security() {
  if ! command -v security >/dev/null 2>&1; then
    echo "error: macOS keychain required (security not found)" >&2
    echo "  CI / non-Darwin: set ANDROID_KEYSTORE_PATH instead." >&2
    exit 1
  fi
}

wipe_file() {
  local f="$1"
  [[ -e "$f" ]] || return 0
  if rm -P "$f" 2>/dev/null; then
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$f" <<'PY' || true
import os, sys
p = sys.argv[1]
try:
    n = os.path.getsize(p)
    with open(p, "r+b") as fh:
        fh.write(b"\x00" * n)
        fh.flush()
        os.fsync(fh.fileno())
except OSError:
    pass
os.remove(p)
PY
    return 0
  fi
  rm -f "$f"
}

item_exists() {
  security find-generic-password -s "$SERVICE" -a "$ACCOUNT" >/dev/null 2>&1
}

read_payload() {
  security find-generic-password -s "$SERVICE" -a "$ACCOUNT" -w
}

cmd_status() {
  require_account
  require_security
  if item_exists; then
    echo "ok: $SERVICE / $ACCOUNT"
    return 0
  fi
  echo "missing: $SERVICE / $ACCOUNT" >&2
  return 1
}

cmd_mint() {
  require_account
  require_security
  require_keytool

  if item_exists && [[ "${ANDROID_KEYSTORE_KEYCHAIN_REPLACE:-}" != "1" ]]; then
    echo "error: keychain item already exists: $SERVICE / $ACCOUNT" >&2
    echo "  Refusing to overwrite. Set ANDROID_KEYSTORE_KEYCHAIN_REPLACE=1 to replace." >&2
    exit 1
  fi

  local tmp password dname payload
  # macOS mktemp creates an empty file; keytool -genkeypair refuses that.
  tmp="$(mktemp -t play-upload.XXXXXX)"
  rm -f "$tmp"
  # Trap must not close over a `local` (unbound after the function returns
  # under `set -u` when keytool fails).
  PLAY_UPLOAD_TMP="$tmp"
  trap 'wipe_file "${PLAY_UPLOAD_TMP:-}"' EXIT INT TERM

  password="$(openssl rand -base64 24)"
  dname="CN=${CN}"
  [[ -n "$OU" ]] && dname+=", OU=${OU}"
  [[ -n "$O" ]]  && dname+=", O=${O}"
  [[ -n "$L" ]]  && dname+=", L=${L}"
  [[ -n "$ST" ]] && dname+=", ST=${ST}"
  [[ -n "$C" ]]  && dname+=", C=${C}"

  keytool -genkeypair \
    -v \
    -keystore "$tmp" \
    -alias "$ALIAS" \
    -keyalg RSA \
    -keysize 2048 \
    -validity "$VALIDITY_DAYS" \
    -storetype PKCS12 \
    -storepass "$password" \
    -keypass "$password" \
    -dname "$dname"

  payload="$(python3 - "$tmp" "$password" "$ALIAS" <<'PY'
import base64, json, sys
path, password, alias = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, "rb") as fh:
    p12 = fh.read()
sys.stdout.write(json.dumps({
    "v": 1,
    "alias": alias,
    "password": password,
    "p12_b64": base64.b64encode(p12).decode("ascii"),
}, separators=(",", ":")))
PY
)"

  if item_exists; then
    security delete-generic-password -s "$SERVICE" -a "$ACCOUNT" >/dev/null
  fi
  security add-generic-password \
    -s "$SERVICE" \
    -a "$ACCOUNT" \
    -l "Play upload ($ACCOUNT)" \
    -w "$payload" \
    -T /usr/bin/security \
    -U

  echo
  echo "Stored Play upload key in keychain:"
  echo "  service: $SERVICE"
  echo "  account: $ACCOUNT"
  echo "  alias:   $ALIAS"
  echo
  echo "SHA-256 fingerprint (record next to the keychain item / Play Console):"
  keytool -list -v \
    -keystore "$tmp" \
    -alias "$ALIAS" \
    -storepass "$password" 2>/dev/null \
    | grep -E 'SHA256:|SHA1:|Alias name:|Valid from:' || true
  echo
  echo "Next steps:"
  echo "  1. ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=$ACCOUNT make ge/android-bundle"
  echo "  2. Follow ge/docs/android-release.md → Play App Signing enrollment."
  echo
  echo "The private key is not on the filesystem. Do not copy it to disk."
}

cmd_export() {
  require_account
  require_security
  if ! item_exists; then
    echo "error: no Play upload key in keychain: $SERVICE / $ACCOUNT" >&2
    echo "  Mint with: ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=$ACCOUNT $0 mint" >&2
    exit 1
  fi
  local dest parsed password alias payload
  dest="$(mktemp -t play-upload.XXXXXX)"
  payload="$(read_payload)"
  parsed="$(
    SERVICE="$SERVICE" ACCOUNT="$ACCOUNT" python3 -c '
import base64, json, os, sys
dest, raw = sys.argv[1], sys.argv[2]
service, account = os.environ["SERVICE"], os.environ["ACCOUNT"]
try:
    data = json.loads(raw)
except json.JSONDecodeError:
    sys.stderr.write(
        f"error: keychain item {service} / {account} is not a ge upload-key envelope\n"
    )
    sys.exit(1)
if int(data.get("v", 0)) != 1 or "p12_b64" not in data or "password" not in data:
    sys.stderr.write(
        f"error: unexpected keychain payload for {service} / {account}\n"
    )
    sys.exit(1)
with open(dest, "wb") as fh:
    fh.write(base64.b64decode(data["p12_b64"]))
os.chmod(dest, 0o600)
sys.stdout.write(data["password"] + "\n")
sys.stdout.write(data.get("alias", "upload") + "\n")
' "$dest" "$payload"
  )"
  password="$(printf '%s\n' "$parsed" | sed -n '1p')"
  alias="$(printf '%s\n' "$parsed" | sed -n '2p')"
  printf "export ANDROID_KEYSTORE_PATH='%s'\n" "$dest"
  printf "export ANDROID_KEYSTORE_PASSWORD='%s'\n" "$password"
  printf "export ANDROID_KEY_ALIAS='%s'\n" "$alias"
  printf "export ANDROID_KEY_PASSWORD='%s'\n" "$password"
}

cmd_fingerprint() {
  require_account
  require_security
  require_keytool
  if ! item_exists; then
    echo "error: no Play upload key in keychain: $SERVICE / $ACCOUNT" >&2
    exit 1
  fi
  eval "$(cmd_export)"
  local tmp="$ANDROID_KEYSTORE_PATH"
  trap 'wipe_file "$tmp"' EXIT INT TERM
  keytool -list -v \
    -keystore "$tmp" \
    -alias "$ANDROID_KEY_ALIAS" \
    -storepass "$ANDROID_KEYSTORE_PASSWORD" 2>/dev/null \
    | grep -E 'SHA256:|SHA1:|Alias name:|Valid from:' || true
}

cmd="${1:-}"
case "$cmd" in
  mint) cmd_mint ;;
  export) cmd_export ;;
  status) cmd_status ;;
  fingerprint) cmd_fingerprint ;;
  *) usage ;;
esac

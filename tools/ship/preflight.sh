#!/usr/bin/env bash
# tools/ship/preflight.sh — 🎯T64.2 pre-ship gate checks
#
# Verifies that the environment is ready for a ship before any build begins.
# Exits 0 + prints "READY" if every check passes.
# Exits 1 + prints "BLOCKED: <reason list>" if any check fails.
#
# Checks performed:
#   1. Git working tree is clean (no uncommitted changes).
#   2. ge submodule is clean and on master (for library releases).
#   3. Required environment variables are set.
#   4. For versioned lanes (beta / release): VERSION format matches
#      MAJOR.MINOR.PATCH.
#   5. fastlane match appstore --readonly succeeds without prompting.
#   6. (Optional, when XCODE_VERSION_PIN is set) Installed Xcode matches pin.
#
# Usage:
#   tools/ship/preflight.sh [--lane LANE] [--version VERSION]
#
# Where LANE is one of: alpha | beta | release.
# VERSION is required for beta and release lanes.

set -uo pipefail

# REPO_ROOT must resolve to the CONSUMING project's root (where the
# game's Gemfile, Matchfile, and ios/CMakeLists.txt live), not to ge's
# own root (which is the submodule). When ge is consumed as a submodule
# (the normal case), `git rev-parse --show-superproject-working-tree`
# returns the parent project's working tree. When ge is run from its
# own repo (e.g. for testing the ship scripts standalone), that command
# returns empty and we fall back to ge's own toplevel.
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-superproject-working-tree 2>/dev/null)"
if [ -z "${REPO_ROOT}" ]; then
    REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
fi

lane="${SHIP_LANE:-alpha}"
version="${SHIP_VERSION:-}"
blocked_reasons=()

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --lane)    lane="${2:?}"; shift 2 ;;
        --version) version="${2:?}"; shift 2 ;;
        *)         echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Helper: record a failure reason (non-fatal, collected until the end).
# ---------------------------------------------------------------------------
fail() { blocked_reasons+=("$*"); }

# ---------------------------------------------------------------------------
# Check 1: git working tree clean
# ---------------------------------------------------------------------------
echo "preflight: checking git status..."
# `--ignore-submodules=untracked` so generated artefacts inside submodules
# (e.g. ge/web/dist/* built by a historical dashboard) don't trip the gate —
# they're untracked-in-submodule, not real changes to the parent. Real
# parent-repo changes still count.
if ! git -C "${REPO_ROOT}" diff --quiet HEAD --ignore-submodules=untracked -- 2>/dev/null || \
   [ -n "$(git -C "${REPO_ROOT}" status --porcelain --ignore-submodules=untracked 2>/dev/null)" ]; then
    fail "working tree has uncommitted changes (commit or stash before shipping)"
else
    echo "  [PASS] working tree clean"
fi

# ---------------------------------------------------------------------------
# Check 2: ge submodule status
# ---------------------------------------------------------------------------
echo "preflight: checking ge submodule..."
ge_submodule_path="${REPO_ROOT}/ge"
if [ -d "${ge_submodule_path}" ]; then
    # `--untracked-files=no` so generated artefacts inside ge (e.g.
    # web/dist/* from a dashboard build, build/* from local engine
    # tests) don't trip the gate — they're not part of ge's source tree
    # and never committed. Real tracked-file modifications still count.
    if ! git -C "${ge_submodule_path}" diff --quiet HEAD -- 2>/dev/null || \
       [ -n "$(git -C "${ge_submodule_path}" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        fail "ge submodule has uncommitted changes"
    else
        echo "  [PASS] ge submodule clean"
    fi

    ge_branch="$(git -C "${ge_submodule_path}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "HEAD")"
    if [ "${ge_branch}" != "master" ] && [ "${ge_branch}" != "HEAD" ]; then
        fail "ge submodule is on branch '${ge_branch}', not master — pin it before shipping"
    else
        echo "  [PASS] ge submodule on master (or detached HEAD at tag)"
    fi
else
    echo "  [SKIP] ge/ directory not found (standalone ge repo build, not a consumer project)"
fi

# ---------------------------------------------------------------------------
# Check 3: Required environment variables
# ---------------------------------------------------------------------------
echo "preflight: checking required env vars..."
required_vars=(
    APP_STORE_CONNECT_API_KEY_KEY_ID
    APP_STORE_CONNECT_API_KEY_ISSUER_ID
    APP_STORE_CONNECT_API_KEY_KEY_PATH
    MATCH_PASSWORD
    SHIP_SCHEME
    APP_ID
    APPLE_TEAM_ID
)
for var in "${required_vars[@]}"; do
    if [ -z "${!var:-}" ]; then
        fail "required env var ${var} is not set (see docs/release-setup.md)"
    else
        echo "  [PASS] ${var} set"
    fi
done

# Verify the .p8 key file actually exists.
key_path="${APP_STORE_CONNECT_API_KEY_KEY_PATH:-}"
if [ -n "${key_path}" ] && [ ! -f "${key_path}" ]; then
    fail "ASC API key file not found at ${key_path}"
fi

# ---------------------------------------------------------------------------
# Check 4: VERSION format for beta / release lanes
# ---------------------------------------------------------------------------
if [ "${lane}" = "beta" ] || [ "${lane}" = "release" ]; then
    echo "preflight: checking VERSION format..."
    if [ -z "${version}" ]; then
        fail "VERSION is required for lane '${lane}' (pass VERSION=x.y.z to make)"
    elif ! echo "${version}" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        fail "VERSION '${version}' is not MAJOR.MINOR.PATCH format"
    else
        echo "  [PASS] VERSION=${version} is valid"
    fi
fi

# ---------------------------------------------------------------------------
# Check 5: fastlane match --readonly
# ---------------------------------------------------------------------------
echo "preflight: verifying fastlane match (--readonly)..."
if ! command -v bundle >/dev/null 2>&1; then
    fail "bundler not found — run 'bundle install' (see docs/release-setup.md)"
else
    # Pass the API key via fastlane's JSON-format key file. Match doesn't
    # auto-read APP_STORE_CONNECT_API_KEY_KEY_PATH for the .p8 directly;
    # it wants the JSON envelope. The envelope is constructed lazily here
    # so consumers don't have to maintain a separate file (the data is
    # all in env vars already).
    api_key_json="${TMPDIR:-/tmp}/ship_preflight_api_key.$$.json"
    trap 'rm -f "${api_key_json}"' EXIT
    cat > "${api_key_json}" <<JSON
{
  "key_id": "${APP_STORE_CONNECT_API_KEY_KEY_ID}",
  "issuer_id": "${APP_STORE_CONNECT_API_KEY_ISSUER_ID}",
  "key": $(awk 'BEGIN{printf "\""} {gsub(/\\/,"\\\\"); gsub(/"/,"\\\""); printf "%s\\n",$0} END{printf "\""}' "${APP_STORE_CONNECT_API_KEY_KEY_PATH}"),
  "duration": 1200,
  "in_house": false
}
JSON
    # `match` needs --app_identifier to know what cert to verify; without
    # it, the readonly check fails silently against an empty Appfile.
    # Consuming game's Makefile exports APP_ID (e.g. com.squz.multimaze)
    # before invoking ship-preflight; we just forward it.
    if ! (cd "${REPO_ROOT}" && bundle exec fastlane match appstore --readonly \
          --app_identifier "${APP_ID}" \
          --api_key_path "${api_key_json}" 2>&1); then
        fail "fastlane match appstore --readonly --app_identifier ${APP_ID} failed — run 'bundle exec fastlane sync_certs --app_identifier ${APP_ID} --api_key_path \"${api_key_json}\"' to diagnose"
    else
        echo "  [PASS] fastlane match --readonly succeeded for ${APP_ID}"
    fi
fi

# ---------------------------------------------------------------------------
# Check 6: Xcode version pin (optional)
# ---------------------------------------------------------------------------
if [ -n "${XCODE_VERSION_PIN:-}" ]; then
    echo "preflight: checking Xcode version against pin ${XCODE_VERSION_PIN}..."
    installed_xcode="$(xcodebuild -version 2>/dev/null | head -1 | awk '{print $2}')"
    if [ "${installed_xcode}" != "${XCODE_VERSION_PIN}" ]; then
        fail "Xcode version mismatch: installed=${installed_xcode}, pin=${XCODE_VERSION_PIN}"
    else
        echo "  [PASS] Xcode ${installed_xcode} matches pin"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
if [ ${#blocked_reasons[@]} -eq 0 ]; then
    echo "READY"
    exit 0
else
    echo "BLOCKED:"
    for reason in "${blocked_reasons[@]}"; do
        echo "  - ${reason}"
    done
    exit 1
fi

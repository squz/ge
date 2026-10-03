#!/usr/bin/env bash
# tools/ship/preflight.sh — 🎯T64.2 pre-ship gate checks
#
# Verifies that the environment is ready for a ship before any build begins.
# Exits 0 + prints "READY" if every check passes.
# Configuration failures exit 1 with BLOCKED reasons. Spyder failures preserve
# its exit code (20 for missing secrets).
#
# Checks performed:
#   1. Git working tree is clean (no uncommitted changes).
#   2. ge submodule is clean and on master (for library releases).
#   3. Non-secret app configuration and spyder secret availability.
#   4. For versioned lanes (beta / release): VERSION format matches
#      MAJOR.MINOR.PATCH.
#   5. spyder fastlane sync_certs (readonly) succeeds without prompting.
#   6. (Optional, when XCODE_VERSION_PIN is set) Installed Xcode matches pin.
#
# Usage:
#   tools/ship/preflight.sh [--lane LANE] [--version VERSION]
#
# Where LANE is one of: alpha | beta | release.
# VERSION is required for beta and release lanes.

set -uo pipefail
source "$(dirname "$0")/spyder.sh"

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
# Check 5: spyder availability, then readonly cert resolution.
# Never invoke fastlane after a failed configuration or secret check.
# ---------------------------------------------------------------------------
ship_validate_lane "${lane}" "${version}" || fail "invalid lane/version"
if [ ${#blocked_reasons[@]} -eq 0 ]; then
    ship_check_lane_secrets "${lane}" || exit $?
    (cd "${REPO_ROOT}" && ship_fastlane sync_certs app_identifier:"${APP_ID}") || exit $?
    echo "  [PASS] readonly sync_certs succeeded"
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

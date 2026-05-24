#!/usr/bin/env bash
# tools/ship/build.sh — 🎯T64.2 + 🎯T64.4 + 🎯T64.5 build orchestrator
#
# Orchestrates a full ship build:
#   1. Creates a worktree at the given tag (🎯T64.4 isolation).
#   2. Writes the build manifest into the worktree (🎯T64.5 provenance).
#   3. Runs `bundle exec fastlane build_ipa` inside the worktree.
#
# On success, removes the worktree and prints the IPA path.
# On failure, leaves the worktree for inspection and exits non-zero.
#
# Usage:
#   tools/ship/build.sh --tag TAG --lane LANE [--output-dir DIR] [--keep-worktree]
#
# Required env:
#   SHIP_SCHEME — Xcode scheme to build (e.g. "MultiMaze2")
#   APP_STORE_CONNECT_API_KEY_KEY_ID
#   APP_STORE_CONNECT_API_KEY_ISSUER_ID
#   APP_STORE_CONNECT_API_KEY_KEY_PATH
#   MATCH_PASSWORD

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# REPO_ROOT = consuming project root when ge is a submodule, else ge's own root.
# See preflight.sh for the rationale.
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-superproject-working-tree 2>/dev/null)"
if [ -z "${REPO_ROOT}" ]; then
    REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
fi

# Source helpers (idempotent — functions defined, no side effects).
# shellcheck source=tools/ship/worktree.sh
source "${SCRIPT_DIR}/worktree.sh"
# shellcheck source=tools/ship/manifest.sh
source "${SCRIPT_DIR}/manifest.sh"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
tag=""
lane=""
output_dir="${REPO_ROOT}/build/ship"
keep_worktree=false

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --tag)           tag="${2:?}"; shift 2 ;;
        --lane)          lane="${2:?}"; shift 2 ;;
        --output-dir)    output_dir="${2:?}"; shift 2 ;;
        --keep-worktree) keep_worktree=true; shift ;;
        *) echo "build.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [ -z "${tag}" ] || [ -z "${lane}" ]; then
    echo "Usage: build.sh --tag TAG --lane LANE [--output-dir DIR] [--keep-worktree]" >&2
    exit 1
fi

# Validate required env vars.
: "${SHIP_SCHEME:?SHIP_SCHEME env var must be set to the Xcode scheme name}"
: "${APP_STORE_CONNECT_API_KEY_KEY_ID:?}"
: "${APP_STORE_CONNECT_API_KEY_ISSUER_ID:?}"
: "${APP_STORE_CONNECT_API_KEY_KEY_PATH:?}"
: "${MATCH_PASSWORD:?}"

ipa_output_dir="${output_dir}/${tag}"
mkdir -p "${ipa_output_dir}"

# ---------------------------------------------------------------------------
# ERR trap — leave worktree for inspection on failure.
# ---------------------------------------------------------------------------
_ship_worktree_dir=""
_err_handler() {
    local exit_code=$?
    echo ""
    echo "ship/build: ERROR (exit ${exit_code})"
    if [ "${keep_worktree}" = "false" ]; then
        ship_worktree_remove_on_failure "${_ship_worktree_dir}"
    fi
    exit "${exit_code}"
}
trap '_err_handler' ERR

# ---------------------------------------------------------------------------
# Step 1: Create worktree (🎯T64.4)
# ---------------------------------------------------------------------------
echo "ship/build: creating worktree for ${tag}..."
ship_worktree_create "${tag}"
_ship_worktree_dir="${SHIP_WORKTREE_DIR}"

# ---------------------------------------------------------------------------
# Step 2: Write + embed build manifest (🎯T64.5)
# ---------------------------------------------------------------------------
echo "ship/build: writing build manifest..."
ship_manifest_write "${tag}" "${lane}" "${SHIP_WORKTREE_DIR}"

# ---------------------------------------------------------------------------
# Step 3: Run fastlane build_ipa inside the worktree
# ---------------------------------------------------------------------------
echo "ship/build: running fastlane build_ipa (scheme=${SHIP_SCHEME}, lane=${lane})..."
(
    cd "${REPO_ROOT}"
    # PATH prefix: ensure macOS's system rsync (not Homebrew 3.x) is used
    # by xcodebuild's codesign workflow — a known incompatibility.
    PATH="/usr/bin:/bin:/usr/sbin:/sbin:${PATH}" \
    SHIP_OUTPUT_DIR="${ipa_output_dir}" \
    bundle exec fastlane build_ipa \
        scheme:"${SHIP_SCHEME}" \
        output_dir:"${ipa_output_dir}" \
        type:"appstore"
)

# ---------------------------------------------------------------------------
# Step 4: Clean up worktree on success
# ---------------------------------------------------------------------------
if [ "${keep_worktree}" = "false" ]; then
    echo "ship/build: removing worktree (build succeeded)"
    ship_worktree_remove "${_ship_worktree_dir}"
else
    echo "ship/build: keeping worktree at ${_ship_worktree_dir} (--keep-worktree)"
fi

echo ""
echo "ship/build: IPA written to ${ipa_output_dir}/"
ls "${ipa_output_dir}"/*.ipa 2>/dev/null || echo "  (no .ipa found — check fastlane output)"

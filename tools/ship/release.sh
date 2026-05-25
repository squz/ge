#!/usr/bin/env bash
# tools/ship/release.sh — 🎯T64.2 release-lane orchestrator
#
# Dispatches alpha / beta / release ship flows. Called by Module.mk's
# ship-alpha, ship-beta, ship-release targets. Each lane:
#
#   alpha   — build at HEAD, upload to TestFlight internal. No semver bump,
#              no git tag. Build number comes from `git rev-list --count HEAD`
#              (MAX-of-two-sources pattern: rev-list vs Transporter SQLite).
#
#   beta    — derive tag v{VERSION}-beta.{N} (auto-incremented from existing
#              beta tags), build, upload to TestFlight external, push the tag.
#
#   release — confirm CONFIRM=1, build v{VERSION}, upload, submit for review,
#              tag v{VERSION}, push the tag.
#
# Usage:
#   tools/ship/release.sh --lane alpha
#   tools/ship/release.sh --lane beta  --version 0.31.0
#   tools/ship/release.sh --lane release --version 0.31.0 --confirm
#
# Required env (see docs/release-setup.md):
#   SHIP_SCHEME, APP_STORE_CONNECT_API_KEY_*, MATCH_PASSWORD

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# REPO_ROOT = consuming project root when ge is a submodule, else ge's own root.
# See preflight.sh for the rationale.
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-superproject-working-tree 2>/dev/null)"
if [ -z "${REPO_ROOT}" ]; then
    REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
fi

source "${SCRIPT_DIR}/worktree.sh"
source "${SCRIPT_DIR}/manifest.sh"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
lane=""
version=""
confirm=false
keep_worktree=false

while [ $# -gt 0 ]; do
    case "$1" in
        --lane)          lane="${2:?}"; shift 2 ;;
        --version)       version="${2:?}"; shift 2 ;;
        --confirm)       confirm=true; shift ;;
        --keep-worktree) keep_worktree=true; shift ;;
        *) echo "release.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [ -z "${lane}" ]; then
    echo "Usage: release.sh --lane alpha|beta|release [--version X.Y.Z] [--confirm] [--keep-worktree]" >&2
    exit 1
fi

# Validate required env.
: "${SHIP_SCHEME:?SHIP_SCHEME env var must be set to the Xcode scheme name}"
: "${APP_STORE_CONNECT_API_KEY_KEY_ID:?}"
: "${APP_STORE_CONNECT_API_KEY_ISSUER_ID:?}"
: "${APP_STORE_CONNECT_API_KEY_KEY_PATH:?}"
: "${MATCH_PASSWORD:?}"

# ---------------------------------------------------------------------------
# Helper: derive the next build number
#
# MAX of:
#   (a) git rev-list --count HEAD  (monotonically increasing per commit)
#   (b) Transporter SQLite cache   (App Store Connect's view of latest build)
#
# This dual-source pattern closes the gap where clearing Transporter's
# SQLite cache would cause (b) to return 0 and reset the build counter.
# ---------------------------------------------------------------------------
next_build_number() {
    local rev_count
    rev_count="$(git -C "${REPO_ROOT}" rev-list --count HEAD)"

    local transporter_max=0
    local transporter_db="${HOME}/Library/Caches/com.apple.amp.itmstransporter/transporterDB.sqlite"
    if [ -f "${transporter_db}" ] && command -v sqlite3 >/dev/null 2>&1; then
        transporter_max="$(sqlite3 "${transporter_db}" \
            "SELECT COALESCE(MAX(CAST(build_number AS INTEGER)), 0) FROM uploaded_packages;" \
            2>/dev/null || echo 0)"
    fi

    # Return the larger of the two.
    if [ "${rev_count}" -ge "${transporter_max}" ]; then
        echo "${rev_count}"
    else
        echo "${transporter_max}"
    fi
}

# ---------------------------------------------------------------------------
# Helper: find next beta N for a given version
# ---------------------------------------------------------------------------
next_beta_n() {
    local ver="${1:?}"
    local max_n=0
    while IFS= read -r tag_line; do
        local n="${tag_line##*-beta.}"
        if [ "${n}" -gt "${max_n}" ] 2>/dev/null; then
            max_n="${n}"
        fi
    done < <(git -C "${REPO_ROOT}" tag -l "v${ver}-beta.*" 2>/dev/null || true)
    echo $((max_n + 1))
}

# ---------------------------------------------------------------------------
# ERR trap
# ---------------------------------------------------------------------------
_ship_worktree_dir=""
_err_handler() {
    local exit_code=$?
    echo ""
    echo "ship/release: ERROR (exit ${exit_code})"
    if [ "${keep_worktree}" = "false" ]; then
        ship_worktree_remove_on_failure "${_ship_worktree_dir}"
    fi
    exit "${exit_code}"
}
trap '_err_handler' ERR

# ---------------------------------------------------------------------------
# Lane: alpha
#
# Build at HEAD (no tag required), upload to TestFlight internal testers.
# ---------------------------------------------------------------------------
if [ "${lane}" = "alpha" ]; then
    echo "ship/release: alpha lane — TestFlight internal"
    build_number="$(next_build_number)"
    echo "  build number: ${build_number}"

    ipa_dir="${REPO_ROOT}/build/ship/alpha-${build_number}"
    mkdir -p "${ipa_dir}"

    # Write manifest at HEAD (no tag for alpha).
    head_sha="$(git -C "${REPO_ROOT}" rev-parse --short HEAD)"
    alpha_tag="alpha-${build_number}"
    ship_manifest_write "${alpha_tag}" "alpha"

    echo "ship/release: building IPA..."
    (
        cd "${REPO_ROOT}"
        PATH="${PATH}:/usr/bin:/bin:/usr/sbin:/sbin" \
        SHIP_OUTPUT_DIR="${ipa_dir}" \
        bundle exec fastlane build_ipa \
            scheme:"${SHIP_SCHEME}" \
            output_dir:"${ipa_dir}" \
            type:"appstore"
    )

    if [ "${SHIP_DRY_RUN:-}" = "1" ]; then
        echo "ship/release: SHIP_DRY_RUN=1 — skipping TestFlight upload."
        echo "ship/release: alpha dry-run done — build ${build_number} (${head_sha}) built but NOT uploaded"
        exit 0
    fi

    echo "ship/release: uploading to TestFlight (internal)..."
    local_ipa="$(ls "${ipa_dir}"/*.ipa 2>/dev/null | head -1)"
    (
        cd "${REPO_ROOT}"
        bundle exec fastlane upload_testflight \
            ipa:"${local_ipa}" \
            external:false
    )

    echo ""
    echo "ship/release: alpha done — build ${build_number} (${head_sha}) uploaded to TestFlight internal"
    exit 0
fi

# ---------------------------------------------------------------------------
# Lane: beta
# ---------------------------------------------------------------------------
if [ "${lane}" = "beta" ]; then
    if [ -z "${version}" ]; then
        echo "ship/release: --version is required for beta lane" >&2
        exit 1
    fi

    beta_n="$(next_beta_n "${version}")"
    beta_tag="v${version}-beta.${beta_n}"
    echo "ship/release: beta lane — ${beta_tag}"

    build_number="$(next_build_number)"
    echo "  build number: ${build_number}"

    # Create and push the beta tag.
    git -C "${REPO_ROOT}" tag -a "${beta_tag}" -m "Beta ${beta_tag}"
    git -C "${REPO_ROOT}" push origin "${beta_tag}"
    echo "  tagged and pushed: ${beta_tag}"

    # Build via worktree (🎯T64.4).
    ship_worktree_create "${beta_tag}"
    _ship_worktree_dir="${SHIP_WORKTREE_DIR}"

    ship_manifest_write "${beta_tag}" "beta" "${SHIP_WORKTREE_DIR}"

    ipa_dir="${REPO_ROOT}/build/ship/${beta_tag}"
    mkdir -p "${ipa_dir}"

    (
        cd "${REPO_ROOT}"
        PATH="${PATH}:/usr/bin:/bin:/usr/sbin:/sbin" \
        SHIP_OUTPUT_DIR="${ipa_dir}" \
        bundle exec fastlane build_ipa \
            scheme:"${SHIP_SCHEME}" \
            output_dir:"${ipa_dir}" \
            type:"appstore"
    )

    local_ipa="$(ls "${ipa_dir}"/*.ipa 2>/dev/null | head -1)"
    (
        cd "${REPO_ROOT}"
        bundle exec fastlane upload_testflight \
            ipa:"${local_ipa}" \
            external:true
    )

    if [ "${keep_worktree}" = "false" ]; then
        ship_worktree_remove "${_ship_worktree_dir}"
    fi

    echo ""
    echo "ship/release: beta done — ${beta_tag} (build ${build_number}) uploaded to TestFlight external"
    exit 0
fi

# ---------------------------------------------------------------------------
# Lane: release
# ---------------------------------------------------------------------------
if [ "${lane}" = "release" ]; then
    if [ -z "${version}" ]; then
        echo "ship/release: --version is required for release lane" >&2
        exit 1
    fi
    if [ "${confirm}" = "false" ]; then
        echo ""
        echo "ERROR: make ship-release requires CONFIRM=1 (this uploads to the App Store)."
        echo "  make ship-release VERSION=${version} CONFIRM=1"
        exit 1
    fi

    release_tag="v${version}"
    echo "ship/release: release lane — ${release_tag}"

    # Verify the tag doesn't already exist.
    if git -C "${REPO_ROOT}" rev-parse --verify "refs/tags/${release_tag}" >/dev/null 2>&1; then
        echo "ship/release: tag ${release_tag} already exists — is this a re-ship?" >&2
        echo "  If intentional, delete the tag and try again." >&2
        exit 1
    fi

    build_number="$(next_build_number)"
    echo "  build number: ${build_number}"

    # Tag and push.
    git -C "${REPO_ROOT}" tag -a "${release_tag}" -m "Release ${release_tag}"
    git -C "${REPO_ROOT}" push origin "${release_tag}"
    echo "  tagged and pushed: ${release_tag}"

    # Build via worktree (🎯T64.4).
    ship_worktree_create "${release_tag}"
    _ship_worktree_dir="${SHIP_WORKTREE_DIR}"

    ship_manifest_write "${release_tag}" "release" "${SHIP_WORKTREE_DIR}"

    ipa_dir="${REPO_ROOT}/build/ship/${release_tag}"
    mkdir -p "${ipa_dir}"

    (
        cd "${REPO_ROOT}"
        PATH="${PATH}:/usr/bin:/bin:/usr/sbin:/sbin" \
        SHIP_OUTPUT_DIR="${ipa_dir}" \
        bundle exec fastlane build_ipa \
            scheme:"${SHIP_SCHEME}" \
            output_dir:"${ipa_dir}" \
            type:"appstore"
    )

    local_ipa="$(ls "${ipa_dir}"/*.ipa 2>/dev/null | head -1)"
    (
        cd "${REPO_ROOT}"
        bundle exec fastlane upload_testflight \
            ipa:"${local_ipa}" \
            external:false
    )

    (
        cd "${REPO_ROOT}"
        bundle exec fastlane submit_release
    )

    if [ "${keep_worktree}" = "false" ]; then
        ship_worktree_remove "${_ship_worktree_dir}"
    fi

    echo ""
    echo "ship/release: release done — ${release_tag} submitted for App Store review"
    echo "  Monitor progress at https://appstoreconnect.apple.com"
    exit 0
fi

echo "ship/release: unknown lane '${lane}'" >&2
exit 1

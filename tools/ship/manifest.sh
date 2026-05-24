#!/usr/bin/env bash
# tools/ship/manifest.sh — 🎯T64.5 build manifest writer
#
# Writes build/ship/<TAG>/manifest.json with full provenance fields and
# optionally embeds the manifest into an iOS or Android build artefact.
#
# Usage:
#   tools/ship/manifest.sh --write TAG LANE [WORKTREE_DIR]
#       Writes manifest.json to build/ship/<TAG>/manifest.json.
#       If WORKTREE_DIR is given, also copies the manifest into the
#       iOS xcassets dataset and Android raw resource directory under
#       that worktree.
#
#   tools/ship/manifest.sh --embed-ios  WORKTREE_DIR MANIFEST_JSON
#   tools/ship/manifest.sh --embed-android WORKTREE_DIR MANIFEST_JSON
#       Embed an already-written manifest into a specific platform tree.

set -euo pipefail

# REPO_ROOT = consuming project root when ge is a submodule, else ge's own root.
# See preflight.sh for the rationale.
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-superproject-working-tree 2>/dev/null)"
if [ -z "${REPO_ROOT}" ]; then
    REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
fi
SHIP_BUILD_DIR="${REPO_ROOT}/build/ship"

# ---------------------------------------------------------------------------
# ship_manifest_write TAG LANE [WORKTREE_DIR]
#
# Writes build/ship/<TAG>/manifest.json.
# If WORKTREE_DIR is non-empty, also embeds into the worktree's iOS / Android
# resource trees.
# ---------------------------------------------------------------------------
ship_manifest_write() {
    local tag="${1:?ship_manifest_write requires TAG}"
    local lane="${2:?ship_manifest_write requires LANE}"
    local worktree_dir="${3:-}"

    local out_dir="${SHIP_BUILD_DIR}/${tag}"
    mkdir -p "${out_dir}"
    local manifest_path="${out_dir}/manifest.json"

    local git_sha build_timestamp_utc git_branch ge_version builder

    # Resolve from the worktree if given, else from REPO_ROOT.
    local git_root="${worktree_dir:-${REPO_ROOT}}"

    git_sha="$(git -C "${git_root}" rev-parse HEAD)"
    git_branch="$(git -C "${git_root}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "HEAD")"
    build_timestamp_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

    # ge_version: strip leading 'v' from the tag.
    ge_version="${tag#v}"

    # Builder: user@hostname (best effort).
    builder="$(id -un 2>/dev/null || echo "unknown")@$(hostname -s 2>/dev/null || echo "unknown")"

    echo "ship/manifest: writing ${manifest_path}"
    cat > "${manifest_path}" <<EOF
{
  "tag": "${tag}",
  "git_sha": "${git_sha}",
  "git_branch": "${git_branch}",
  "build_timestamp_utc": "${build_timestamp_utc}",
  "ge_version": "${ge_version}",
  "lane": "${lane}",
  "builder": "${builder}"
}
EOF

    echo "ship/manifest: written"
    cat "${manifest_path}"

    if [ -n "${worktree_dir}" ]; then
        ship_manifest_embed_ios "${worktree_dir}" "${manifest_path}"
        ship_manifest_embed_android "${worktree_dir}" "${manifest_path}"
    fi

    export SHIP_MANIFEST_PATH="${manifest_path}"
}

# ---------------------------------------------------------------------------
# ship_manifest_embed_ios WORKTREE_DIR MANIFEST_PATH
#
# Copies the manifest into ios/Assets.xcassets/manifest.dataset/manifest.json
# inside the worktree, creating the dataset directory + Contents.json if
# necessary.
# ---------------------------------------------------------------------------
ship_manifest_embed_ios() {
    local worktree_dir="${1:?}"
    local manifest_path="${2:?}"
    local ios_xcassets="${worktree_dir}/ios/Assets.xcassets"

    if [ ! -d "${ios_xcassets}" ]; then
        echo "ship/manifest: ios/Assets.xcassets not found in ${worktree_dir} — skipping iOS embed"
        return 0
    fi

    local dataset_dir="${ios_xcassets}/manifest.dataset"
    mkdir -p "${dataset_dir}"

    cp "${manifest_path}" "${dataset_dir}/manifest.json"

    # Write a minimal Xcode dataset Contents.json so Xcode recognises it.
    if [ ! -f "${dataset_dir}/Contents.json" ]; then
        cat > "${dataset_dir}/Contents.json" <<'CONTENTS'
{
  "data" : [
    {
      "filename" : "manifest.json",
      "idiom" : "universal"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
CONTENTS
    fi

    echo "ship/manifest: embedded into ${dataset_dir}/manifest.json"
}

# ---------------------------------------------------------------------------
# ship_manifest_embed_android WORKTREE_DIR MANIFEST_PATH
#
# Copies the manifest into android/app/src/main/res/raw/manifest.json inside
# the worktree.
# ---------------------------------------------------------------------------
ship_manifest_embed_android() {
    local worktree_dir="${1:?}"
    local manifest_path="${2:?}"
    local android_res="${worktree_dir}/android/app/src/main/res"

    if [ ! -d "${android_res}" ]; then
        echo "ship/manifest: android/app/src/main/res not found in ${worktree_dir} — skipping Android embed"
        return 0
    fi

    local raw_dir="${android_res}/raw"
    mkdir -p "${raw_dir}"
    cp "${manifest_path}" "${raw_dir}/manifest.json"
    echo "ship/manifest: embedded into ${raw_dir}/manifest.json"
}

# ---------------------------------------------------------------------------
# Standalone mode
# ---------------------------------------------------------------------------
# Guard: only run when EXECUTED directly, not when SOURCED from
# release.sh / build.sh. Same fix as worktree.sh — without this, `$1`
# inherits the caller's argv and trips the unknown-arg path.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    case "${1:-}" in
        --write)
            tag="${2:?Usage: manifest.sh --write TAG LANE [WORKTREE_DIR]}"
            lane="${3:?Usage: manifest.sh --write TAG LANE [WORKTREE_DIR]}"
            worktree="${4:-}"
            ship_manifest_write "${tag}" "${lane}" "${worktree}"
            ;;
        --embed-ios)
            ship_manifest_embed_ios "${2:?}" "${3:?}"
            ;;
        --embed-android)
            ship_manifest_embed_android "${2:?}" "${3:?}"
            ;;
        *)
            echo "Usage: manifest.sh --write TAG LANE [WORKTREE_DIR]" >&2
            echo "       manifest.sh --embed-ios WORKTREE_DIR MANIFEST_PATH" >&2
            echo "       manifest.sh --embed-android WORKTREE_DIR MANIFEST_PATH" >&2
            exit 1
            ;;
    esac
fi

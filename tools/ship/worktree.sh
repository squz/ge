#!/usr/bin/env bash
# tools/ship/worktree.sh — 🎯T64.4 worktree-isolated build primitive
#
# Creates (or re-uses) build/ship/<TAG>/ as a git worktree checked out to
# exactly the given tag. Submodules are initialised recursively inside the
# worktree so the build is hermetic — WIP in the developer's working copy
# cannot leak into a release.
#
# Usage (called by other ship scripts, not directly by humans):
#
#   # Create and enter a worktree for the given tag
#   source "$(dirname "$0")/worktree.sh"
#   ship_worktree_create TAG
#       → sets SHIP_WORKTREE_DIR, exports it
#   ship_worktree_remove            # removes on success
#   ship_worktree_remove_on_failure # called from ERR trap; leaves for inspection
#
# Standalone mode — `make ship-worktree TAG=v0.4.0` calls:
#   ./tools/ship/worktree.sh --create v0.4.0
#   ./tools/ship/worktree.sh --remove v0.4.0

set -euo pipefail

# REPO_ROOT = consuming project root when ge is a submodule, else ge's own root.
# See preflight.sh for the rationale.
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-superproject-working-tree 2>/dev/null)"
if [ -z "${REPO_ROOT}" ]; then
    REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
fi
SHIP_BUILD_DIR="${REPO_ROOT}/build/ship"

# ---------------------------------------------------------------------------
# ship_worktree_create TAG
#
# Creates build/ship/<TAG>/ as a git worktree at the given tag.
# Initialises ge submodule recursively inside the worktree.
# Sets and exports SHIP_WORKTREE_DIR.
# ---------------------------------------------------------------------------
ship_worktree_create() {
    local tag="${1:?ship_worktree_create requires TAG}"
    local worktree_dir="${SHIP_BUILD_DIR}/${tag}"
    export SHIP_WORKTREE_DIR="${worktree_dir}"

    # Verify the tag exists locally; if not, fetch it.
    if ! git -C "${REPO_ROOT}" rev-parse --verify "refs/tags/${tag}" >/dev/null 2>&1; then
        echo "ship/worktree: tag ${tag} not found locally; fetching..."
        git -C "${REPO_ROOT}" fetch --tags origin
    fi

    if [ -d "${worktree_dir}" ]; then
        # Worktree already exists (left from a previous failed build). Check
        # it's pointing at the right tag.
        local existing_sha
        existing_sha="$(git -C "${worktree_dir}" rev-parse HEAD 2>/dev/null || true)"
        local tag_sha
        tag_sha="$(git -C "${REPO_ROOT}" rev-parse "refs/tags/${tag}^{}" 2>/dev/null)"
        if [ "${existing_sha}" = "${tag_sha}" ]; then
            echo "ship/worktree: reusing existing worktree at ${worktree_dir}"
            return 0
        fi
        echo "ship/worktree: stale worktree at ${worktree_dir} (expected ${tag_sha:0:7}, got ${existing_sha:0:7})"
        echo "  Removing stale worktree before recreating..."
        git -C "${REPO_ROOT}" worktree remove --force "${worktree_dir}" 2>/dev/null || rm -rf "${worktree_dir}"
    fi

    echo "ship/worktree: creating build/ship/${tag}/"
    mkdir -p "${SHIP_BUILD_DIR}"
    git -C "${REPO_ROOT}" worktree add --detach "${worktree_dir}" "refs/tags/${tag}"

    echo "ship/worktree: initialising submodules inside ${worktree_dir}"
    git -C "${worktree_dir}" submodule update --init --recursive
}

# ---------------------------------------------------------------------------
# ship_worktree_remove [TAG]
#
# Removes the worktree on success. If TAG is not supplied, uses
# $SHIP_WORKTREE_DIR (set by ship_worktree_create).
# ---------------------------------------------------------------------------
ship_worktree_remove() {
    local worktree_dir="${1:-${SHIP_WORKTREE_DIR:-}}"
    if [ -z "${worktree_dir}" ]; then
        echo "ship/worktree: nothing to remove (SHIP_WORKTREE_DIR not set)"
        return 0
    fi
    if [ ! -d "${worktree_dir}" ]; then
        return 0
    fi
    echo "ship/worktree: removing ${worktree_dir}"
    git -C "${REPO_ROOT}" worktree remove --force "${worktree_dir}" 2>/dev/null \
        || rm -rf "${worktree_dir}"
    git -C "${REPO_ROOT}" worktree prune
}

# ---------------------------------------------------------------------------
# ship_worktree_remove_on_failure [TAG]
#
# Called by the ERR trap in the orchestrating scripts. Leaves the worktree
# in place for inspection and prints a clear message about how to clean it.
# ---------------------------------------------------------------------------
ship_worktree_remove_on_failure() {
    local worktree_dir="${1:-${SHIP_WORKTREE_DIR:-}}"
    if [ -n "${worktree_dir}" ] && [ -d "${worktree_dir}" ]; then
        echo ""
        echo "ship/worktree: BUILD FAILED — worktree left for inspection at:"
        echo "  ${worktree_dir}"
        echo ""
        echo "To remove it manually:"
        echo "  git worktree remove --force '${worktree_dir}'"
        echo "  # or: make ship-clean"
    fi
}

# ---------------------------------------------------------------------------
# ship_worktree_clean [--max-age-days N]
#
# Prunes all build/ship/<tag>/ worktrees older than N days (default 14).
# ---------------------------------------------------------------------------
ship_worktree_clean() {
    local max_age_days=14
    while [ $# -gt 0 ]; do
        case "$1" in
            --max-age-days) max_age_days="${2:?}"; shift 2 ;;
            *) shift ;;
        esac
    done

    if [ ! -d "${SHIP_BUILD_DIR}" ]; then
        echo "ship/worktree: nothing to clean (${SHIP_BUILD_DIR} not found)"
        return 0
    fi

    echo "ship/worktree: pruning worktrees older than ${max_age_days} days in ${SHIP_BUILD_DIR}"
    local count=0
    for worktree_dir in "${SHIP_BUILD_DIR}"/*/; do
        [ -d "${worktree_dir}" ] || continue
        local age_days
        # mtime of the directory root — if we couldn't stat it, skip.
        if ! age_days="$(find "${worktree_dir}" -maxdepth 0 -mtime "+${max_age_days}" 2>/dev/null)"; then
            continue
        fi
        if [ -n "${age_days}" ]; then
            echo "  removing: ${worktree_dir}"
            git -C "${REPO_ROOT}" worktree remove --force "${worktree_dir}" 2>/dev/null \
                || rm -rf "${worktree_dir}"
            count=$((count + 1))
        fi
    done
    git -C "${REPO_ROOT}" worktree prune
    echo "ship/worktree: pruned ${count} stale worktree(s)"
}

# ---------------------------------------------------------------------------
# Standalone mode
# ---------------------------------------------------------------------------
# Guard so this only runs when EXECUTED directly, not when SOURCED from
# release.sh. Without the guard, `$1` here inherits the parent script's
# argv (e.g. "--lane") and trips the unknown-arg path.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    case "${1:-}" in
        --create)
            ship_worktree_create "${2:?Usage: worktree.sh --create TAG}"
            echo "SHIP_WORKTREE_DIR=${SHIP_WORKTREE_DIR}"
            ;;
        --remove)
            ship_worktree_remove "${2:-}"
            ;;
        --clean)
            shift
            ship_worktree_clean "$@"
            ;;
        *)
            echo "Usage: worktree.sh --create TAG | --remove [TAG] | --clean [--max-age-days N]" >&2
            exit 1
            ;;
    esac
fi

#!/usr/bin/env bash
# Forward this checkout's bullseye.yaml onto master (🎯T200).
#
# The companion to tools/check-ledger-parity.sh. A loud refusal with no cheap
# remedy is a refusal people learn to bypass, so the check's error message
# points here and this does the whole job in one command from any worktree.
#
# It never switches your branch, never touches your index, and never commits
# inside somebody else's worktree: when master is not checked out here, the
# commit is built with plumbing (read-tree into a scratch index, commit-tree,
# compare-and-swap update-ref) so no working tree is disturbed at all.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

LEDGER="bullseye.yaml"
BASE="${GE_LEDGER_BASE:-master}"

cd "$(git rev-parse --show-toplevel)"
HERE="$PWD"

[[ -f "$LEDGER" ]] || { echo "land-ledger: no $LEDGER here" >&2; exit 1; }
git rev-parse --verify --quiet "refs/heads/$BASE" >/dev/null \
  || { echo "land-ledger: no '$BASE' branch in this repo" >&2; exit 1; }

old="$(git rev-parse "$BASE")"
work_blob="$(git hash-object "$LEDGER")"

if [[ "$work_blob" == "$(git rev-parse --verify --quiet "$BASE:$LEDGER" || true)" ]]; then
  echo "✓ $BASE:$LEDGER already matches this copy — nothing to land"
  exit 0
fi

msg="${GE_LEDGER_MSG:-Forward the ledger onto $BASE}"

# Who has $BASE checked out? Committing into another worktree is off-limits,
# and moving the ref under one would leave its index silently stale.
# (temp file, not `< <(...)`: macOS ships bash 3.2, where process
# substitution is a runtime trap rather than a syntax error.)
holder=""
wt=""
wtlist="$(mktemp)"
trap 'rm -f "$wtlist"' EXIT
git worktree list --porcelain > "$wtlist"
while IFS= read -r line; do
  case "$line" in
    worktree\ *) wt="${line#worktree }" ;;
    branch\ refs/heads/"$BASE") holder="$wt" ;;
  esac
done < "$wtlist"
rm -f "$wtlist"

if [[ -n "$holder" && "$holder" != "$HERE" ]]; then
  echo "land-ledger: '$BASE' is checked out at" >&2
  echo "    $holder" >&2
  echo "Landing from here would leave that worktree's index stale." >&2
  echo "Run this from there, or detach it first:" >&2
  echo "    git -C '$holder' switch --detach" >&2
  exit 1
fi

if [[ "$holder" == "$HERE" ]]; then
  # Simple case: we are on $BASE. A normal commit of one path.
  git add -- "$LEDGER"
  git commit -q -m "$msg"
  echo "✓ landed on $BASE: $(git rev-parse --short HEAD)"
  exit 0
fi

# We are elsewhere. Build the commit without touching any working tree.
blob="$(git hash-object -w "$LEDGER")"
idx="$(mktemp)"
trap 'rm -f "$idx" "$wtlist"' EXIT
GIT_INDEX_FILE="$idx" git read-tree "$BASE"
GIT_INDEX_FILE="$idx" git update-index --cacheinfo "100644,$blob,$LEDGER"
tree="$(GIT_INDEX_FILE="$idx" git write-tree)"
new="$(git commit-tree "$tree" -p "$old" -m "$msg")"
# Compare-and-swap: refuse if someone else moved $BASE since we read it.
git update-ref "refs/heads/$BASE" "$new" "$old"
echo "✓ landed on $BASE: $(git rev-parse --short "$BASE")  (built without checking out $BASE)"

#!/usr/bin/env bash
# Refuse to let a bullseye filing live only in a working copy (🎯T200).
#
# ge is worked from a dozen worktrees at once and every one carries its own
# `bullseye.yaml`. `bullseye` writes the ledger into whatever checkout it is
# pointed at (`--cwd`), so a filing is complete, silent and invisible the
# moment it is written — no error, no diff anyone reads, no id anyone else
# can see. 🎯T188 wrote that rule into AGENTS.md; it held for under an hour,
# and three actors (one of them the PO who wrote the rule) re-forked the
# ledger the same evening. Two of them filed the SAME defect minutes apart
# because neither filing was visible to the other.
#
# bullseye's own write path already refuses a DETACHED-HEAD checkout
# (code=unsafe_repo). This covers the case that guard misses: a write onto a
# named branch that is not master.
#
# The rule, stated as the thing that is actually lethal:
#
#     Ledger content must exist in at least one commit. Content that exists
#     ONLY in a working copy has reached nothing and nobody.
#
# So this errors when the working-tree ledger differs from BOTH `HEAD` and
# `master`. That is precisely the "filed and left behind" shape, and it is
# deliberately the only hard failure:
#
#   - a worktree sitting at an older commit  (working == HEAD)   → fine
#   - a feature branch carrying its filings  (working == HEAD)   → fine,
#     because this repo's lifecycle rule is that the ledger rides the commit
#     that changes it, and reaches master when that branch does
#   - a checkout already at parity          (working == master)  → fine
#
# A branch whose ledger is committed but not yet on master gets a WARNING,
# not a failure: it is visible in git and recoverable, unlike the working-copy
# case. Blocking it would fight the documented branch workflow.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

LEDGER="bullseye.yaml"
BASE="${GE_LEDGER_BASE:-master}"

TMP_BASE="$(mktemp)"; TMP_MINE="$(mktemp)"; TMP_THEIRS="$(mktemp)"
trap 'rm -f "$TMP_BASE" "$TMP_MINE" "$TMP_THEIRS"' EXIT

cd "$(git rev-parse --show-toplevel)"

# Consumers vendor ge as a submodule: detached HEAD, no local `master`, and
# often no ledger at all. Skipping is not a silently-disabled feature — there
# is genuinely nothing to enforce in a checkout with no ledger branch.
if [[ ! -f "$LEDGER" ]]; then
  echo "ledger-check: no $LEDGER in this repo — nothing to enforce"
  exit 0
fi
if ! git rev-parse --verify --quiet "refs/heads/$BASE" >/dev/null; then
  echo "ledger-check: no '$BASE' branch here (submodule or partial clone) — nothing to enforce"
  exit 0
fi

work_blob="$(git hash-object "$LEDGER")"
base_blob="$(git rev-parse --verify --quiet "$BASE:$LEDGER" || true)"
head_blob="$(git rev-parse --verify --quiet "HEAD:$LEDGER" || true)"

if [[ "$work_blob" == "$base_blob" ]]; then
  echo "✓ ledger matches $BASE:$LEDGER"
  exit 0
fi

if [[ "$work_blob" == "$head_blob" ]]; then
  # Committed here, just not on master yet. Visible and recoverable.
  branch="$(git symbolic-ref --quiet --short HEAD || echo 'detached HEAD')"
  echo "ℹ ledger differs from $BASE but is committed on $branch —"
  echo "  it reaches $BASE when this branch does. Not a fork."
  exit 0
fi

# Working-copy-only content: the lethal shape.
branch="$(git symbolic-ref --quiet --short HEAD || echo 'detached HEAD')"
{
  echo ""
  echo "================================================================"
  echo "✗ LEDGER FORKED — a filing is sitting in this working copy only"
  echo ""
  echo "  repo:   $PWD"
  echo "  branch: $branch"
  echo ""
  echo "  $LEDGER here matches neither HEAD nor $BASE, so this content"
  echo "  exists in no commit. Nothing else in the project can see it:"
  echo "  not the frontier, not another worktree, not the next agent."
  echo "  Another checkout filing right now will mint the same ids."
  echo ""
  echo "  Targets present here but not on $BASE:"
  # Target ids are the `  T<id>:` keys. `grep -c` style counting would hide
  # which ones, and the id set is what tells you whether this is a new filing
  # or an edit to an existing target.
  git show "$BASE:$LEDGER" > "$TMP_BASE"
  grep -oE '^  T[0-9A-Za-z.]+:' "$LEDGER"    | sort > "$TMP_MINE"
  grep -oE '^  T[0-9A-Za-z.]+:' "$TMP_BASE"  | sort > "$TMP_THEIRS"
  comm -23 "$TMP_MINE" "$TMP_THEIRS" | sed 's/^/    + /' > "$TMP_BASE"
  if [[ -s "$TMP_BASE" ]]; then
    cat "$TMP_BASE"
  else
    echo "    (no new ids — the difference is in existing targets' bodies)"
  fi
  echo ""
  echo "  Land it:"
  echo "      make ledger-land"
  echo ""
  echo "  Or, if this copy is stale rather than newly filed, discard it:"
  echo "      git checkout $BASE -- $LEDGER"
  echo "================================================================"
  echo ""
} >&2
exit 1

---
name: release-notes
description: Draft CHANGELOG bullets from git log since the last tag, grouped by 🎯T-prefix when present.
user-invocable: true
---

# /ge:release-notes

Draft release notes from git history. Idempotent — safe to run multiple times.

## Step 1: Find range

```bash
git tag -l | sort -V | tail -1
```

Use the result as `<last-tag>`. If no tags exist, use the first commit.

## Step 2: Gather commits

```bash
git log <last-tag>..HEAD --oneline
```

Parse each line as `<sha> <subject>`. Do not re-run git log; use this output for
all subsequent steps.

## Step 3: Group and draft

Group commit subjects by 🎯T-prefix when present. A commit subject containing
`🎯T<N>` (or `T<N>` in parentheses, e.g. `(🎯T42)`) belongs to that target group.
Commits with no 🎯T reference go into an "Other" group.

Within each group, write one bullet per logical change (not per commit — collapse
fixup/typo commits into the substantive entry they fix).

Output format:

```
## Changes since <last-tag>

### 🎯T<N> — <target name if known>
- <bullet>

### Other
- <bullet>
```

If there are fewer than ~6 commits total, skip grouping and emit a flat bullet
list — forced grouping of tiny changelogs reads worse than a simple list.

## Step 4: Present

Print the draft to the transcript. Do not write it to a file unless the user
asks. Do not halt for approval — the output is a draft for the user to refine.

Note at the bottom: "Edit as needed, then pass to `/ge:ship` or use in
`make ship-release VERSION=vX.Y.Z`."

## Notes

- Do not include merge commits, dependency bumps, or CI-only changes unless
  they represent a meaningful behaviour change.
- Commit subjects that start with `Merge`, `chore:`, `ci:`, or `deps:` are
  candidates for omission — use judgment.
- If `git log` output is empty (HEAD == last-tag), report "No commits since
  <last-tag>" and stop.

---
name: ship-status
description: Portfolio table of last-ship state across squz ge-consumer repos.
user-invocable: true
---

# /ge:ship-status

Read `build/ship/*/manifest.json` from each known squz game repo and emit a
portfolio table. Read-only — makes no changes.

## Step 1: Locate repos

The squz game repos that consume ge are:

- `~/work/github.com/squz/multimaze2`
- `~/work/github.com/squz/yourworld2`
- `~/work/github.com/squz/esfera2`

Add any repo that appears in `~/.claude/managed-repos.md` under the `squz`
org that is not listed above (check the file; the list may grow).

## Step 2: Read manifests

For each repo, glob `<repo>/build/ship/*/manifest.json`. Each manifest
documents one ship run. Read each file. Expected fields:

```json
{
  "version": "0.5.0",
  "timestamp": "2026-05-01T14:23:00Z",
  "git_sha": "abc1234",
  "lane": "alpha"
}
```

If the field names differ (the T64.2 make rules define the exact schema),
adapt — display whatever is present in a readable form.

If `build/ship/` does not exist in a repo, that repo has never shipped via
this pipeline. Note it as "no ship history".

## Step 3: Emit table

Print a Markdown table:

```
| Repo          | Lane    | Version | Timestamp           | SHA     |
|---------------|---------|---------|---------------------|---------|
| multimaze2    | release | 0.5.0   | 2026-05-01 14:23 Z  | abc1234 |
| yourworld2    | alpha   | 0.4.0   | 2026-04-28 09:11 Z  | def5678 |
| esfera2       | —       | —       | no ship history     | —       |
```

Sort rows by timestamp descending (most recently shipped first). If multiple
manifest files exist for a repo (multiple runs), show only the most recent.

## Step 4: Summary line

After the table, print one line:

```
<N> repos shipped | last ship: <repo> <version> (<lane>) at <timestamp>
```

## Error handling

- If a manifest file is malformed JSON, note "malformed manifest" for that repo
  and continue.
- If a repo directory does not exist at the expected path, note "repo not found
  at <path>" and continue — do not abort the whole scan.
- If no manifests are found anywhere, print "No ship history found across
  tracked repos."

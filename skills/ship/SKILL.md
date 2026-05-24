---
name: ship
description: Ship a new ge consumer app release (alpha / beta / release lane via fastlane).
user-invocable: true
---

# /ge:ship

Conversational orchestrator for shipping a ge-based app. Dispatches to
`make ship-alpha`, `make ship-beta VERSION=x`, or `make ship-release VERSION=x`
based on user intent. Never bypasses the make substrate.

## Step 1: Determine lane

Ask the user one question (if intent is not already clear from their message):

> "Which lane — alpha, beta, or release?"

- **alpha** — CI build, no version bump, uploaded to TestFlight internal group.
  No user confirmation needed beyond lane selection.
- **beta** — Version bump required. Confirm the next version with the user
  (compute from `git tag -l | sort -V | tail -1`, bump MINOR, zero PATCH).
  Uploads to TestFlight external group.
- **release** — Version bump required. **Hard stop for user confirmation before
  dispatch** (see Step 3). Submits to App Store Review.

If the user's message already states the lane clearly (e.g. "ship an alpha",
"cut a beta", "release v0.5.0"), skip the question and proceed.

## Step 2: Determine version (beta and release only)

1. Run `git tag -l | sort -V | tail -1` to find the latest tag.
2. Propose next minor: `vMAJOR.(MINOR+1).0`. Do not suggest PATCH or MAJOR
   unless the user explicitly requests it.
3. For beta: confirm with the user, then proceed.
4. For release: confirm with the user, then gate (Step 3).

## Step 3: Preflight

Run `make ship-preflight`. Surface the full output verbatim.

If preflight fails:
- Print the failure output.
- Ask: "Preflight failed. Fix and retry, or abort?"
- Do not proceed until preflight passes.

## Step 4: Dispatch

Once preflight passes, dispatch based on lane:

```
# Alpha
make ship-alpha

# Beta
make ship-beta VERSION=vMAJOR.MINOR.PATCH

# Release — only after explicit user confirmation (Step 4a)
make ship-release VERSION=vMAJOR.MINOR.PATCH
```

### Step 4a: Release confirmation gate

Before running `make ship-release`, present this summary and wait for explicit
"yes" or equivalent:

```
Ready to submit to App Store Review:
  Version: <version>
  Preflight: passed
  Lane: release

This submits to App Store Review. Proceed?
```

Do not proceed if the user says anything other than an unambiguous yes.

## Step 5: Handle make output

Stream the make output. If the make command exits non-zero:

1. Print the full error output.
2. Diagnose the root cause from the output (do not guess).
3. Propose a fix or escalation path.
4. Do not retry automatically — ask the user how to proceed.

If the make command exits 0, report success and the artifact location or
TestFlight/App Store link if emitted by the make output.

## Notes

- Always use `make ship-*` — never call fastlane, xcodebuild, or altool directly.
- The make rules are defined in the consuming project's Makefile (via T64.2).
  If `make ship-preflight` fails with "No rule to make target", the project
  has not yet wired T64.2. Tell the user and stop.
- Version strings follow `vMAJOR.MINOR.PATCH` with the `v` prefix.
- Do not modify `bullseye.yaml` or any source files as part of this skill.

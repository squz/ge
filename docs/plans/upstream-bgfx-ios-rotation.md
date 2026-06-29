# Upstreaming the iOS orientation-change fix to bgfx

> **OBSOLETE (2026-06-30, 🎯T34):** ge no longer uses bgfx — the renderer
> migrated to sokol_gfx (🎯T38), and the bgfx/bx/bimg submodules + the
> `bgfx-drawable-as-truth` patch were removed from the tree. This plan is kept
> only as a historical record; there is nothing left to upstream.

**Status:** draft plan — no issue raised yet, no fork yet.

**Goal:** get the fix in `vendor/patches/bgfx-drawable-as-truth.patch`
merged into upstream bgfx so we can drop it from our tree.

**Strategy:** raise an issue first, link a fork branch in the issue so
the maintainer can review the exact diff. If the response is
favourable, graduate to a PR.

## 0. Required context (for a fresh session)

### What we're upstreaming

A two-part fix inside `RendererContextMtl::submit()` in
`vendor/github.com/bkaradzic/bgfx/src/renderer_mtl.cpp`:

1. **Drawable-as-truth.** When there are draws to submit (`_render->m_numRenderItems > 0`), call `currentDrawableTexture()` immediately
   and adopt the returned texture's `width()`/`height()` as the
   canonical resolution for this submit (overwriting `_render->m_resolution`
   before `updateResolution` runs). Avoid acquiring the drawable on
   idle/init frames or `flip()` will present an undrawn drawable.
2. **View-rect reconciliation.** Walk the submit buffer's views;
   when a swap-chain view's `m_rect` disagrees with the real drawable
   dims, replace it with the drawable's full dims. Framebuffer-backed
   views clamp to their framebuffer. Skip the `(0,0,1,1)` default
   `View::reset` rect so `bgfx::init`'s internal frames don't open
   empty-clear encoders.

The diff in this repo: `vendor/patches/bgfx-drawable-as-truth.patch`
(authoritative). Keep it in sync with the submodule via
`scripts/apply-vendor-patches.sh`. See `vendor/patches/README.md` for
the full rationale.

### The bug it fixes

During iOS orientation change, UIKit calls `layoutSubviews` on the
main thread and mutates `CAMetalLayer.drawableSize`. bgfx runs its
render thread concurrently; the submit buffer that the render thread
is processing was prepared on the API (main) thread *before* the
`drawableSize` mutation, so its `m_resolution` and
`m_view[].m_rect` reflect the old orientation. When `submit()` runs:

- `nextDrawable()` returns a drawable at the **new** size.
- bgfx builds the render pass at the drawable's new dims.
- `setViewport` / `setScissorRect` use the **old** view rect.
- Metal validation in debug: `scissor.y + scissor.height > render pass
  height` → `-[MTLDebugRenderCommandEncoder setScissorRect:]` abort.
- Release: Metal silently renders to a sub-region; the rest of the
  drawable is uninit GPU memory → magenta strip during the animation.

### Minimal reproduction scope

Any bgfx iOS example that registers both portrait and landscape in
its `UISupportedInterfaceOrientations`, launched on a physical iPad
or iPhone, and then rotated. Adds a crash in debug and a magenta
strip in release. Simulator rotation (`⌘← / ⌘→`) does **not** hit
the race — the simulator has no rotation animation and `layoutSubviews`
fires synchronously from a safe point. Only physical devices reproduce.

### Prior bgfx work in this area

Commit `c1cd627cb` (2016, attilaz) changed drawable acquisition to
"just before needed" — i.e., moved it *out* of the top of submit,
into the per-view render-pass setup. Our patch conditionally moves
it back to the top (gated on `hasDraws`). That's a direct tension the
maintainer will notice. Acknowledge it up-front in the issue: we
preserve laziness for idle/init frames (the case the 2016 commit
likely cared about) and only eagerly acquire when we're about to
render anyway. Ask whether there's a way to get both properties
that we're missing.

### Repro patch (tiny, for the bug report)

A minimal repro patch should modify `examples/00-helloworld/helloworld.cpp`
(or another existing example) to:

- Declare both portrait and landscape in the iOS `Info.plist`.
- Keep the default `setViewRect(0, BGFX_INVALID_HANDLE)` /
  `setViewClear` pattern.
- Run on a physical device, rotate between orientations.

Expected (buggy) result: `setScissorRect` Metal validation assert in
Debug configuration. No change required to the app's draw logic — the
clear-and-touch view 0 is enough to trigger the encode path that sets
scissor.

(If the maintainer prefers the repro as a new minimal example, build
it against the vendored bgfx on the `bgfx-original` commit, then
walk through their "modify an existing example with minimal changes"
preference from `CONTRIBUTING.md`.)

## 1. Raise the issue

### Preparation

- Read recent issues/PRs tagged `platform:ios` or `backend:metal` to
  check for duplicates. Search terms:
  `drawableSize`, `layoutSubviews`, `orientation`, `rotation`,
  `setScissorRect`, `magenta`. Prior relevant commit:
  `c1cd627cb` (July 2016).
- Check open issues for anything already-tracked:
  `https://github.com/bkaradzic/bgfx/issues?q=is%3Aissue+is%3Aopen+ios+rotation`.
- Note bgfx uses Discord for one-liners (see `CONTRIBUTING.md`); this
  is multi-line, GitHub is the right channel.

### Draft issue body

Title: `Metal: race with UIKit-driven CAMetalLayer.drawableSize on iOS device rotation`

Body template (copy-paste into the GitHub issue editor, edit placeholders in angle brackets):

```
### Summary

On iOS (physical devices), rotating the device during rendering
produces either a Metal validation abort (`setScissorRect`: scissor
height exceeds render pass height) in Debug, or a magenta strip
during the rotation animation in Release. The root cause is that
`CAMetalLayer.drawableSize` is mutated from UIKit's `layoutSubviews`
on the main thread, between the bgfx API thread's `frame()` (which
snapshots `m_resolution` and `m_view[].m_rect` into the submit
buffer) and the render thread's processing of that submit.

### Reproducer

- Device: <iPad Air 11" M2 / iPhone 14 — please fill in any device
  you've tested on>.
- iOS: <26.x>
- bgfx: <commit hash at the time the issue is raised, i.e. whatever
  `HEAD` is>.
- Xcode: <version>. Metal API Validation: ON.

Steps:

1. Run any bgfx iOS example whose Info.plist allows landscape +
   portrait orientations.
2. Rotate the device between orientations.
3. Observe: `-[MTLDebugRenderCommandEncoder setScissorRect:]:XXXX:
   failed assertion 'Set Scissor Rect Validation (rect.y(0) +
   rect.height(2360))(2360) must be <= render pass height(1640)'`.

Repeatable within a few seconds of starting the app.

### Root cause

During rotation UIKit triggers `layoutSubviews` (via SDL's
`SDL_uikitmetalview`, or equivalent for other window frameworks),
which sets `metalLayer.drawableSize = bounds.size * contentsScale`.
This is a main-thread mutation that can land at any point relative
to the bgfx render thread. If it lands while the render thread is
inside `submit()` processing a frame that was committed before the
mutation:

- `nextDrawable()` hands back a drawable at the **new** dimensions.
- bgfx creates the render pass from that drawable.
- `m_resolution` in the submit buffer still reflects the **old**
  dimensions, as do `_render->m_view[v].m_rect` values set by the
  app.
- `setViewport` and `setScissorRect` use those stale rects.

Metal validates scissor against the actual render pass (drawable
texture) bounds and aborts. In Release the scissor is clipped to a
sub-region of the drawable; the remainder is uninitialized GPU
memory, visible as a magenta strip during the (~300 ms) rotation
animation.

I note commit c1cd627cb (2016) explicitly moved drawable
acquisition to "just before needed" — my local fix reverses that
partially (acquires at the top of `submit()`, gated on
`_render->m_numRenderItems > 0`). Happy to be corrected on whether
there's a cleaner approach that preserves both the 2016 property
and guards the race.

### Proposed fix

Two-part change at the top of `RendererContextMtl::submit()` in
`src/renderer_mtl.cpp`:

1. Drawable-as-truth. When the app has draws this submit, acquire
   the drawable immediately via `currentDrawableTexture()` and adopt
   its texture `width`/`height` as the authoritative resolution,
   overwriting `_render->m_resolution` before `updateResolution`.
   Gated on `hasDraws` so idle/init frames don't force `flip()` to
   present an undrawn (magenta) drawable.

2. Swap-chain view-rect reconciliation. Walk `_render->m_view[*]`;
   for any view with an invalid framebuffer handle whose `m_rect`
   doesn't match the drawable dims, replace the rect with the full
   drawable. Framebuffer-backed views clamp instead. Skip the
   `View::reset` `(0,0,1,1)` default so `bgfx::init` internal frames
   don't open empty-clear encoders.

A fork branch with the full diff is linked below for review.

### Branch

<https://github.com/squz/bgfx/tree/ios-rotation-drawable-race>
(fork of `bkaradzic/bgfx`, branch `ios-rotation-drawable-race`).

Verified on iPad (ProductVersion 26.3.1): no magenta, no validation
abort across repeated portrait⇄landscape rotations. I have not yet
tested macOS live resize, visionOS, tvOS. Happy to expand coverage
and open a PR if you'd like.

### Questions

1. Does the tension with c1cd627cb suggest a better approach?
   (e.g. hoist only the drawableSize query, not the full acquisition.)
2. Do you consider the swap-chain view-rect replacement in scope for
   this fix, or should that be a separate concern left to the app?
3. Any preference on which example to adapt for a reproducer commit?
```

## 2. Fork bgfx and push the branch

### Prerequisites

- GitHub account `squz` (or `marcelocantos` — user's choice) with
  SSH keys set up for `github.com`.
- Local copy of the vendored bgfx with our patch applied (already
  the case in the submodule at
  `vendor/github.com/bkaradzic/bgfx`).
- Current submodule HEAD: the branch is `ge-fork-upgrade`, pinned to
  whichever commit the parent repo's submodule pointer references.
  Confirm with `cd vendor/github.com/bkaradzic/bgfx && git log --oneline -1`.

### Steps

1. Fork `bkaradzic/bgfx` on GitHub via the web UI (owner `squz`
   recommended so the fork lives alongside the project). Do **not**
   enable "Copy the main branch only" if asked — we need the full
   history so the maintainer can see our branch is based on a known
   commit.

2. Add the fork as a remote to our local clone:
   ```sh
   cd vendor/github.com/bkaradzic/bgfx
   git remote add squz git@github.com:squz/bgfx.git
   git fetch squz
   ```

3. Make sure our vendored tree has the patch applied (it is, but
   verify):
   ```sh
   cd "$(git rev-parse --show-toplevel)"    # in the ge repo
   ./scripts/apply-vendor-patches.sh
   # Should print: "OK    bgfx-drawable-as-truth.patch — already applied"
   ```

4. Create a clean branch off the upstream commit our submodule
   points at, then commit just the patch content on top:
   ```sh
   cd vendor/github.com/bkaradzic/bgfx
   UPSTREAM=$(git rev-parse HEAD)   # where our submodule is pinned
   git switch -c ios-rotation-drawable-race $UPSTREAM

   # Stage only the renderer_mtl.cpp change (nothing else should be
   # dirty in the submodule; if it is, git status will warn).
   git status
   git add -p src/renderer_mtl.cpp   # accept all hunks belonging to
                                      # the ge-patch markers
   ```

5. Before committing, **strip every `ge-patch` comment marker** and
   rewrite the comments in a style consistent with bgfx's own
   commentary. Look at surrounding code for tone — bgfx is terse
   and avoids multi-paragraph block comments. Suggested rewrite:

   ```cpp
   // iOS: UIKit can mutate CAMetalLayer.drawableSize from the main
   // thread during rotation, between the API thread's frame() and
   // the render thread's submit(). Adopt the drawable texture's
   // actual dimensions as the canonical resolution so the render
   // pass and view rects agree. Gated on hasDraws so idle/init
   // frames don't force flip() to present an undrawn drawable.
   ```

6. Verify the patch still applies cleanly, then commit:
   ```sh
   git diff --staged
   git commit -m "Metal: adopt drawable texture dims as canonical per-submit resolution"
   # single commit, no Co-Authored-By footer (bgfx uses simple
   # subject lines; see git log --oneline for examples).
   ```

7. Verify EditorConfig compliance — tabs for indent, no trailing
   whitespace, final newline. `git diff HEAD~1` should show clean.

8. Push to our fork:
   ```sh
   git push -u squz ios-rotation-drawable-race
   ```

9. Update the ge repo's patch file to match the upstreamable version
   (might differ slightly from our local-marker-heavy one). Rerun
   `git diff HEAD~1 > ../../../../vendor/patches/bgfx-drawable-as-truth.patch`
   from inside the submodule, or regenerate via the existing
   workflow and re-test locally before switching the ge tree to the
   new patch.

## 3. Link the branch in the issue

Once pushed, edit the issue body's `### Branch` section to point at
the exact commit on `squz/bgfx/ios-rotation-drawable-race`. Including
the commit SHA (not just the branch tip) means the maintainer can
review a stable diff even if we push more commits later.

## 4. Monitor for response

Typical bgfx triage: Branimir responds within days. Possible outcomes:

- **Thumbs up** → open a PR (section 5).
- **Maintainer wants differences** → likely candidates:
  - Prefer only hoisting `drawableSize` query (not full drawable
    acquisition) to preserve the 2016 "just-before-needed" pattern.
    We'd need to verify that alone is sufficient (it isn't in our
    testing — the race is on the drawable texture itself, not just
    the layer property — but re-test carefully on a fresh fork).
  - Prefer the view-rect reconciliation be the app's responsibility,
    not bgfx's. If so, split the PR: accept drawable-as-truth, keep
    view-rect clamp out of upstream.
  - Platform-guard the change with `#if BX_PLATFORM_IOS` so macOS
    isn't affected (we should proactively do this; our current patch
    runs on all Apple platforms).
  - Ask for a repro as an example modification. If so, fork a branch
    off an example, provide a PR against the example too.
- **No response after two weeks** → ping the Discord (`CONTRIBUTING.md`
  mentions it at `https://discord.gg/9eMbv7J`) with a one-liner: "Any
  interest in #<issue num>? Happy to PR."

## 5. Graduate to PR

When the issue conversation is positive, open the PR from the same
branch:

- Target: `bkaradzic/bgfx`, base `master`.
- Title: `Metal: adopt drawable texture dims as canonical per-submit resolution`.
- Body: reference the issue (`Fixes #<num>`), restate the rationale
  briefly, note what testing was done (device names, iOS versions,
  orientation cycles), mention the repro steps.
- Platform-guard if maintainer asked: wrap the drawable-texture
  acquisition block in `#if BX_PLATFORM_IOS` so macOS is unaffected.
- Test any example the maintainer suggests, document the result.
- Tick the `CONTRIBUTING.md` checklist: tested against examples,
  focused scope, EditorConfig respected.

## 6. After merge

1. Our vendored `bgfx` submodule bump picks up the upstream version.
2. `./scripts/apply-vendor-patches.sh` now sees the patch is already
   applied upstream → delete
   `vendor/patches/bgfx-drawable-as-truth.patch`.
3. Update `vendor/patches/README.md` removing the entry.
4. If `vendor/patches/` is empty after removal, consider deleting the
   directory and the apply script. Keep them if we anticipate future
   vendored patches.

## Fallback: if the PR is rejected

Maintainer may decline to merge — e.g., considers this the app's
responsibility or wants a very different shape. In that case:

- Keep our local patch in `vendor/patches/`.
- Update `vendor/patches/README.md` noting the attempt and the
  maintainer's reasoning, so future us doesn't re-litigate.
- Consider whether it's worth exposing a `BGFX_CONFIG_*` flag that
  activates the fix, and trying again against that bar.

## Artifacts to keep consistent

- `vendor/patches/bgfx-drawable-as-truth.patch` — authoritative diff.
- `vendor/patches/README.md` — rationale.
- `scripts/apply-vendor-patches.sh` — applier.
- `vendor/github.com/bkaradzic/bgfx/src/renderer_mtl.cpp` — working
  tree (modified), not staged for commit in the submodule.
- Parent repo commit `1471f9d` — landed the fix in ge.

Do not commit the patch into the vendored bgfx submodule locally — the
submodule should stay on its upstream branch/commit so
`scripts/apply-vendor-patches.sh` can do the right thing on rebase.

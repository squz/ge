# Rendering library choice

**Decision:** Migrate to sokol_gfx, deferred. Reversed 2026-05-04.

**Status:** bgfx remains in use. Migration deferred until esfera2, multimaze2, and yourworld2 have all shipped — mid-flight renderer migration would destabilise active release branches. See "Reversal (2026-05-04)" below for the trigger and "When to start" for the unblocking conditions.

---

**Original decision (2026-04-20):** Stay with bgfx.

## Context

ge has been through one rendering-library migration already (Dawn/WebGPU → bgfx, 🎯T53, achieved 2026-03-26). This document captures a subsequent broader re-evaluation of the alternatives, prompted by a general discussion of declarative rendering APIs and the state of the art in open-source rendering libraries.

The conclusion is to stay with bgfx. This document records the reasoning so the choice is revisited only when the underlying constraints change, not re-litigated from scratch.

## Why the question came up

Two prompts:

1. bgfx has a 2011-era design (view-based ordering, no real render graph, no mesh shaders, pre-bindless). Modern alternatives (The Forge, Diligent Engine, wgpu-native, Dawn, sokol_gfx, Filament) were worth benchmarking against.
2. ge recently changed its distribution architecture: the original Dawn-wire remote-rendering protocol was replaced by H.264 frame streaming (🎯T52). The primary rationale for choosing Dawn in the first place — `dawn_wire`'s production-hardened command serialization — no longer applies.

## History: what drove the original WebGPU choice

The Dawn/WebGPU era was built around **remote rendering via `dawn_wire`**. Architecture:

```
game → WGPU calls → dawn_wire serialize → stream relay → player deserialize → native GPU
```

This leveraged Chrome's own wire protocol, already production-hardened by the browser sandbox model. Nothing else in the ecosystem offered an equivalent: a standards-based, auto-generated serialization of a modern GPU API.

Secondary factors: WebGPU as a durable, multi-org-backed standard; a single shader language (WGSL) across platforms.

## Why the original rationale no longer applies

🎯T52 replaced the wire protocol with H.264 frame streaming: the server renders normally and encodes frames; the player decodes video. This is simpler, universally supported (VideoToolbox, MediaCodec, WebCodecs), and doesn't require ge to own a full GPU abstraction for serialization.

Consequence: `dawn_wire` — the load-bearing reason to use Dawn — became dead weight. The bgfx migration was a direct consequence of removing the wire protocol, not a comparison of bgfx against Dawn on ergonomic or capability grounds.

Summary of what survives:

| Factor | Applies now? |
|---|---|
| `dawn_wire` for remote rendering | No — H.264 replaced it |
| ge owning a full GPU abstraction | No — unnecessary without wire serialization |
| WebGPU API verbosity | Yes, still a real pain |
| WebGPU as a durable standard | Yes, arguably strengthened |
| WGSL as a single shader language | Yes, but `.sc` / sokol-shdc cover this |

## The alternatives

Evaluated for an engine that will ship many titles across iOS, Android, desktop (dev), and potentially web/macOS-store.

| Library | Strengths | Fit |
|---|---|---|
| **bgfx** | 15-yr track record, GLES 3.2 first-class, unified `.sc` shader language, Branimir remarkably consistent solo maintainer, already integrated | **Chosen** |
| **sokol_gfx** | Smaller (~10K LOC), cleaner API design, no Vulkan on Android, easy to fully understand and fork | **Closest runner-up** |
| **Diligent Engine** | Modern render-graph-friendly design, bindless-first, MIT | GLES backend less tested on mobile |
| **The Forge** | AAA-shipped pedigree, mesh shaders, work graphs | Vulkan-primary on Android — no escape hatch |
| **Dawn (WebGPU)** | W3C standard, multi-org backing | Vulkan-only on Android — no escape hatch |
| **wgpu-native (WebGPU)** | Standards-backed, Rust + C bindings | Vulkan-primary; GLES fallback experimental |
| **Filament** | Google-built, GLES default on Android, battle-tested | Different category — it's a renderer, not a rendering library |
| **NVRHI** | Modern, render-graph-native | No mobile story |

## The Adreno Vulkan hazard

A finding from 🎯T24 (2026-04-18) reshaped the evaluation. On the Asus ASUSAI2501 (Snapdragon 8 Elite Gen, Adreno 830, Android 16), bgfx's Vulkan backend crashes with a null pointer dereference inside `vulkan.adreno.so` — the Qualcomm driver itself, not bgfx. Details in [`papers/adreno-830-bgfx-vulkan-crash.md`](papers/adreno-830-bgfx-vulkan-crash.md).

Two important properties of this bug:

1. **It's driver-level.** The bug affects any Vulkan consumer on that hardware, not bgfx specifically. Dawn, wgpu, The Forge, and native Vulkan apps would all hit it.
2. **It's an ecosystem-wide problem.** Adreno's Vulkan stack has a long history of bgfx-incompatible quirks and is widely reported as flaky. The Qualcomm Vulkan path matures slowly because everyone historically shipped GLES and the driver team's energy is elsewhere.

**bgfx's escape hatch:** one line — `init.type = RendererType::OpenGLES`. The GLES 3.2 backend is mature, predates bgfx's Vulkan support by years, and is the industry's most-debugged path on Adreno.

**What other libraries give you:**

| Library | Android backend | Escape hatch |
|---|---|---|
| bgfx | GLES / Vulkan | ✅ One-line switch |
| sokol_gfx | **GLES only** | ✅ Built-in — Vulkan never used |
| Filament | GLES default, Vulkan opt-in | ✅ Default is the safe path |
| Diligent | GLES possible | ⚠️ Less tested |
| wgpu-native | Vulkan primary, GLES experimental | ⚠️ Rough |
| Dawn | Vulkan only | ❌ None |
| The Forge | Vulkan primary | ❌ Effectively none |

The broader principle: on Android, **API modernity and shipping durability are in active tension**. Vulkan 1.3 is the technically correct answer; the Qualcomm Vulkan driver is where most of your Android audience lives and it's full of landmines. Libraries that treat GLES as a first-class citizen (rather than a legacy fallback) gain a structural durability boost that doesn't show up in feature comparisons. bgfx's vintage — often framed as a drawback — is precisely what saves it here.

## The bgfx vs. sokol_gfx shootout

The Adreno analysis narrowed the field to bgfx and sokol_gfx. Filament is a different category (a PBR renderer with opinions, not a rendering library to build a renderer on). The remaining alternatives either expose us to the Adreno Vulkan hazard or lack the mobile story.

**Where they meaningfully differ:**

- **Size:** bgfx ~200K LOC across many backends; sokol_gfx ~10K LOC single header.
- **API model:** bgfx's view-based ordering is pragmatic but dated; sokol_gfx's pipeline-object model is cleaner and more principled.
- **Shader tooling:** bgfx uses custom `.sc` + shaderc; sokol uses GLSL + sokol-shdc + SPIRV-Cross to produce MSL/HLSL/GLSL/WGSL.
- **Batteries:** bgfx has built-in debug draw, text rendering, extensive examples, PBR samples. sokol's core is minimal; companion headers (`sokol_gl`, `sokol_debugtext`, `sokol_imgui`, etc.) fill specific gaps.
- **Capability ceiling:** bgfx is higher — mature compute, indirect draws, MRT. sokol's compute support is newer and more bounded.
- **Bus-factor-1 recoverability:** you could realistically read, understand, and maintain all of sokol_gfx yourself in a weekend. Forking bgfx is a real undertaking.

**Where they're effectively equivalent:**

- Platform coverage for our targets (both cover iOS Metal, Android GLES, desktop, web).
- Both sidestep the Adreno Vulkan hazard.
- Both are solo-maintained by remarkable maintainers.
- Both are leagues ahead of WebGPU on ergonomics.

**What tips the balance for ge:**

1. **Sunk-cost alignment, not sunk-cost fallacy.** bgfx is already integrated across ge, yourworld2, multimaze2, ge-t30.1. The abstraction layer is thin but paid for. A switch costs ~2–4 focused weeks.
2. **Muscle memory from 🎯T53.** We know what a renderer migration actually costs. The answer is not "nothing."
3. **Incremental wins don't justify migration.** sokol's advantages over bgfx are real but incremental (cleaner API, smaller codebase, better bus-factor recovery). None is a structural difference worth another migration.
4. **No specific bgfx pain point forces the move.** The one concrete failure mode we've hit (Adreno Vulkan crash) was resolved with a one-line bgfx config change. That's bgfx working as designed, not a motivation to leave.

## Decision

Stay with bgfx.

If we were greenfield — no existing integration, no sunk cost — sokol_gfx would be the recommendation, because it matches ge's post-🎯T52 architecture (direct rendering, no wire protocol, ergonomic-first, no Vulkan exposure on Android) more cleanly than bgfx does. But we're not greenfield, and the migration cost is the dominant factor.

## When to revisit

Switch only if one of these becomes true:

1. **bgfx stops being maintained.** Branimir is a one-person bus factor. If activity stops for >6 months, re-evaluate sokol_gfx or Diligent.
2. **A specific bgfx limitation blocks a shipped title.** Abstract ergonomic preferences don't count — something has to concretely break.
3. **The ge architecture changes again** in a way that makes a different renderer's properties structurally important (e.g., reintroducing a wire protocol would bring Dawn back into contention).
4. **Android Vulkan stabilises** across Qualcomm hardware to the point where Vulkan-primary libraries stop being a shipping risk. This would reopen The Forge, Diligent, and wgpu-native as candidates.
5. **Accumulated bgfx friction crosses a threshold** — the 2026-04-20 evaluation underweighted this. See the reversal note below.

Until then: ship titles, don't re-evaluate.

## Reversal (2026-05-04)

The 2026-04-20 decision held the line on the assumption that bgfx friction would stay episodic (one-off Adreno crash, resolved one-line). It hasn't. Concrete pain points accumulated since:

1. **View-state reuse sensitivity.** Reusing a view ID for semantically different render concepts (e.g. view 0 = playing maze on one frame, view 0 = picker chrome on the next) produces backend-specific failures. The bgfx-idiomatic fix is "one view = one render concept," but the failure mode is invisible until a backend disagrees about cached state. This entire class of bug doesn't exist in pass-descriptor-per-frame designs (sokol_gfx, WebGPU family, Diligent, The Forge, NVRHI).
2. **Backend leakage on coordinate flips and view defaults.** bgfx attempts to hide Metal/D3D vs GL/Vulkan NDC differences and `setViewClear` / `setViewScissor` defaults; the abstraction sometimes leaks. sokol_gfx is more honest about backend differences and has fewer surprise leaks as a result.
3. **The Adreno Vulkan crash (🎯T24).** Resolved by a one-line GLES switch, but it's a reminder that bgfx's Vulkan path is not the well-trodden one on Android.

None of these is individually a structural blocker. Together they're a steady drumbeat of bgfx-shaped friction that the original framing didn't account for. The fifth revisit trigger above is meant to capture this honestly.

### Why sokol_gfx specifically

Unchanged from the 2026-04-20 analysis. The runner-up assessment recorded then —

> "If we were greenfield — no existing integration, no sunk cost — sokol_gfx would be the recommendation, because it matches ge's post-🎯T52 architecture (direct rendering, no wire protocol, ergonomic-first, no Vulkan exposure on Android) more cleanly than bgfx does."

— still holds. The friction class above is a sokol-shaped solution: pass descriptors instead of retained view state, GLES-first on Android (so the Adreno hazard never re-emerges), and a small enough codebase that we can fork and patch if needed.

### When to start

Migration is gated on three release targets across consuming repos. Do not begin work on the migration while any of these is still in flight:

- **esfera2 🎯T1** — Phase 2 binary submitted to App Store (v2.1)
- **multimaze2 🎯T1** — Desktop game is polished and feature-complete
- **yourworld2** — first store submission (target TBD; treat as "yourworld2 has shipped a build to the App Store")

Once all three have shipped, the bullseye target tracking the migration moves from "identified, deferred" to the active frontier.

### Migration plan (sketch, not committed)

To be elaborated when the migration target unblocks. The 🎯T53 (Dawn → bgfx) migration cost ~2–4 focused weeks across ge + consumers; sokol's smaller surface and closer architectural fit suggest a similar estimate, with the caveats below pushing the upper bound.

Affected repos: `ge`, `yourworld2`, `multimaze2`, `esfera2`, `ge-t30.1`, plus any sample/test programs.

#### Known scope items (cost going in, not discovered later)

1. **Wrap sokol's streaming primitives in a transient-buffer-shaped helper at ge's abstraction layer.**
   bgfx's `allocTransientVertexBuffer` / `allocTransientIndexBuffer` is the per-frame dynamic-geometry pattern ge uses extensively. sokol's equivalent is `SG_USAGE_STREAM` buffers updated via `sg_update_buffer` / `sg_append_buffer`, with restrictions (one `sg_update_buffer` per frame; fixed staging-buffer size, default ~16 MB, configurable at init). The mapping is close but not identical. Audit every transient-buffer call site in ge and consumers, design a thin wrapper at ge's abstraction layer that preserves the transient-buffer mental model, and tune the staging buffer size for the heaviest frame (likely yourworld2's atlas region streaming or any level-transition particle bursts).
2. **Multi-threading regression risk.**
   bgfx has a deferred command-recording model with a separate render thread. sokol is essentially single-threaded — rendering happens on whichever thread the caller is on. Current ge workloads do not appear CPU-submission-bound, but this is a one-way door: if a future title hits CPU-submission limits on sokol, the answer is "rewrite around it" not "flip a flag." **Mitigation:** profile the heaviest scenes (yourworld2 globe + carousel; multimaze2 win-animation; any other scene with high draw-call counts) under sokol on a low-end Android device *early* in the migration, not at the end. Bail out / re-evaluate if submission cost dominates a frame.
3. **Compute is on the table now.**
   sokol's compute support has matured (dispatch, storage buffers, storage images, available across all shipping backends). Earlier framing of "compute is bgfx-only" no longer holds. ge doesn't lean on compute today, so this isn't a migration cost — but it's worth recording that the option exists post-migration for things like GPU-driven particles or culling.
4. **Debugging tooling parity.**
   bgfx and sokol both emit the same underlying debug-marker calls (`vkCmdBeginDebugUtilsLabelEXT` / `MTLCommandEncoder.pushDebugGroup` / `glPushDebugGroup`), so RenderDoc captures look comparable on either. The real difference is *what gets labelled by default*: bgfx auto-names views (a coarse render-concept grouping), while sokol labels resources (pipelines, buffers, images, passes) and requires explicit `sg_push_debug_group` / `sg_pop_debug_group` calls for coarser groupings. **Mitigation, done once at migration time:** make ge's wrapper layer enforce both — every "begin scene/view" call internally pushes a debug group with the concept name; every resource creation in ge passes a label. Consumers can't forget, and we get bgfx-equivalent annotation without ongoing discipline cost. This is a design decision, not a recurring tax.

#### Open questions to resolve during scoping

- Does ge currently rely on bgfx's multithreaded renderer, or is it effectively single-threaded already? (If the latter, the multi-threading concern above is theoretical.)
- What is the peak transient-buffer footprint in a single frame across all titles? (Drives the staging buffer sizing.)
- Are there any bgfx features in active use that I haven't inventoried — occlusion queries, indirect draws, MRT configurations, MSAA settings — that need explicit mapping?

#### Pre-migration case-study check

A few hours of research before committing: hunt for documented bgfx↔sokol_gfx migrations (in either direction). Grok's framing included the unverified claim that "many projects start with sokol and later evaluate bgfx if they hit feature walls." That's a load-bearing signal if true; cheap to falsify if not. Reasons people gave for going either direction are useful intel for what to watch for during ge's migration. Specifically look for:

- Migrations *away from* sokol (what feature wall did they hit? does ge's workload approach it?)
- Migrations *to* sokol from a similar starting point (what surprises did they hit? what took longer than expected?)
- Honest postmortems, not marketing.

If two or more credible reports point at the same sokol limitation, fold it into the scope items above before starting work.

#### Early-bailout checkpoint

After **week 1** of migration work (defined as: ge's wrapper layer ported to sokol + a single representative scene from one consumer running on it — pick the simplest title's simplest scene), pause and reassess. Decision gate:

- **Continue** if the per-scene cost is tracking toward the 2–4 week estimate and no new structural surprises have emerged.
- **Stop and reconsider** if either:
  - The actual cost is on track to exceed ~6 weeks (1.5× the upper bound), or
  - A structural mismatch has emerged that ge's wrapper layer can't paper over cleanly (e.g., a sokol limitation forces architectural changes in consumers, not just in ge).
- If we stop, options are: stay on bgfx and invest in stricter view-state discipline at ge's abstraction layer (cheaper, addresses the original pain point at the source); or re-evaluate Diligent (the bgfx-friction case for moving still applies; just not via sokol).

This checkpoint exists because we've already done one renderer migration (🎯T53) and we know what sunk-cost feels like mid-migration. Naming the bailout up front makes it easier to take.

## Sources

- 🎯T52 — H.264 streaming dev mode (replaced Dawn wire)
- 🎯T53 — Migrate from Dawn/WebGPU to bgfx
- 🎯T24 — Fix Adreno 830 crash in bgfx transient buffer update (resolved by GLES switch)
- 🎯T38 — ge engine has migrated from bgfx to sokol_gfx (deferred until releases ship)
- [`papers/adreno-830-bgfx-vulkan-crash.md`](papers/adreno-830-bgfx-vulkan-crash.md)
- Broader discussion: `~/think` session 2026-04-20 covering declarative rendering APIs and OSS rendering library landscape
- Sokol vs bgfx scope-items added 2026-05-09 after Grok comparison surfaced multi-threading and transient-buffer mapping as concrete migration cost items

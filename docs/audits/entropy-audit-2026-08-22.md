# Entropy audit — ge

**Date:** 2026-08-22
**Mode:** full (entropy + hygiene)
**Auditor:** entropy-audit owner (this campaign)
**Snapshot:** `squz/ge` · branch `master` · HEAD `49f1994123468759233c740f99af2fcfca2cc580` (`49f1994 Decode UTF-8 codepoints in FreeType text rasterizer.`) · `master...origin/master [ahead 4]`

**Initial dirty state (user-owned — not modified by this audit):**

```
 M prebuilt/ios-arm64-debug/{cook.json,libge.a,manifest.json}
 M prebuilt/ios-arm64/{cook.json,libge.a,manifest.json}
 M src/hint_hand.cpp
 M src/hint_test.cpp
 M vendor/github.com/nayuki/QR-Code-generator
```

No `docs/audits/` tree existed; this report is the first file there. Prior related artefact: `docs/audit/fable-2026-07.md` (2026-07-02/03 Fable-5).

---

## Executive summary

ge is a C++20 rendering engine (sokol_gfx + SDL3) consumed as a git submodule. After Plateau P the declared architecture is sharp: **ge owns simulation/render/encode/wire/app-channel client; spyder owns inventory/launch/relay/player glass**. The *code* of the compile graph (`tools/ge-sources.mk`, `Module.mk`'s failing `player` target, `src/bridge/` server-only) largely matches that split. The *instruction surface* (AGENTS.md, STABILITY.md, agents-guide.md, several docs) still describes the pre-split world: protocol v6/v7, `player.cpp` in this repo, `ge/PLAYER`, bgfx vendor trees, `PlayerWireBridge`.

**Headline mechanism:** Plateau P moved the player and bumped the wire, but the agent-facing source of truth and the incremental-build oracle were not converged. Local `make` still does not `-include` engine `.d` files (🎯T100, set aside). GitHub CI does not run unit tests. Agents following AGENTS.md will edit files that do not exist and implement a protocol version the header has already left behind.

**Highest-consequence findings:**

- **ENT-001 (P1):** Engine/vendor/test `.d` files are emitted (`-MMD -MP`) but never `-include`d. Incremental header edits skew `libge.a` vs app objects (ODR/ABI). False comment in `Module.mk` claims GNU Make tracks them implicitly.
- **ENT-002 (P1):** Competing truths on protocol version (code **9**, STABILITY **7**, AGENTS **6**) and on where the stream player lives (spyder vs `player.cpp` / `ge/PLAYER` / `tools/ios`).
- **ENT-003 (P1):** The only live GitHub workflow on PRs/master is prebuilt-hash verify. `make unit-test`, `dispatch-null-safety-test`, and `release-surface-test` are local-only.

**Unverified residue (owner judgment, not mechanical work):**

- Whether Android `VK_ERROR_SURFACE_LOST_KHR` after background is a live device bug given OUT_OF_DATE self-heal (🎯T144 was set aside on the black-screen claim).
- Whether spyder's wire copy matches `Protocol.h` v9 (T11.1 single-source protocol is set aside; this audit did not read spyder).
- Owner-visible journeys on device (matrix cells 🎯T33* set aside). Not re-run here.
- Taste: how much historical docs (`docs/ge-remote.md`, `docs/rendering-library-choice.md`) to keep vs tombstone.

---

## Scope and exclusions

**In scope:** `include/ge/`, `src/` (engine + tests), `tools/` (build/ship/matrix, excluding generated dispatch contents except as a product of `gen.py`), `Module.mk`, `Makefile`, `tools/ge-sources.mk`, `.github/workflows/`, `scripts/hooks/`, public docs (`AGENTS.md`, `STABILITY.md`, `agents-guide.md`, `docs/*` except papers as history), `formal/`, `android-shared/`, `sample/tiltbuggy` as the in-tree consumer contract, `bullseye.yaml` status of T100/T144/T11.1, hygiene declaration.

**Named exclusions (not silent):**

| Tree | Role | Treatment |
|---|---|---|
| `vendor/github.com/**` | Submodules (sokol, SDL, spdlog, lunasvg, box2d, …) | Excluded from clone/quality judgment; listed as deps. |
| `headers/` | Lifted vendor include mirrors | Cook/prebuilt input; not engine logic. |
| `prebuilt/` | Cross-arch `libge.a` + cook manifests (LFS) | Oracle target, not source. Dirty iOS artefacts are user-owned. |
| `build/` | Local compile tree | Ignored. |
| `sample/tiltbuggy/build*`, PCM/assets | Sample products / LFS | Consumer, not engine entropy except Makefile contract. |
| `sample/tiltcal/` | Calibration sample | Noted as possible bgfx-era residue (🎯T130 set aside); not deep-read. |
| `vendor/bgfx` | **Absent** (docs still mention it) | Finding ENT-002, not an exclusion of live code. |
| `tools/sokol-dispatch/generated/` | Generated shim | Read as product of `gen.py`; not hand-edited. |
| spyder repo | Player + relay | Out of workspace; protocol duplication is inferred. |

Languages analyzed (companions read): C++ (`cpp.md`), Python (`python.md`), Bash (`bash.md`), TLA+ (`tlaplus.md`). Java/Swift/Ruby present as platform glue; no dedicated companion. No Go/Rust in first-party code.

---

## Commands run

| Command | Version / notes | Exit | Shipped vs auxiliary | Limitations |
|---|---|---|---|---|
| `git rev-parse --abbrev-ref HEAD && git rev-parse HEAD && git status --porcelain=v1 -b` | git 2.55.0 | 0 | provenance | — |
| `ls hygiene.yaml`; `ls .github/workflows` | — | 0 | hygiene | `hygiene.yaml` **absent** |
| `make dispatch-null-safety-test` | Make 3.81; Homebrew clang 22.1.8 | 0 | **shipped-path local oracle** (Makefile `bullseye` dep) | Tests only `sg_isvalid` / `sg_shutdown`, not other forwarders |
| `python3 tools/test_verify_prebuilds.py` | Python 3.13.0 | 0 | auxiliary unit tests of the verifier (15 tests) | Does not hash this tree's prebuilts |
| `python3 tools/verify-prebuilds.py` | — | **1** | **shipped-path** staleness gate | Failed: `hint_hand` / `text` inputs changed vs manifests. Mix of user-dirty `hint_hand.cpp` and commits ahead of recook. Oracle is functioning. |
| `bundle exec ruby tools/ios-build/test_build_project.rb` | 17 runs / 103 assertions | 0 | auxiliary (local `bullseye`) | Does not build Xcode for a real app |
| `python3 tools/depgraph.py --format dot --output /tmp/ge-deps` | 210 nodes, 294 edges, 0 simple cycles | 0 | auxiliary include graph | `#include` scan of `src/` + `include/`; vendor skipped |
| `~/.claude/skills/hygiene/hygiene_check.py` | uv-run hygiene skill | **1** | hygiene validator | `FileNotFoundError: hygiene.yaml` — undeclared posture |
| `jscpd --version` | — | n/a | — | **not installed**; no clone detector run (not added) |
| `make unit-test` / `make check` / device matrix | — | **not run** | — | Long; dirty tree; this audit does not mutate the working tree. Residue: doctest + matrix not re-executed. |
| bullseye `query` list/summary/target T100 | MCP | 0 | intent ledger | T176 id not found (text-thread fix landed without that id) |

Tool versions: GNU Make **3.81** (macOS; the T100 mechanism was originally proved on this Make), clang **22.1.8**, Python **3.13.0**, git **2.55.0**.

---

## Observed architecture

```
                    ┌──────────── spyder (other repo) ────────────┐
                    │ inventory, launch, dashboard, stream relay, │
                    │ native player glass (bin/player)            │
                    └────────────┬───────────────▲────────────────┘
                                 │ app-channel   │ H.264 / SP2S
                                 │ (NDEBUG off)  │ WebSocket
┌─ consumer game ────────────────────────────────┴────────────────┐
│  ge::run(Factory)                                               │
│    ├─ DirectRenderHost + SokolContext  (make / make game)       │
│    │     Apple Metal | Android VK/GLES dispatch | WebGL2        │
│    └─ runServer + ServerSession        (make server,            │
│           -DGE_SERVER_BUILD)            hidden drawable         │
│  libge.a ← tools/ge-sources.mk  (single compile-list SoT)       │
│  public API: include/ge/*.h                                     │
└─────────────────────────────────────────────────────────────────┘
```

**Entry points**

| Product | How | Host |
|---|---|---|
| Windowed game | `ge::run` → `DirectRenderHost` | local SDL/Metal/Vulkan/WebGL |
| Stream host | `make server` (`GE_SERVER_BUILD`) → `SessionHost_server.mm` + `ServerSession.mm` | console + spyder relay |
| Stream glass | **not in this repo** (`Module.mk` `player` target exits 1) | spyder |
| Headless PNG | `ge::renderToPng` / `renderBatch` | hidden DirectRenderHost |
| Cook tools | `bin/ge-texenc`, `ge-icon-gen` | host CLI |

**Declared vs observed rules**

| Rule | Status |
|---|---|
| ge vs spyder ownership (AGENTS.md Architecture) | **Agree** with `ge-sources.mk` brokered list, failing `player` target, no `tools/ios` player tree |
| `tools/ge-sources.mk` is the single compile list | **Enforced** (Module.mk include + `prebuild.sh` print targets) |
| Header change rebuilds every dependent `.o` (🎯T100) | **Contradicted** — `.d` emitted, not included |
| Android SokolContext lifecycle hooked from DirectRenderHost | **Contradicted** — methods exist; TODOs still say “when the parallel agent lands” |
| `wire::kProtocolVersion` is the catalogue value | **Contradicted** by AGENTS.md (6) and STABILITY.md (7); code is 9 |
| Player sources live in spyder | **Agree** in AGENTS.md “Modifying the player”; **contradicted** eight lines later (`player.cpp`) and in STABILITY/agents-guide |
| Dispatch teardown is NULL-safe | **Enforced** by `make dispatch-null-safety-test` (local); other `sg_*` still unguarded |
| NDEBUG strips app-channel; default link dead-strips stream symbols | **Enforced** locally by `tools/check-release-surface.sh` (🎯T145); not in GHA |
| Prebuilt cook matches source hashes | **Enforced** by `verify-prebuilds.py` + GHA + pre-commit |
| Incremental engine objects tracked by “implicit pattern-rule dep tracking” | **False** (Make 3.81 has no such mechanism) |

**High fan-in / fan-out**

- `Module.mk` (1321 lines, 126 commits in the 2026 window): consumer contract, ship, matrix, icons, iOS/Android, tests. Highest change-amplification file.
- `include/ge/SessionHost.h`, `src/render/DirectRenderHost.mm`, `include/ge/Protocol.h`: public/runtime hubs.
- `src/appchannel.cpp` (1372 lines): per-session RPC (T175) — large but intentionally centralized.

**Cycles:** `tools/depgraph.py` produced 210 nodes / 294 edges and **no simple include cycles** among first-party headers. Platform triples (apple/android/stub) are the intended N-way implementations.

---

## Dimension vector

No scalar. Change-from-baseline is vs the 2026-07 Fable-5 audit + Plateau P, not a prior entropy report (none existed).

| Dimension | State | Evidence summary | Change from baseline |
|---|---|---|---|
| Architecture topology | concern | ge/spyder split is real in the compile graph; Android lifecycle seam and leftover public `Model.h` / `SdlContext` blur the public surface | Player code removed (improvement); docs and TODOs not converged |
| Redundancy / sources of truth | concern | Protocol version 6/7/9; player location; `WebSocketClient.h` twice; FFmpeg NOTICES vs compile list; TLA+ models `ged` | T143 bounded WS; T140/T141 teardown NULL-safe; T11.1 still set aside |
| Change amplification | concern | `Module.mk` + T100 means a header edit is a clean-build or a silent skew; protocol bump requires N doc files that already disagree | Unchanged T100 |
| Local code quality | concern | Dead `Model.h` (includes missing `Mesh.h`); stale T38-android TODOs; `recordPresent` unconditional | Fable F2–F4 teardown crash closed; F1/F8/F9 open |
| Correctness / verification | concern | Strong local oracles (doctest volume, dispatch test, release-surface, prebuild verify) **not** on GHA except prebuilds; TLA+ specs target a deleted daemon | Dispatch + WS bounds added since Fable |
| Security / dependencies | concern | No secret-scan/dependabot/CODEOWNERS; remaining unbound `sg_*` deref; T11.1 parked; FFmpeg LGPL text for a TU not in `ge-sources.mk` | WS 512 MiB cap now enforced |
| Build / release / operations | concern | Prebuild+LFS gate is real; GHA `release.yml` both jobs `if: false`; T181 (release-hosted prebuilts) converging; T100 set aside | T181 in flight (healthy direction) |
| Documentation / governance | critical | AGENTS.md is the agent SoT and is internally inconsistent on the player and three versions behind `Protocol.h`; STABILITY catalogue lists `ge/PLAYER` as **Stable** | Plateau P docs in README/STABILITY intro; body not fully rewritten |

---

## Findings

### ENT-001: Engine `.d` files are written and then ignored (🎯T100 still open)

- **Priority:** P1
- **Dimensions:** Change amplification; Correctness / verification; Build / release / operations
- **Status:** observed fact
- **Evidence:**
  - Engine pattern rule emits deps: `Module.mk:385-387` (target at 385; compile line 387 is `$(CXX) … -MMD -MP -c $< -o $@`).
  - Order-only shader comment even claims “the `-MMD` `.d` files capture the real header dependency” (`Module.mk:400`).
  - The only `-include` of any `.d` in first-party makefiles is `Module.mk:1319-1321`:
    ```
    # Dep-file include for the app's own objects. Engine object .d files are
    # already picked up by their own implicit pattern-rule dep tracking.
    -include $(APP_OBJ:.o=.d)
    ```
  - `ge/OBJ`, `ge/TEST_OBJ`, vendor objects: no `-include`. Sample tree currently has **82** `build/ge/src/*.d` files sitting unused.
  - GNU Make 3.81 (this machine) does not implicitly consume compiler `.d` files.
  - bullseye 🎯T100 remains **set_aside** (“Plateau P: incremental build hygiene”). Acceptance still requires the `-include`.
  - Fable-5 F1 proved the skew: touch `include/ge/sprite.h` → app `Renderer.o` rebuilds, `build/ge/src/sprite.o` / `libge.a` do not.
- **Mechanism:** A public-header layout/signature change recompiles consumer TUs (their `.d` *is* included) against a stale `libge.a`. Result is ODR/ABI mismatch or a false-green `unit-test` binary. Prebuilds (`tools/prebuild.sh`) compile from scratch, so CI hash-verify does not see this class of bug.
- **Blast radius:** Every desktop consumer incremental `make` / `make unit-test`. Mobile prebuilts unaffected. Silent memory corruption if layouts change and still link.
- **Counterevidence checked:** Prebuild path always recooks; T100 parked on purpose post-P; comment at 400 is wishful, not a second include. No ArchUnit-style make test exists.
- **Smallest coherent remediation:** Replace the false comment with `-include` of `$(ge/OBJ:.o=.d) $(ge/TEST_OBJ:.o=.d) $(APP_DEBUG_OBJ:.o=.d)` and the vendor groups that pass `-MMD -MP`. Add a regression: touch a header used by an engine TU + a test, `make unit-test` without `clean`, assert the engine `.o` rebuilds.
- **Verification:** Fable’s `touch include/ge/sprite.h && make` recipe; must rebuild `sprite.o` and relink `libge.a`.
- **Ratchet candidate:** Make recipe or `tools/check-incremental-deps.sh` wired into `make bullseye`; then a hygiene `make_target` once `hygiene.yaml` exists. Re-open 🎯T100 rather than leaving it set-aside while the comment stays false.

### ENT-002: Agent-facing docs and the stability catalogue disagree with the compile graph

- **Priority:** P1
- **Dimensions:** Documentation / governance; Redundancy / sources of truth; Change amplification
- **Status:** observed fact
- **Evidence:**

  Protocol version:

  | Source | Value |
  |---|---|
  | `include/ge/Protocol.h:64` | **9** (comments document v7/v8/v9) |
  | `STABILITY.md:105` and `:282` | **7** |
  | `AGENTS.md:519` (constants table) | **6** |
  | `vendor/include/sqlpipe.h` | 7 (different protocol — OK if named) |

  Player ownership (same `AGENTS.md` file):

  - `AGENTS.md:1238-1242` — “Player sources live in the **spyder** repo… Do not add player code back into ge.”
  - `AGENTS.md:1246` — “updating both `SessionHost.mm` (server side) and `player.cpp` (player side) in lockstep.”
  - `AGENTS.md:177`, `:450-453` — `player: $(ge/PLAYER)` as consumer Makefile.
  - `AGENTS.md:462-466` — `tools/player.cpp`, `tools/ios/`, `tools/android/`, `vendor/github.com/bkaradzic/{bgfx,bx,bimg}/`. **None of those paths exist.**
  - `Module.mk:478-483` — `player` / `ge/player` **exits 1** (“stream player lives in the spyder repo”).
  - `STABILITY.md:227` lists `ge/PLAYER` (`bin/player`) as **Stable**; `:241-247` still catalogue `ge/player` / `ge/player-ios*`.
  - `agents-guide.md:42-55`, `:85`, `:180-182`, `:737` — `PlayerWireBridge`, `tools/ios/`, `make ge/player`, `tools/player_core.cpp`. Headers/files absent (`include/ge/PlayerWireBridge.h` does not exist).
  - `docs/device-api.md:23` — `make player → desktop glass` as a Module.mk product.
  - `docs/rendering-library-choice.md:1-5` — “**Status:** bgfx remains in use.” Engine is sokol (🎯T38); `vendor/github.com/bkaradzic` is absent.
  - `Module.mk:104-107` — “brokered streaming sources still reference bgfx and are not being ported.” `GE_SRC_BROKERED` is sokol-era `ServerSession` / `VideoEncoder_apple.mm` (`tools/ge-sources.mk:150-155`).
  - `Protocol.h:65` still says kMaxMessageSize “matches **ged**/bridge.go”.
- **Mechanism:** Agents (and humans) load AGENTS.md first. A protocol bump or player bugfix will be applied to the wrong tree and the wrong version. The stability catalogue cannot audit breaking changes if it pins v7 while the header is v9. This is one domain fact (wire + player ownership) with four authorities.
- **Blast radius:** Every consumer integration, every protocol change, every agent session in this repo. Spyder coupling is protocol-only; a v6 client against a v9 header is a handshake reject.
- **Counterevidence checked:** README and STABILITY intro correctly describe Plateau P / no `ged`. `Module.mk` player target correctly fails. The contradiction is *inside* AGENTS.md, not only across files.
- **Smallest coherent remediation:** One pass over AGENTS.md / STABILITY.md / agents-guide.md / device-api.md: delete `ge/PLAYER` and `player.cpp` instructions; set catalogue version to 9; tombstone `docs/rendering-library-choice.md` as historical. Keep a single “player lives in spyder” paragraph.
- **Verification:** Grep gate: `rg 'ge/PLAYER|tools/player.cpp|kProtocolVersion \\| 6' AGENTS.md STABILITY.md agents-guide.md` empty; `rg 'kProtocolVersion = 9' STABILITY.md`.
- **Ratchet candidate:** `command` evidence in hygiene, or a small `tools/check-docs-protocol-version.py` that parses `Protocol.h` and fails if AGENTS/STABILITY constants tables disagree. Standing: add to `make bullseye`.

### ENT-003: GitHub CI does not run the test oracles the Makefile already has

- **Priority:** P1
- **Dimensions:** Correctness / verification; Build / release / operations
- **Status:** observed fact
- **Evidence:**
  - `.github/workflows/verify-prebuilds.yml` — only live workflow on `pull_request` + `push` to `master`. Job: `python3 tools/verify-prebuilds.py`. No compile, no doctest.
  - `.github/workflows/release.yml` — template; **both** `android` and `ios` jobs have `if: false` (`release.yml:53`, `:127`).
  - Local `Makefile:47-84` `bullseye` already runs `ruby-test`, `python-test`, `dispatch-null-safety-test`, `release-surface-test`, plus dirty-tree check. `make unit-test` exists (`Module.mk:650`) and is the doctest suite.
  - Path filter on verify-prebuilds includes `src/**` so a logic change without recook fails CI — but **recooking with a red test suite still merges**.
- **Mechanism:** The shipped-path correctness net for C++ is optional and laptop-only. Prebuilt hash equality is necessary and insufficient. A regression in `Rect` / `hint` / IAP stub can land if `libge.a` is recooked.
- **Blast radius:** All consumers of a merged `master` / tag. Pre-1.0 (STABILITY.md) does not make this acceptable for an engine.
- **Counterevidence checked:** Pre-commit + GHA prebuild gate is real and caught this tree’s stale hashes (exit 1). `dispatch-null-safety-test` and ruby/python tests are fast enough for CI. macOS unit-test needs frameworks; Linux GHA cannot run the full doctest without a new job — that is an implementation constraint, not a reason to have zero C++ tests in CI.
- **Smallest coherent remediation:** Add a GHA job on ubuntu or macos that runs `python3 tools/test_verify_prebuilds.py`, `make dispatch-null-safety-test`, and (macos) `make unit-test`. Leave `release.yml` disabled until T181/ship, but do not leave correctness un-gated.
- **Verification:** A PR that breaks `nullsafe_test.c` must fail CI; today it would not if prebuilt hashes are unchanged (that file is under `tools/sokol-dispatch/`, **not** in verify-prebuilds `paths:`).
- **Ratchet candidate:** `ci_job: verify-prebuilds.yml#…` plus a new `unit-test` job; hygiene `ci_job` once declared.

### ENT-004: Android SokolContext lifecycle is implemented and never called

- **Priority:** P2
- **Dimensions:** Architecture topology; Correctness / verification
- **Status:** observed fact (unwired); inference (driver crash); needs verification (SURFACE_LOST on device)
- **Evidence:**
  - `SokolContext_android.cpp:854-868` — `VkM::onBackground` (`vkDeviceWaitIdle`, `paused=true`); `onForeground` recreates the swapchain. GLES counterparts at `:240-256`. Public forwarders `:910-911`.
  - `DirectRenderHost.mm:948-971` — Android FOCUS_LOST/HIDDEN/MINIMIZED and RESTORED/FOCUS_GAINED/SHOWN still contain `TODO(T38-android): … when the parallel agent lands the Android backend`. Audio is routed; `sokolCtx->onBackground/onForeground` are not.
  - `DirectRenderHost::paused()` (`DirectRenderHost.mm:633-648`) returns `i_->backgrounded` **only** on iOS (`:644`); `#else` (macOS + Android) `return false` (`:648`). iOS DID_ENTER_BACKGROUND sets `backgrounded` (`:917-919`); the Android branch does not.
  - SessionHost comments (`src/SessionHost.mm:157-158`) claim Android tears the swapchain down and SDL blocks the loop.
  - Fable-5 F7 **refuted** the “permanent black screen” claim because `VkM::beginFrame/endFrame` self-heal on `VK_ERROR_OUT_OF_DATE_KHR`. 🎯T144 set aside on that basis.
- **Mechanism:** The backend’s documented contract (idle GPU, drop swapchain, recreate on foreground, skip draws while `paused`) is dead code. OUT_OF_DATE self-heal covers rotation/resize; it does not run `vkDeviceWaitIdle` before the `ANativeWindow` dies, and it does not handle `VK_ERROR_SURFACE_LOST_KHR`.
- **Blast radius:** Android Vulkan devices on background/rotate/foreground. GLES path is milder (no swapchain object).
- **Counterevidence checked:** T144 set-aside reason; Fable verifier; SDL-blocking-loop claim not independently measured here (needs-verification). iOS path *is* wired (`:925-938`).
- **Smallest coherent remediation:** Replace the two TODOs with `i_->sokolCtx->onBackground/onForeground()`; consider `paused()` true on Android while backgrounded so SessionHost skips the render bracket. Delete “when the parallel agent lands”.
- **Verification:** Log line `SokolContext: foregrounded (Vulkan)` after bg→fg on a VK device; no SURFACE_LOST spam. AVD cell or spyder smoke.
- **Ratchet candidate:** Reopen T144 as “lifecycle methods are called” (grep/architecture test) plus one device cell. Do not treat Fable’s black-screen refutation as “the seam is finished.”

### ENT-005: Dispatch shim NULL-safety is teardown-only

- **Priority:** P2
- **Dimensions:** Correctness / verification; Security / dependencies
- **Status:** observed fact
- **Evidence:**
  - `tools/sokol-dispatch/gen.py:128-152` — `NULL_SAFE_TEARDOWN = {"sg_isvalid", "sg_shutdown"}`; every other generated forwarder is `g_ge_sg_api->sg_X(...)`.
  - `tools/sokol-dispatch/generated/ge_sokol_dispatch.c:5-9` — `g_ge_sg_api = 0`; only `sg_shutdown` / `sg_isvalid` guard; `sg_setup` and ~148 others do not.
  - `tools/sokol-dispatch/nullsafe_test.c` and `make dispatch-null-safety-test` — **PASS** on this snapshot; comments describe Fable F2/F3/F4 only.
- **Mechanism:** The Fable crash (dtor after failed VK bind) is closed. Any other `sg_*` on the unbound table (failed GLES init that still submits, a future capture/trace hook, T109 instrumentation) is still a NULL deref. The generator knows how to emit guards and chooses not to for the rest.
- **Blast radius:** Android backend selection failure paths beyond the two teardown symbols; future shim users.
- **Counterevidence checked:** Destructors were the demonstrated path and they call the guarded symbols. Apple does not use the shim (`SOKOL_IMPL` in `SokolContext.mm`).
- **Smallest coherent remediation:** Default every generated forwarder to NULL-safe (no-op / zero return) *or* install a default abort-with-log table at process start. Extend `nullsafe_test.c` to call `sg_begin_pass` unbound.
- **Verification:** Unbound `sg_setup`/`sg_begin_pass` must not SIGSEGV.
- **Ratchet candidate:** Existing `make dispatch-null-safety-test` (already in `bullseye`) — widen the test; put the job on CI (ENT-003).

### ENT-006: Public `Model.h` does not compile; bgfx-era types remain in the API folder

- **Priority:** P2
- **Dimensions:** Local code quality; Redundancy / sources of truth; Documentation / governance
- **Status:** observed fact
- **Evidence:**
  - `include/ge/Model.h:5-6` includes `<ge/Mesh.h>` and `<ge/Texture.h>`. `include/ge/Mesh.h` **does not exist**.
  - On this APFS volume `include/ge/Texture.h` is the **same inode** as `include/ge/texture.h` (T167 `loadTexture` API) — the include would compile on macOS and then fail to find `ge::Texture` / `ge::Mesh`.
  - `src/Model.cpp` is the only TU including `Model.h`; it is **not** in `tools/ge-sources.mk`.
  - `AGENTS.md:955-956` still advertises `Model.h` as public API (“Associates mesh data with metadata”).
  - `src/SdlContext.cpp` implements `include/ge/SdlContext.h` but only `SdlContext_android.cpp` is in the compile list (`ge-sources.mk:84`). Desktop `SdlContext` is leftover player windowing.
  - `src/bridge/VideoDecoder_android.cpp` and `VideoDecoder_ffmpeg.cpp` exist; `GE_SRC_BROKERED` lists only `VideoDecoder_apple.mm` (`ge-sources.mk:155`).
- **Mechanism:** Public headers and AGENTS.md describe types the library does not build. Consumers (or agents) who follow the catalogue get a missing header. Case-insensitive FS hides the Texture collision until a Linux CI appears.
- **Blast radius:** Anyone including `Model.h`; Linux case-sensitive builds; license/docs for FFmpeg (see ENT-009).
- **Counterevidence checked:** `ModelFormat.h` / `MeshVertex` are live. `ge-sources.mk` comment “Add a source here exactly once” is otherwise held. Dead files are not linked into `libge.a`.
- **Smallest coherent remediation:** Remove or quarantine `Model.h` / `Model.cpp` / unused `SdlContext.cpp` (keep Android if JNI window helper is live). Drop the AGENTS.md Model bullet or retarget it at `ModelFormat.h`.
- **Verification:** `clang++ -fsyntax-only -Iinclude include/ge/Model.h` fails today; after removal, AGENTS.md must not name `Model.h`.
- **Ratchet candidate:** A “public header compiles” compile-db check; `ge-sources.mk` vs `include/ge/*.h` inventory with an allow-list for header-only types.

### ENT-007: TLA+ specs still model the deleted `ged` daemon

- **Priority:** P2
- **Dimensions:** Correctness / verification; Documentation / governance
- **Status:** observed fact
- **Evidence:**
  - `formal/GedSessionLifecycle.tla:1-11` — “TLA+ specification of the **ged daemon's** session lifecycle… mirrors the Daemon struct in **daemon.go**.”
  - `formal/GedWireHandshake.tla` (same prefix). `formal/` also contains `tla2tools.jar` and TLC traces.
  - Plateau P / 🎯T145 removed `ged`. Relay is spyder. No `daemon.go` in this repo.
- **Mechanism:** A standing formal oracle for the wrong process. Running TLC here cannot decide spyder session invariants and will not fail when ge’s wire session code regresses. Looks like coverage; is archaeology.
- **Blast radius:** Anyone citing `formal/` as session-lifecycle evidence; confusion with spyder’s session model (🎯T163 multi-session).
- **Counterevidence checked:** Specs may still document historical ged behaviour. They are not wired to `make bullseye`.
- **Smallest coherent remediation:** Move to `docs/historical/formal-ged/` or rewrite against `ServerSession.mm` / spyder (owner call). Do not leave live `formal/` names implying current enforcement.
- **Verification:** `make` / CI / bullseye must not imply TLC of ged is a ge gate unless the spec’s constants match live code.
- **Ratchet candidate:** If kept, a `make tla` job with **bounded** TLC against a spec that names live types; if not, hygiene `absent: file formal/GedSessionLifecycle.tla`.

### ENT-008: `recordPresent()` runs even when `endFrame` presented nothing

- **Priority:** P2
- **Dimensions:** Correctness / verification; Local code quality
- **Status:** observed fact (code); inference (on-demand stuck frame)
- **Evidence:**
  - `DirectRenderHost.mm:550-572` — swapchain-pass teardown always `endFrame()` then `ctx->recordPresent()`.
  - Fable-5 F8: `beginFrame` can skip opening a pass (nil Metal drawable; Vulkan OUT_OF_DATE). `endFrame` then no-ops; `recordPresent` still advances `framesPresented()` and the `renderWhenStateChanges` baseline (`Context.cpp` present accounting).
- **Mechanism:** 🎯T131.4/T131.5 treat `framesPresented()` as ground truth and the generation baseline as “this State was drawn.” A skipped present still consumes the dirty generation → on-demand idles on a stale framebuffer.
- **Blast radius:** Render-on-demand consumers (`sample/tiltbuggy` `RENDER_ON_DEMAND=1`, menus using `renderWhenStateChanges`). Tools that trust `frames_presented`.
- **Counterevidence checked:** No test asserts `recordPresent` iff present succeeded. Fable listed this as medium; not known to be filed as a live bullseye target.
- **Smallest coherent remediation:** `SokolContext::endFrame()` returns bool; gate `recordPresent()` on it. Optionally return a no-op `Pass` when beginFrame opened nothing.
- **Verification:** Stub a nil-drawable frame under `renderWhenStateChanges`; `framesPresented()` must stay flat and the trigger must remain true.
- **Ratchet candidate:** Unit test next to `Context_test.cpp` with a fake host, plus the existing `tools/render-on-demand-check.sh` assertion on present count.

### ENT-009: NOTICES.md claims Android compiles FFmpeg `VideoDecoder_ffmpeg.cpp`; the compile list does not

- **Priority:** P2
- **Dimensions:** Redundancy / sources of truth; Security / dependencies; Documentation / governance
- **Status:** observed fact
- **Evidence:**
  - `NOTICES.md:48-53` — FFmpeg prebuilts “used only in `src/bridge/VideoDecoder_ffmpeg.cpp`, which is **compiled solely for Android**,” plus LGPL static-link obligations.
  - `tools/ge-sources.mk:150-155` `GE_SRC_BROKERED` — `VideoDecoder_apple.mm` only. No ffmpeg, no `VideoDecoder_android.cpp`.
  - Prebuilts are direct-only (same file, comments `:147-149`).
- **Mechanism:** License inventory and the actual link graph disagree. Either ge still ships static FFmpeg (and the manifest is wrong — LGPL gap) or it does not (and NOTICES over-claims, plus dead decoder TUs).
- **Blast radius:** Distribution/license review; Android stream decode if someone assumes MediaCodec/FFmpeg is in `libge.a`.
- **Counterevidence checked:** Apple VideoToolbox encoder/decoder *are* in the brokered list. Android player decode lives in spyder now — consistent with Plateau P, inconsistent with NOTICES.
- **Smallest coherent remediation:** Align NOTICES with `ge-sources.mk`. If ffmpeg is unused, stop vendoring prebuilt `.a` for it or mark “reference only” like the FFmpeg submodule sentence already does for source.
- **Verification:** `nm` / cook manifest of `prebuilt/android-arm64/libge.a` has no `avcodec` symbols; NOTICES matches.
- **Ratchet candidate:** `tools/verify-cook.py` or a notices-vs-sources grep in `bullseye`.

### ENT-010: TextureEncoder `downscale2x` still divides by `n` with no empty-source guard

- **Priority:** P3
- **Dimensions:** Local code quality; Correctness / verification
- **Status:** observed fact (code); inference (needs a 0-width input)
- **Evidence:**
  - `src/TextureEncoder.cpp:21-35` — `n` stays 0 when `w==0`; `r / n` integer division.
  - `textureToFile` (`:259`) does not reject `width<=0`.
  - Fable-5 F9; offline `ge-texenc` only, not `libge.a`.
- **Mechanism:** Degenerate PNG/cook input → SIGFPE in the cook tool, not the game.
- **Blast radius:** Asset pipeline / CI cook, not runtime.
- **Counterevidence checked:** Not in `ge-sources.mk` common list (tool-only). No doctest for zero-size.
- **Smallest coherent remediation:** Reject `width<=0 || height<=0` at `textureToFile`; `n = max(n,1)` as belt.
- **Verification:** `ge::textureToFile(..., width=0, height=4)` throws; UBSan clean.
- **Ratchet candidate:** One doctest in a texenc test TU.

### ENT-011: Duplicate `WebSocketClient.h` has already drifted

- **Priority:** P3
- **Dimensions:** Redundancy / sources of truth
- **Status:** observed fact
- **Evidence:**
  - Live include: `src/bridge/WebSocketClient.cpp` and `ServerSession.mm` use `<ge/WebSocketClient.h>`.
  - `include/ge/WebSocketClient.h:24` has `setRecvTimeout`; `src/WebSocketClient.h:21-22` does not (file dated older).
  - `src/WebSocketClient.h` is tracked; no `#include` of it remains.
- **Mechanism:** Two headers for one type; the stale copy can be re-included by accident (`"WebSocketClient.h"` from `src/`).
- **Blast radius:** Wire/server builds only.
- **Counterevidence checked:** Public header is the one compiled against.
- **Smallest coherent remediation:** Delete `src/WebSocketClient.h`.
- **Verification:** `git grep WebSocketClient.h` only `include/ge/`.
- **Ratchet candidate:** None once deleted.

---

## Redundancy and competing-source-of-truth inventory

| Fact | Authorities | Drift? | Deliberate? |
|---|---|---|---|
| Stream protocol version | `Protocol.h` (9), STABILITY (7), AGENTS (6) | **Yes** | No |
| Stream player location | Module.mk fail target; AGENTS “spyder”; AGENTS `player.cpp`; STABILITY `ge/PLAYER`; agents-guide | **Yes** | Split is deliberate; extra copies are not |
| Compile file list | `tools/ge-sources.mk` (declared SoT) vs leftover `src/Model.cpp`, `SdlContext.cpp`, extra decoders | **Yes** (disk vs list) | List is SoT; leftovers are residue |
| WS payload cap | `wire::kMaxMessageSize` + `detail::wsPayloadWithinCap` (T143) vs sqlpipe’s own 64 MiB cap | Two protocols | **Yes** (different wires) |
| Magic numbers C++ vs spyder Go | `Protocol.h` vs spyder (unread) | Unknown | T11.1 set aside — accepted duplication until reopen |
| Android renderer | `SokolContext_android` + dispatch `.so`s | Single | **Yes** (sokol compile-time backend) |
| IAP stores | Stub / Local / Apple / Android | Single API `ge::iap` | **Yes** |
| Platform triples | Attitude, FontLoader, DeviceTier, RefreshRateBoost, log | Single role each | **Yes** |
| `WebSocketClient.h` | `include/ge/` vs `src/` | **Yes** | No |
| FFmpeg on Android | NOTICES vs `ge-sources.mk` | **Yes** | No |
| Session lifecycle | `ServerSession.mm` vs `formal/Ged*` | **Yes** | Specs are leftover |
| Rendering library | Code sokol vs `docs/rendering-library-choice.md` bgfx | **Yes** | Doc is stale ADR |
| Hygiene posture | (none) | n/a | Undeclared |

Deliberate duplication worth keeping: platform backends, IAP stores, sqlpipe vs stream caps, sokol VK/GLES `.so`s.

---

## Healthy structure worth retaining

- **`tools/ge-sources.mk`** as the single compile-list SoT, with goal-transparent print targets for `prebuild.sh`. Do not re-list sources in Module.mk.
- **ge vs spyder product split** in code: `player` target fails closed; brokered TUs are explicit; app-channel compiled out under NDEBUG (`tools/check-release-surface.sh`, 🎯T145).
- **Per-session scoping (🎯T174/T175)** documented in `docs/globals-audit.md` and implemented (debug queues, app-channel stores, cmdstream sinks). GPU pipelines remain process-global by sokol axiom.
- **Dispatch generator + teardown NULL-safety + `make dispatch-null-safety-test`** (this run: `PASS`). Keep the generator as the only way to edit the shim.
- **WebSocket length cap (🎯T143)** with doctest in `src/wire_input_test.cpp` (`wsPayloadWithinCap`).
- **Prebuilt cook identity:** `cook.json` + `manifest.json` + `verify-prebuilds.py` + GHA + `scripts/hooks/pre-commit` (LFS pointer check + freshness). This run’s verifier **failed** on hint/text inputs — the gate works. 🎯T181 (release-hosted prebuilts) is the right next SoT, not a second hash format.
- **Doctest volume** next to implementation (`src/*_test.cpp`; `ge/TEST_SRC` in Module.mk). Local `bundle exec ruby tools/ios-build/test_build_project.rb` 17/17.
- **Include graph** acyclic at the scanned grain (depgraph.py).
- **Pass RAII (🎯T101)** and Context rects in point space (🎯T60) as the public frame contract.
- **LICENSE** Apache-2.0; Triangle opt-in not in `libge.a`; 16 KB Android page-align flag documented and duplicated on purpose (consumer cmake vs player — player tree removed, consumer path remains).

---

## Hygiene posture

**`hygiene.yaml` is absent. Hygiene posture not declared.**

Validator:

```
~/.claude/skills/hygiene/hygiene_check.py
FileNotFoundError: .../ge/hygiene.yaml
exit 1
```

No per-dimension held tiers, floors, or drift vector. This audit did **not** initialize `hygiene.yaml`.

**Overlap with entropy (do not double-count as hygiene drift):** ENT-001..003 are the controls hygiene would eventually declare (`make_target: unit-test`, `ci_job` for dispatch/prebuilds, incremental `.d` include). ENT-002’s protocol-version grep is a future `command` evidence.

**Entropy findings suitable for future hygiene items (when the owner onboards hygiene):**

| Candidate id | Evidence kind | Ties to |
|---|---|---|
| `build.incremental-deps` | `file` Module.mk matches `-include $(ge/OBJ:.o=.d)` | ENT-001 |
| `docs.protocol-version` | `command` parser vs Protocol.h | ENT-002 |
| `correctness.unit-test-ci` | `ci_job` | ENT-003 |
| `correctness.dispatch-null-safe` | `make_target: dispatch-null-safety-test` | ENT-005 |
| `build.prebuilt-verify` | `ci_job: verify-prebuilds.yml#verify` | already live |
| `release.surface` | `make_target: release-surface-test` | T145 |

Observed **undeclared** gaps a later init would record as `planned`/`skipped`: no CODEOWNERS, no dependabot, no secret scanner, GHA release jobs disabled, no `hygiene.yaml`.

---

## Oracle coverage and residue

| Property | Decided by |
|---|---|
| Prebuilt archives match hashed sources / NDK pin | **Shipped-path:** `verify-prebuilds.py` (GHA + hook). This tree: fail (stale vs hint/text). |
| Dispatch teardown NULL-safe | **Shipped-path local:** `make dispatch-null-safety-test` PASS. Other `sg_*`: auxiliary gap (ENT-005). |
| NDEBUG strips app-channel; default binary has no encode symbols | **Auxiliary local:** `make release-surface-test` (not re-run). |
| iOS xcodeproj generator | **Auxiliary:** ruby tests PASS. |
| Verifier implementation | **Auxiliary:** `test_verify_prebuilds.py` 15/15. |
| Engine C++ behaviour (Rect, IAP stub, hint timeline, …) | **Auxiliary local:** `make unit-test` — **not run** this audit (dirty tree / length). |
| Incremental header rebuild | **Nothing** (ENT-001). Accepted risk via T100 set-aside. |
| Android bg/fg swapchain | **Manual / device** (ENT-004). T144 set aside. |
| Wire handshake vs spyder | **Nothing in this repo** (T11.1 set aside). |
| ged session invariants | **Dead auxiliary:** TLC of `formal/Ged*` (ENT-007). |
| Owner-visible device journeys | **Accepted parked:** 🎯T33* set aside. `tools/smoke-test.sh` exists. |
| Secret/CVE scan | **Nothing.** |
| Include acyclicity | **Auxiliary:** depgraph.py this run. |
| Hygiene floors | **Nothing** (undeclared). |

**Failed / skipped checks:** `verify-prebuilds.py` exit 1 (stale cook vs current sources — user dirty + ahead-4). `hygiene_check.py` exit 1 (no yaml). `make unit-test`, matrix, TLC, jscpd, device smokes **skipped**.

**Owner residue only:** (1) Accept T100 set-aside vs reopen. (2) Tombstone vs rewrite of bgfx ADR and ged TLA+. (3) Whether Android SURFACE_LOST is a ship blocker. (4) Hygiene init (yes/no). (5) Spyder protocol codegen (reopen T11.1?).

---

## Sequenced remediation

1. **Oracle seams (do first, small):**
   - `-include` engine/test/vendor `.d` files; delete the false Make comment; reopen 🎯T100 with the touch-header check (ENT-001).
   - CI job: `dispatch-null-safety-test` + `test_verify_prebuilds.py` + (macos) `unit-test` (ENT-003). Widen dispatch test (ENT-005).
2. **Converge competing truths:**
   - One docs pass: protocol **9**, player **spyder-only**, no `ge/PLAYER`, tombstone bgfx ADR, fix Protocol.h “ged/bridge.go” (ENT-002).
   - Delete `src/WebSocketClient.h`; fix or remove `Model.h` (ENT-006, ENT-011).
   - Align NOTICES with `ge-sources.mk` (ENT-009).
   - Relocate or rewrite `formal/Ged*` (ENT-007).
3. **Boundary wiring after oracles exist:**
   - Call Android `onBackground`/`onForeground`; drop T38-android TODOs (ENT-004).
   - Gate `recordPresent` on real present (ENT-008).
   - Zero-size guard in texenc (ENT-010).
4. **Ratchet:** add the grep/make/CI checks to `make bullseye`. Initialize `hygiene.yaml` **only if the owner asks**, with floors that match reality (prebuild verify held; unit-test-CI not held until ENT-003 ships).
5. **Re-run this audit** on the same finding ids and the same Protocol.h version / `.d` include definitions.

Do not start with a Module.mk rewrite (🎯T35 CMake is set aside) or with spyder protocol codegen (T11.1) — those are larger than the seams above.

---

## Comparison appendix

No prior `docs/audits/entropy-audit-*.md`. Relative to `docs/audit/fable-2026-07.md`:

| Fable id | 2026-08-22 |
|---|---|
| F1 `.d` not included | **Open** → ENT-001 (T100 still set aside) |
| F2/F3/F4 dispatch NULL on teardown | **Closed** (gen.py + nullsafe test PASS) → residue ENT-005 |
| F5 PlayerWireBridge OOB | **N/A** (player TU gone; `wire_input.h` validators remain) |
| F6 unbounded WS resize | **Closed** (T143, `WebSocketClient.cpp:188-197`) |
| F7 Android lifecycle black screen | **Refuted then; seam still unwired** → ENT-004 |
| F8 recordPresent | **Open** → ENT-008 |
| F9 texenc div0 | **Open** → ENT-010 |
)
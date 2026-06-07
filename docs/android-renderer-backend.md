# Android renderer backend: Vulkan with GLES fallback (🎯T107)

ge runs its sokol_gfx renderer on **Metal** (Apple) and **OpenGL ES 3**
(Android). This document covers the Android decision to add a **Vulkan**
backend for performance on capable hardware while keeping **GLES3 as a
guaranteed fallback** — and the *production-robust selection* that decides,
per device, which backend to load **before any backend code runs**.

It deliberately separates **two questions** that the early experiment
conflated:

1. **The sokol-Vulkan integration contract** — a fixed engineering fact: what
   ge must hand sokol so its Vulkan backend initialises (§1). This does not
   vary by device.
2. **The Android portability policy** — a judgement call: *which* devices
   should use Vulkan in production, and how a device that can't is detected
   and routed to GLES *without crashing* (§2–§4).

## 1. The sokol-Vulkan integration contract

sokol_gfx's Vulkan backend is **not a one-flag swap** from GLES. Unlike the
GLES path (where sokol drives the default EGL framebuffer), the Vulkan backend
makes **ge own** the Vulkan instance / physical device / logical device /
queue / swapchain, and feed sokol the handles:

- `sg_setup()` needs `sg_desc.environment.vulkan` populated with the
  **instance, physical device, logical device, graphics queue, and queue
  family index**.
- **Every swapchain pass** needs `sg_pass.swapchain.vulkan` populated with the
  current swapchain **image + image view**, the **render/present semaphores**,
  format, sample count, and extent — re-supplied each frame after
  `vkAcquireNextImageKHR`.
- ge owns the **acquire → submit → present** loop (`vkAcquireNextImageKHR`,
  queue submit with the semaphores sokol used, `vkQueuePresentKHR`).
- **Shaders** must include a `spirv_vk` variant (sokol-shdc).
- **Screenshot / readback** (🎯T92.6) needs a separate Vulkan implementation
  (image → linear buffer copy), distinct from the GLES `glReadPixels` path.

### Required device capabilities (what current vendored sokol uses)

Current vendored sokol actively loads and uses **`VK_EXT_descriptor_buffer`**
entry points (`vkGetDescriptorSetLayoutSizeEXT`,
`vkGetDescriptorSetLayoutBindingOffsetEXT`, `vkGetDescriptorEXT`,
`vkCmdBindDescriptorBuffersEXT`, `vkCmdSetDescriptorBufferOffsetsEXT`), plus a
1.3 feature set. The hard requirements:

| Requirement | Why |
|---|---|
| Physical-device Vulkan **≥ 1.3** | sokol uses 1.3-era entry points |
| **`VK_EXT_descriptor_buffer`** ext **+ `descriptorBuffer` feature** | sokol's binding model |
| `bufferDeviceAddress` (1.2) | required by descriptor-buffer usage |
| `dynamicRendering` (1.3) | sokol's pass model |
| `synchronization2` (1.3) | sokol's submit/sync |
| graphics + present queue | rendering + presentation |
| compatible surface format / composite alpha / swapchain usage | swapchain creation |
| `VK_EXT_debug_utils` (**debug builds only**) | sokol labels objects via `vkSetDebugUtilsObjectNameEXT`; absence crashes *debug* builds only |

### API level vs dynamic loading

Directly linking the Vulkan functions current sokol uses forced **API 35** in
the experiment, because the Android `libvulkan.so` stub only exports **Vulkan
1.0** symbols at low API levels (1.1 entry points such as
`vkGetPhysicalDeviceFeatures2` and `vkEnumerateInstanceVersion` arrive at
higher platform levels). **Resolution: dynamic function loading.** Link only
`vkGetInstanceProcAddr` (1.0) and resolve everything past `vkCreateInstance`
through it. This keeps **minSdk unchanged** and lets one binary run on every
device, rejecting (not crashing) where an entry point is missing. The probe
(`tools/vkprobe/ge_vkprobe.c`) already does exactly this.

## 2. The backend-selection policy

**Decide once, at startup, before the renderer backend `.so` is mapped. Never
switch at runtime.** A device that fails *any* probe check selects GLES; app
code never observes a partially-initialised sokol instance and never reaches
`sg_make_*` with `_sg.valid == false` (the crash mode the Samsungs hit).

**Prefer Vulkan iff the device passes the full probe** (§1 capabilities + the
in-app surface/swapchain viability check); **otherwise GLES3**. GLES remains
the unconditional fallback — ge does **not** raise minSdk or require the Vulkan
feature set of any device. Vulkan is an opportunistic performance path, not a
floor.

The decision is logged at startup with enough detail for field diagnosis:

```
ge.renderer: ACCEPT_VULKAN  device="Mali-G710" api=1.4.305 descriptor_buffer=1 ...
ge.renderer: FALLBACK_GLES  device="Mali-G57 MC2" api=1.3.303 reasons=[no VK_EXT_descriptor_buffer]
```

## 3. Package design — sokol as a per-backend plugin (dispatch layer)

sokol_gfx selects its backend at **compile time** (`SOKOL_VULKAN` *xor*
`SOKOL_GLES3`); both define the same 150 `sg_*` symbols, so they **cannot
coexist in one `.so`**, and runtime switching inside one sokol instance is
impossible. Rather than duplicate the *whole* engine + app into two variants
(which would also force a second NDK-pinned `libge.a` prebuilt, doubling the
🎯T103/🎯T78 surface forever), the split is confined to **where the backend
difference actually lives — sokol, one header** — via a thin dispatch layer.
This is safe because the **public sokol API has zero backend `#ifdef`s**: every
struct/enum (incl. `sg_vulkan_environment`/`sg_vulkan_swapchain`, which are
plain `const void*` handles) is ABI-identical across backends, so one shared
`sokol_gfx.h` describes both.

**Pieces:**

- **`libgevkprobe.so`** — the capability probe (§1), links *only* `libvulkan`,
  no sokol/SDL. Zero `sg_*`, can never conflict.
- **`libge.a`** (single, backend-agnostic, **one prebuilt**) — includes
  `sokol_gfx.h` for *types only* (no `SOKOL_IMPL`) and links the generated
  **forwarder shim** (`tools/sokol-dispatch/gen.py` → 150 real `sg_*` symbols
  that dispatch through an installed table `g_ge_sg_api`). Every existing
  `sg_*` call site in ge is unchanged; one indirect call per `sg_*` (a GL/Vulkan
  loader already costs this).
- **`libgesokol-gles.so`** / **`libgesokol-vk.so`** — each compiles `SOKOL_IMPL`
  + its backend `#define` + that backend's device/swapchain/acquire-present
  glue, `-fvisibility=hidden` so all `sg_*` and the impl are internal. Each
  exports **exactly one** `visibility("default")` symbol, named per backend:
  `ge_sokol_bind_gles` / `ge_sokol_bind_vulkan`. Distinct names → co-mapping is
  collision-free (only one is loaded anyway). The bind function installs the
  `sg_*` table (`ge_sokol_fill.inc`) **and** returns a small `ge_render_backend`
  vtable (`init(ANativeWindow*)`, `begin_frame(→ swapchain)`, `end_frame()`,
  `screenshot()`) — because the acquire/submit/present loop and readback are
  backend-specific (sokol explicitly does *not* call `vkAcquireNextImageKHR` /
  `vkQueuePresentKHR`; ge owns them).
- All `NEEDED libSDL3.so` (shared, backend-agnostic — SDL creates a GL *or*
  Vulkan window per the runtime choice).

**Selection.** The probe runs before renderer startup; `GeActivity` (or the app
bootstrap) `dlopen`s the chosen `libgesokol-<backend>.so`, calls its single
`ge_sokol_bind_*`, which installs the table — so **exactly one** sokol impl is
ever mapped, with no `sg_*`/`_sg` collision. GLES devices get an
unchanged render path; `libge` and the app are built **once**.

> On Apple (one backend) the dispatch indirection is optional — `libge` can keep
> `SOKOL_IMPL` inline. The plugin shape is required only where the duality is:
> Android.

## 4. Device-test matrix

Each device records: Vulkan feature/extension support, the probe verdict, the
selected backend, launch result, screenshot/readback status, and frame pacing.
**Probe-verdict rows marked ✅ device-verified were run live with
`tools/vkprobe/build-cli.sh --run` on NDK r27.**

| Device | GPU | Vulkan | `descriptor_buffer` | Probe verdict | Source |
|---|---|---|---|---|---|
| **Pixel Tablet** (tangorpro) | Mali-G710 | 1.4.305 | ✓ + all features | **ACCEPT_VULKAN** | ✅ device-verified 2026-06-07 |
| **Tab A9 / SM-X110** (gta9wifi) | Mali-G57 MC2 | 1.3.303 | ✗ | **FALLBACK_GLES** `[no VK_EXT_descriptor_buffer]` | ✅ device-verified 2026-06-07 |
| ASUS AI2501C | — | 1.3+ | ✓ | ACCEPT_VULKAN (needed `VK_EXT_debug_utils` for labels) | experiment (PR #140), not re-run |
| Samsung S21 / SM-G9980 | — | **1.1** | n/a | FALLBACK_GLES `[device Vulkan < 1.3]` | experiment (PR #140), not re-run |
| Android emulator | SwiftShader/host | varies | varies | (to record) | pending |

**Launch results (app-level smoke) — full dual-backend matrix, all device-verified 2026-06-07:**

| Device | GPU | Probe → selected | Launch / render | Screenshot |
|---|---|---|---|---|
| S24 / SM-S921B | Xclipse 940 | ACCEPT → **Vulkan** | ✅ renders (button, title, playfield, buggy) | screencap ✅ + app-channel readback ✅ |
| ASUS AI2501C | **Adreno 830** | ACCEPT → **Vulkan** | ✅ renders | screencap ✅ |
| Pixel Tablet | Tensor G2 | ACCEPT → **Vulkan** | ✅ renders | screencap ✅ |
| S21 / SM-G9980 | Mali (Vk 1.1) | FALLBACK → **GLES3** | ✅ renders | screencap ✅ |
| Tab A9 / SM-X110 | Mali (no desc_buffer) | FALLBACK → **GLES3** | ✅ renders | screencap ✅ |

All three ACCEPT devices render correctly on Vulkan; both FALLBACK devices
correctly select GLES and render. Three integration bugs were found and fixed
to get the Vulkan path rendering:

1. **Missing `spirv_vk` shader variant.** ge's internal shaders (`ge_sprite.h`,
   `ge_debug.h`) and the consumer's shaders were generated without `spirv_vk`,
   so `*_shader_desc(sg_query_backend())` returned NULL on a Vulkan-selected
   device → `sg_make_shader(NULL)` aborted. Fixed by adding `spirv_vk` to
   `GE_SHDC_LANGS` in `tools/prebuild.sh`, `Module.mk`, and the consumer
   `CMakeLists.txt.in`.
2. **sokol `int`-overflow on "unlimited" descriptor limits.** The Xclipse / AMD
   (and Adreno) drivers report `maxPerStageDescriptor*` as `0xFFFFFFFF`; sokol's
   `_sg_min((int)limit, 32)` cast that to `-1`, so every shader failed
   `SHADERDESC_TOO_MANY_*` validation (even 0-binding stages, since `0 > -1`).
   Patched in vendored `sokol_gfx.h` (`_sg_vk_init_caps`) to take the min in
   `uint32` space before casting. **TODO: upstream to floooh/sokol.**
3. **`VK_EXT_debug_utils` not enabled.** sokol's debug build labels every
   Vulkan object via `vkSetDebugUtilsObjectNameEXT` and asserts the pointer is
   non-NULL. Strict loaders (Adreno) return NULL unless the extension is
   enabled; `SokolContext_android`'s `createInstance` now enables it when
   present.

**The Adreno 830 finding (headline).** `docs/papers/adreno-830-bgfx-vulkan-crash.md`
records that ge's *bgfx* Vulkan backend crashed the Adreno 830 driver with a
null-deref on **draw submission** (`vkCmdCopyBuffer2` / `vkCmdPipelineBarrier`
inside `RendererContextVK::submit`). That doc's open question was whether a
different renderer's command pattern would avoid the bug. **Answer: yes.**
sokol's pattern (dynamic-rendering + `synchronization2` +
`vkCmdPipelineBarrier2` + descriptor-buffers) renders correctly on the same
device + driver — the bgfx-era crash does not reproduce. The crash doc is
therefore historical only.

(Establishing the GLES baseline also required a FreeType link fix — see 🎯T30
commit — since `libge.a`'s `text.o` referenced `FT_*` that SDL3_ttf hid
privately.)

## 5. Go / no-go for shipping Vulkan on Android — **GO** (all criteria met)

GLES3 stays the always-available fallback; Vulkan is the selected path on
ACCEPT devices. All ship criteria are now device-proven (2026-06-07):

1. ✅ The probe **accepts** on capable devices and **rejects → GLES** on
   `descriptor_buffer`-less and `<1.3` devices, **before `sg_setup`**, with no
   `_sg.valid` crash. Verified on all 5 matrix devices (3 ACCEPT, 2 FALLBACK).
2. ✅ The Vulkan path **launches and renders** with the same visual output as
   GLES — on S24, ASUS (Adreno 830), and Pixel Tablet.
3. ✅ **Screenshot / readback parity** — the Vulkan readback
   (`VkM::captureSwapchain`, `vkCmdCopyImageToBuffer`) produces correct RGBA8
   output; verified via app-channel `app_screenshot` on a Vulkan device, so
   visual-regression baselines hold across backends.
4. ✅ The GLES-fallback path on a rejected device renders correctly (S21, Tab A9)
   and is selected by the same runtime probe rather than a separate binary.

**Selection model (as shipped).** Both backend `.so` (`libgesokol-gles.so`,
`libgesokol-vk.so`) are linked into `libmain.so`; `SokolContext_android` runs
`ge_vk_probe` at construction and binds exactly one via the dispatch shim, with
GLES fallback if Vulkan bring-up fails for any reason. App code is unchanged and
backend-agnostic (it calls `sg_*`, which dispatch to the bound backend).

## References

- Probe: `tools/vkprobe/ge_vkprobe.c` (`ge_vk_probe()`), `build-cli.sh`.
- Experiment (reference-only force-Vulkan spike): squz/ge PR #140.
- sokol_gfx: `vendor/github.com/floooh/sokol/sokol_gfx.h` (`SOKOL_VULKAN`).
- NDK pin: r27 (🎯T103); 16 KB page alignment (🎯T75).

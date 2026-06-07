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

**Launch / screenshot / pacing rows** (the app-level smoke half) are filled by
the dual-variant smoke test: launch `libmain-vk.so` on the Pixel Tablet
(Vulkan-accepted path) and `libmain-gles.so` on the Tab A9 (GLES-fallback
path), each verified via `spyder`/app-channel screenshot + readback, and
recorded here.

## 5. Go / no-go for shipping Vulkan on Android

GLES3 stays the default-shipped, always-available backend. Vulkan ships as the
selected path **only** when all hold:

1. The probe correctly **accepts** on a known-good device and **rejects → GLES**
   on `descriptor_buffer`-less and `<1.3` devices, **before `sg_setup`**, with
   no `_sg.valid` crash. *(Probe half: ✅ verified on Pixel + Tab A9.)*
2. The Vulkan variant **launches and renders** on an accepted device with the
   same visual output as GLES.
3. **Screenshot / readback parity** — the Vulkan readback path produces output
   matching the GLES path (so app-channel screenshots and visual-regression
   baselines hold across backends).
4. The GLES-fallback variant on a rejected device is **byte-unchanged** from
   today's shipping Android binary.

Until 2–3 are device-proven on the Vulkan path, the production default remains
**GLES-only**, with the probe + dual-variant plumbing landed but the Vulkan
variant gated behind an explicit opt-in.

## References

- Probe: `tools/vkprobe/ge_vkprobe.c` (`ge_vk_probe()`), `build-cli.sh`.
- Experiment (reference-only force-Vulkan spike): squz/ge PR #140.
- sokol_gfx: `vendor/github.com/floooh/sokol/sokol_gfx.h` (`SOKOL_VULKAN`).
- NDK pin: r27 (🎯T103); 16 KB page alignment (🎯T75).

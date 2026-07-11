# ge

Rendering and asset engine for Dawn (WebGPU) + SDL3 applications. Provides RAII resource management, manifest-driven asset loading, and animation utilities.

## Dependencies

- [Dawn](https://dawn.googlesource.com/dawn) — WebGPU implementation
- [SDL3](https://libsdl.org/) + SDL3_image — Windowing, input, image loading
- [spdlog](https://github.com/gabime/spdlog) — Logging (header-only)
- [linalg.h](https://github.com/sgorsten/linalg) — Linear algebra (header-only)
- [doctest](https://github.com/doctest/doctest) — Testing (header-only)
- [Triangle](https://www.cs.cmu.edu/~quake/triangle.html) — Constrained Delaunay triangulation *(opt-in only; not linked into `libge.a`. Non-commercial license — see [`NOTICES.md`](NOTICES.md#triangle-j-r-shewchuk) before shipping in a paid product.)*

Dependencies live in `vendor/` as git submodules or vendored sources.

## Integration

ge is consumed as a git submodule. The parent project includes `Module.mk` from its own Makefile:

```makefile
include ge/Module.mk
```

This exports variables (`ge/LIB`, `ge/OBJ`, `ge/FRAMEWORK_LIBS`, etc.) and pattern rules for compiling engine sources, test sources, and shaders. There is no standalone build.

### iOS / iPad orientation lock

iPadOS 26+ ignores the usual single-knob orientation locks (`UIRequiresFullScreen`, `SDL_HINT_ORIENTATIONS`, `requestGeometryUpdate`, even narrowing `UISupportedInterfaceOrientations` alone). To lock orientation on iPad you need **two things together**:

1. Narrow `UISupportedInterfaceOrientations` in your `Info.plist` to the orientation(s) you want allowed (e.g. just `LandscapeLeft` and `LandscapeRight` for a landscape-only game).
2. Set `SessionHostConfig.orientation = wire::kOrientationLandscape` (or any other non-zero `wire::kOrientation*`) when calling `ge::run`. The engine handles the swizzle (Apple TN3192 — `prefersInterfaceOrientationLocked`) for you, and the orientation source is linked into `libge` from v0.3.0+.

Plist alone won't survive iPad multitasking; the swizzle alone locks "whatever orientation the user happened to be holding the device in at launch." Ship both. Setting different `kOrientation*` constants is currently a boolean ("lock yes/no"); the plist decides *which* orientation. See `agents-guide.md` and `CLAUDE.md` (search for "iOS orientation lock") for the full background and history.

## Structure

```
include/        Public headers (one per class)
src/            Implementation + unit tests (*_test.cpp)
shaders/        Test shaders (app shaders live in the parent project)
vendor/         Third-party dependencies
Module.mk       Build rules and exported variables
```

## API Overview

### Platform

| Header | Description |
|--------|-------------|
| `GpuContext.h` | WebGPU device/queue/surface lifecycle |
| `SdlContext.h` | RAII SDL3 window and event loop setup |

### Resources

| Header | Description |
|--------|-------------|
| `WgpuResource.h` | Move-only RAII wrapper for WebGPU handles (`BufferHandle`, `TextureHandle`, `SamplerHandle`, etc.) |
| `CaptureTarget.h` | Offscreen framebuffer with RGBA8 color texture for readback |
| `ShaderUtil.h` | `ge::loadProgram()` — load compiled vertex + fragment shaders |

### Assets

| Header | Description |
|--------|-------------|
| `Mesh.h` | Vertex/index buffer pair with binary stream loader |
| `Texture.h` | GPU texture loaded from image files via SDL3_image |
| `svg.h` | SVG rasterization, live-document rendering, hit testing, font registration, and post-layout document/element bounds measurement |
| `Model.h` | Mesh + texture binding |
| `ModelFormat.h` | `ge::MeshVertex` — vertex layout (pos3f + uv2f) |
| `ManifestSchema.h` | JSON-serializable manifest types, templated on app metadata |
| `ManifestLoader.h` | `ge::loadManifest<Meta>()` — loads manifest + mesh pack + textures |

### Animation

| Header | Description |
|--------|-------------|
| `DampedRotation.h` | Quaternion rotation with angular velocity and exponential decay |
| `DampedValue.h` | 1D value with velocity and exponential decay |
| `DeltaTimer.h` | Frame delta-time tracking |

### Testing

| Header | Description |
|--------|-------------|
| `ImageDiff.h` | CPU and GPU pixel comparison (RMS difference) |

## Dev control plane

Use **[spyder](https://github.com/marcelocantos/spyder)** for device inventory, launch, tweaks, logs, screenshots, and the optional H.264 stream relay. The historical `ged` daemon has been removed from this repo.

## Launching the Player with a Specific Stream Relay Address

For automated and scripted launches (CI, matrix testing, agent workflows), point the
native player at spyder's stream relay (same host/port as `spyder serve`, default
`127.0.0.1:3030` for local; use the machine LAN IP from a phone).

**Desktop**
```bash
bin/player --host 127.0.0.1 --port 3030 --name <server-name>
```

**iOS Simulator** — pass `-ged_addr host:port` as a launch argument (flag name is historical):
```bash
xcrun simctl launch <udid> com.squz.player -ged_addr localhost:3030
```

**iOS Device** — use `devicectl` with `--console-pty` and pass args after `--`:
```bash
xcrun devicectl device process launch --console-pty --device <udid> com.squz.player -- -ged_addr 192.168.1.100:3030
```

**Android Emulator** — pass `--es ged_addr host:port` to `am start`:
```bash
adb shell am start -n com.squz.player/.GeActivity --es ged_addr 10.0.2.2:3030
```

**Android Device** — same syntax with the Mac's LAN IP:
```bash
adb shell am start -n com.squz.player/.GeActivity --es ged_addr 192.168.1.100:3030
```

If the parameter is absent the player falls back to any saved address, then QR scan.

## License

See individual files and `vendor/` subdirectories for license terms.

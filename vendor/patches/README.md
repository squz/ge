# Vendored-library patches

Patches we maintain against upstream vendored libraries whose
submodules point at **upstream** remotes. They live here (not in
the upstream submodules) so submodule updates don't overwrite them
silently. Re-apply after pulling a new vendored revision.

## When to use this directory vs. a fork

- **Upstream-pinned submodule** → carry divergence as a `.patch`
  file here and register it in `scripts/apply-vendor-patches.sh`.
- **Fork-pinned submodule** → commit the divergence to the fork
  branch directly; the submodule SHA then captures it with no apply
  step needed.

There are no fork-pinned or patched submodules at present. (ge used
the fork approach for `squz/bgfx` until the renderer migrated to
sokol_gfx in 🎯T38 and the bgfx/bx/bimg submodules were removed in
🎯T34.)

## Applying

From the repo root:

```sh
scripts/apply-vendor-patches.sh
```

The script is idempotent — it uses `git apply --check` before
applying and skips already-applied patches. With no patches
currently registered, it's a no-op.

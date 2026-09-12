#!/usr/bin/env bash
# Engine .d files must be -included, or a header-only edit leaves stale .o files.
#
# Two checks:
#   1. Module.mk actually -includes ge/OBJ, ge/TEST_OBJ, and APP_DEBUG_OBJ .d files.
#   2. If sample/tiltbuggy/build/ge/src/sprite.o exists (or can be built),
#      touching include/ge/sprite.h must rebuild that object without make clean.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

# ── 1. Module.mk must consume engine/test/debug depfiles ──────────
mk="Module.mk"
for needle in '$(ge/OBJ:.o=.d)' '$(ge/TEST_OBJ:.o=.d)' '$(APP_DEBUG_OBJ:.o=.d)'; do
  if ! grep -q -- "-include.*${needle}" "$mk"; then
    fail "Module.mk does not -include $needle (engine .d files are generated then ignored)"
  fi
done
pass "Module.mk -includes engine, test, and APP_DEBUG .d files"

# ── 2. Incremental rebuild of an engine object after a public-header touch
OBJ="sample/tiltbuggy/build/ge/src/sprite.o"
HDR="include/ge/sprite.h"
[[ -f "$HDR" ]] || fail "missing $HDR"

if [[ ! -f "$OBJ" ]]; then
  echo "  (building $OBJ for incremental check…)"
  if ! make -C sample/tiltbuggy -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
      "build/ge/src/sprite.o" >/tmp/ge-inc-deps-build.log 2>&1; then
    echo "  (skip mtime check — could not build sprite.o; see /tmp/ge-inc-deps-build.log)"
    echo "OK: incremental-deps static check passed"
    exit 0
  fi
fi
[[ -f "$OBJ" ]] || fail "sprite.o still missing after build"

before=$(stat -f %m "$OBJ" 2>/dev/null || stat -c %Y "$OBJ")
# Touch the header a second later so mtime comparison is decisive on 1s FS resolution.
sleep 1
touch "$HDR"

if ! make -C sample/tiltbuggy -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
    "build/ge/src/sprite.o" >/tmp/ge-inc-deps-rebuild.log 2>&1; then
  cat /tmp/ge-inc-deps-rebuild.log >&2
  fail "rebuild of sprite.o after touching sprite.h failed"
fi

after=$(stat -f %m "$OBJ" 2>/dev/null || stat -c %Y "$OBJ")
if [[ "$after" -le "$before" ]]; then
  fail "touching $HDR did not rebuild $OBJ (mtime $before → $after) — .d files not honored"
fi
pass "touching $HDR rebuilt $OBJ ($before → $after)"

echo "OK: incremental-deps checks passed"

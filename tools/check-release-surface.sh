#!/usr/bin/env bash
# 🎯T145 acceptance #2 — release surface oracle.
#
# Class-1 checks that a shipping (NDEBUG, non-server) consumer does not carry
# the dev control-plane or stream-encode surfaces:
#
#   1. App-channel: under -DNDEBUG, appchannel.cpp collapses to empty no-ops —
#      object is tiny and has no live channel/dial implementation.
#   2. Stream / encode: GE_SERVER_BUILD is a separate build variant. A default
#      (direct) link of the sample must not retain ServerSession / VideoEncoder
#      / runServer symbols (static dead-strip of the brokered TUs that only
#      runServer references).
#
# Exit 0 on pass; non-zero with a clear FAIL line otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${TMPDIR:-/tmp}/ge-release-surface-$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

# ── 1. appchannel under NDEBUG ─────────────────────────────────────
INCLUDES=(
  -std=c++20 -c
  -Iinclude
  -Ivendor/include
  -Ivendor/github.com/gabime/spdlog/include
  -Ivendor/github.com/chriskohlhoff/asio/include
  -Ivendor/github.com/libsdl-org/SDL/include
  -Ivendor/sdl3/include
  -DASIO_STANDALONE
)

if ! clang++ "${INCLUDES[@]}" -o "$OUT/appchannel-debug.o" src/appchannel.cpp 2>"$OUT/dbg.err"; then
  cat "$OUT/dbg.err" >&2
  fail "could not compile appchannel.cpp (debug)"
fi
if ! clang++ "${INCLUDES[@]}" -DNDEBUG -o "$OUT/appchannel-release.o" src/appchannel.cpp 2>"$OUT/rel.err"; then
  cat "$OUT/rel.err" >&2
  fail "could not compile appchannel.cpp (-DNDEBUG)"
fi

DBG_SZ=$(wc -c <"$OUT/appchannel-debug.o" | tr -d ' ')
REL_SZ=$(wc -c <"$OUT/appchannel-release.o" | tr -d ' ')
# Local (non-export) Channel class is the live implementation; stubs have none.
DBG_CH=$(nm "$OUT/appchannel-debug.o" 2>/dev/null | grep -c 'appchannel.*Channel' || true)
REL_CH=$(nm "$OUT/appchannel-release.o" 2>/dev/null | grep -c 'appchannel.*Channel' || true)

# Debug TU is megabytes (full msgpack channel); NDEBUG stubs are a few KB.
if [[ "$DBG_SZ" -lt 100000 ]]; then
  fail "debug appchannel.o unexpectedly small ($DBG_SZ bytes) — compile may have failed open"
fi
if [[ "$REL_SZ" -gt 50000 ]]; then
  fail "NDEBUG appchannel.o still large ($REL_SZ bytes; debug=$DBG_SZ) — feature not stripped"
fi
if [[ "$DBG_CH" -lt 1 ]]; then
  fail "debug appchannel.o missing Channel symbols (oracle needle drifted?)"
fi
if [[ "$REL_CH" -gt 0 ]]; then
  fail "NDEBUG appchannel.o still contains Channel symbols ($REL_CH) — live channel remains"
fi
pass "appchannel is empty under NDEBUG (debug=${DBG_SZ}B/${DBG_CH} Channel → release=${REL_SZ}B/${REL_CH} Channel)"

# ── 2. default (direct) sample link has no stream/encode symbols ───
SAMPLE_BIN="sample/tiltbuggy/bin/tiltbuggy"
if [[ ! -x "$SAMPLE_BIN" ]]; then
  echo "  (building sample/tiltbuggy for symbol scan…)"
  make -C sample/tiltbuggy -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
fi
[[ -x "$SAMPLE_BIN" ]] || fail "no sample binary at $SAMPLE_BIN"

FORBIDDEN_RE='VideoEncoder|ServerSession|runServer'
if nm -gU "$SAMPLE_BIN" 2>/dev/null | grep -E ' [TSDBtsdb] ' | grep -Eq "$FORBIDDEN_RE"; then
  echo "--- defined stream/encode symbols in $SAMPLE_BIN ---" >&2
  nm -gU "$SAMPLE_BIN" 2>/dev/null | grep -E ' [TSDBtsdb] ' | grep -E "$FORBIDDEN_RE" >&2 || true
  fail "default (non-GE_SERVER) binary retains stream/encode symbols"
fi
pass "default sample binary has no defined ServerSession/VideoEncoder/runServer symbols"

if ! grep -q 'GE_SERVER_BUILD' src/SessionHost.mm; then
  fail "SessionHost.mm no longer gates runServer on GE_SERVER_BUILD"
fi
pass "runServer remains compile-gated on GE_SERVER_BUILD"

echo "OK: release surface checks passed (T145 acceptance #2)"

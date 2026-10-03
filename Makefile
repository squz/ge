# ge — top-level delegator.
#
# ge itself has no standalone build: it's a library consumed via
# `-include $(ge)/Module.mk` from an app's Makefile. This file exists
# purely for developer ergonomics — running `make check` (or any other
# app-level target) from the ge root proxies to the canonical in-tree
# sample at sample/tiltbuggy/.
#
# The sample is responsible for exercising as much of ge's surface as
# practicable; see sample/tiltbuggy/Makefile.

SAMPLE ?= sample/tiltbuggy

# Default to parallel build for any meta-target that has multiple
# independent dependencies (prebuild, for one). Module.mk sets the same
# flag for consumer-side builds; setting it here covers ge-developer
# targets in this file.
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Specific targets the delegator should proxy. `check` is the big one:
# runs the 24-cell e2e matrix via the sample's Module.mk integration.
.PHONY: all check matrix-test check-list unit-test init clean run bullseye ruby-test \
        ledger-check ledger-land \
        python-test dispatch-null-safety-test release-surface-test ndk-discovery-test iap-android-test \
        prebuild prebuild-ios-arm64 prebuild-ios-arm64-simulator prebuild-android-arm64 \
        prebuild-libge prebuild-libge-ios-arm64 prebuild-libge-ios-arm64-simulator \
        prebuild-libge-android-arm64 \
        ge/lift-headers depgraph clean-depgraph

all check matrix-test check-list unit-test clean run:
	$(MAKE) -C $(SAMPLE) $@

# ge/init and the sample's init both contribute to dev-machine setup.
init:
	$(MAKE) -C $(SAMPLE) ge/init
	$(MAKE) -C $(SAMPLE) init 2>/dev/null || true

# `ge/*` engine targets can be invoked directly from here
# via the sample's Module.mk. Forward anything starting with `ge/`.
ge/%:
	$(MAKE) -C $(SAMPLE) $@

# Forward per-cell invocations for parity with the sample:
#   make cell.ios-sim-tablet-dist
cell.%:
	$(MAKE) -C $(SAMPLE) $@

# Standing invariants for /cv. Exit 0 means all green; non-zero means a
# violation. Stdout is relayed verbatim to the agent. Ignores untracked
# files so the user's WIP notes don't trip the check.
#
# 🎯T198: `unit-test` is a prerequisite so a red doctest suite on master
# cannot sit unnoticed. It went unnoticed once already — the T156.6 parity
# oracle was failing intermittently while agents cited green gates that ran
# downstream of it. CI is not the right home for this one: the unit suite
# needs a macOS runner (10x minute multiplier) and a from-source SDL cook,
# which release.yml already defers for cost, so the gate rides the local
# standing path that /cv actually runs. Incremental after the first build.
bullseye: ruby-test python-test dispatch-null-safety-test release-surface-test ndk-discovery-test iap-android-test \
          unit-test
	@git diff --quiet && git diff --cached --quiet \
	  && echo "✓ no uncommitted changes to tracked files" \
	  || { echo "✗ uncommitted changes to tracked files"; \
	       git status --short --untracked-files=no; exit 1; }
	@untracked=$$(git ls-files --others --exclude-standard); \
	  if [ -n "$$untracked" ]; then \
	    echo "ℹ untracked files (not a violation):"; \
	    echo "$$untracked" | sed 's/^/    /'; \
	  fi

# Ruby-side regression tests for ge tooling (build_project.rb etc.).
# Fast — runs in well under a second; safe to wire into `bullseye` so
# every /cv invocation guards against regressions.
ruby-test:
	@bundle exec ruby tools/ios-build/test_build_project.rb

# Python-side regression tests — currently the prebuilt-staleness
# verifier (🎯T78). Runs in ~1s; wired into `bullseye` for the same
# reason as ruby-test.
python-test:
	@python3 tools/test_verify_manifest.py
	@python3 tools/test_verify_prebuilds.py

# 🎯T140/T141: the sokol dispatch shim's teardown forwarders (sg_isvalid /
# sg_shutdown) must be NULL-safe when no backend is bound (the Android VK->GLES
# fallback-teardown crash the Fable-5 audit traced). Standalone: the shim
# defines real sg_* symbols that collide with sokol's SOKOL_IMPL in the doctest
# binary, so it can't ride `unit-test`.
dispatch-null-safety-test:
	@mkdir -p build
	@clang -DSOKOL_GLES3 -Itools/sokol-dispatch/generated \
	    -Ivendor/github.com/floooh/sokol -o build/dispatch-nullsafe-test \
	    tools/sokol-dispatch/generated/ge_sokol_dispatch.c \
	    tools/sokol-dispatch/nullsafe_test.c
	@build/dispatch-nullsafe-test

# 🎯T145 acceptance #2: NDEBUG strips app-channel; default (non-GE_SERVER)
# sample link dead-strips stream/encode symbols.
release-surface-test:
	@tools/check-release-surface.sh

# 🎯T200: the intent ledger must not live only in a working copy.
#
# `bullseye` writes bullseye.yaml into whatever checkout it is pointed at, so
# a filing is silent and invisible the instant it is written. 🎯T188 wrote the
# rule into AGENTS.md and it held for under an hour — three actors re-forked
# the ledger the same evening, one of them the PO who wrote the rule, and two
# of them filed the same defect minutes apart because neither filing was
# visible to the other. Documentation was already tried; this is the part that
# cannot be forgotten.
#
# Deliberately its own target rather than folded into the dirty-tree check
# above: that check fails on *any* uncommitted file, so in a checkout carrying
# unrelated WIP it is permanently red and says nothing about the ledger. A
# signal that is always on is one nobody reads — which is exactly how tonight's
# fork stayed invisible.
#
# The prerequisite is declared on its own line (Make accumulates prerequisites
# across declarations) so wiring this in does not edit the `bullseye` recipe
# body, which has a long-lived WIP branch against it.
bullseye: ledger-check

ledger-check:
	@tools/check-ledger-parity.sh

# The remedy the check points at. A loud refusal with no cheap way out is one
# people learn to bypass.
ledger-land:
	@tools/land-ledger.sh

# 🎯T191/🎯T186: Android toolchain paths come from globs, not from parsing
# `ls`, so a colourised host cannot break the cook. Sub-second; wired into
# `bullseye` so a reintroduced `$(ls …)` capture is caught at /cv time.
ndk-discovery-test:
	@tools/test-android-ndk.sh

# Production Java bridge + AndroidStore JNI lifecycle, with mocked Play Billing.
iap-android-test:
	@python3 tools/iap-android-test/run.py

# ── Prebuilt static libs (🎯T73) ───────────────────────────────────
#
# Each per-platform rule shells out to tools/prebuild.sh <platform>. The
# meta-target `prebuild` runs all three as plain Make dependencies, so
# the MAKEFLAGS -j set above fans them out in parallel — three concurrent
# clang processes on a multi-core dev box.
#
# Per-platform prereqs:
#   ios-arm64 / ios-arm64-simulator  → Xcode + iphoneos/iphonesimulator SDKs
#   android-arm64                    → Android NDK (auto-detected from
#                                       ANDROID_NDK_HOME or ~/Library/Android/sdk/ndk/)

prebuild-ios-arm64:
	tools/prebuild.sh ios-arm64

prebuild-ios-arm64-simulator:
	tools/prebuild.sh ios-arm64-simulator

prebuild-android-arm64:
	tools/prebuild.sh android-arm64

# Debug prebuilts: prebuilt/<platform>-debug/ with -g -O2 on *all* archives
# (libge + vendor) so optimised-only bugs still reproduce under the debugger.
# GE_PREBUILD_OPT=0 for true -O0. Android Studio Debug links
# android-arm64-debug via cmake/android-arm64.cmake.
prebuild-ios-arm64-debug:
	tools/prebuild.sh --debug ios-arm64

prebuild-ios-arm64-simulator-debug:
	tools/prebuild.sh --debug ios-arm64-simulator

prebuild-android-arm64-debug:
	tools/prebuild.sh --debug android-arm64

prebuild-debug: prebuild-ios-arm64-debug prebuild-ios-arm64-simulator-debug prebuild-android-arm64-debug

prebuild-web-wasm:
	tools/prebuild.sh web-wasm

prebuild: prebuild-ios-arm64 prebuild-ios-arm64-simulator prebuild-android-arm64 prebuild-web-wasm

# Historical names: always full cook (partial libge-only cooks removed).
prebuild-libge-ios-arm64: prebuild-ios-arm64
prebuild-libge-ios-arm64-simulator: prebuild-ios-arm64-simulator
prebuild-libge-android-arm64: prebuild-android-arm64
prebuild-libge: prebuild
prebuild-libge-android-arm64-debug: prebuild-android-arm64-debug

# ── ge-maintenance rules (not for consuming apps) ──────────────────
#
# These live here because they touch ge's own source tree — consumers
# would gain nothing by running them. The `ge/%` pattern delegator
# above is hit only for targets not matched explicitly here, so these
# explicit definitions take precedence.

# Refresh headers/<vendor>/include/ trees from submodule sources. Pairs
# with the prebuild scripts: when vendor submodule SHAs change, lift +
# rebuild both run.
ge/lift-headers:
	tools/lift-headers.sh

# Visualise ge's internal module dependency graph (engine source files →
# headers). Useful when refactoring ge; consumers want their own graph,
# not this one.
DEPGRAPH_DEPS := $(wildcard src/*.cpp src/**/*.cpp src/*.mm src/**/*.mm include/ge/*.h) tools/depgraph.py

depgraph: deps.svg

deps.svg: $(DEPGRAPH_DEPS)
	python3 tools/depgraph.py --format svg --output deps

deps.dot: $(DEPGRAPH_DEPS)
	python3 tools/depgraph.py --format dot --output deps

clean-depgraph:
	rm -f deps.dot deps.svg deps.png

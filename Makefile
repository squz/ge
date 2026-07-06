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
.PHONY: all check matrix-test check-list unit-test init clean ged run bullseye ruby-test \
        python-test dispatch-null-safety-test \
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

# ged and other `ge/*` engine targets can be invoked directly from here
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
bullseye: ruby-test python-test
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

prebuild: prebuild-ios-arm64 prebuild-ios-arm64-simulator prebuild-android-arm64

# Source-only engine refresh: rebuild just libge.a for each platform and
# preserve existing vendor archives + their manifest input hashes. Use this
# when ge sources/headers change but vendor submodules and vendor sources do not.
prebuild-libge-ios-arm64:
	tools/prebuild.sh --libge-only ios-arm64

prebuild-libge-ios-arm64-simulator:
	tools/prebuild.sh --libge-only ios-arm64-simulator

prebuild-libge-android-arm64:
	tools/prebuild.sh --libge-only android-arm64

prebuild-libge: prebuild-libge-ios-arm64 prebuild-libge-ios-arm64-simulator prebuild-libge-android-arm64

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

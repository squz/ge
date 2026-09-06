# TiltBuggy differential-harness instance (🎯T137 oracle, re-derived on the
# reusable template at ~/think/differential-harness).
#
#   make check     build both adapters, run `dh check` (acceptance + ratchet)
#   make selftest  every declared plant must make `dh check` fail
#   make run       divergence-vs-horizon report only
#   make study SEED=<hex|#index>
#
# Reference = 2013 Chipmunk physics (../oracle.cpp on the vendored Chipmunk
# 6.2.1 at $(CM)); port = the box2d Scene (../../src/Scene.cpp). No libge, no
# SDL, no window: Scene.cpp needs only headers, box2d, and sqlite3 (ge::tweak).

ge := ../../../..
TB := ../..
CM ?= $(HOME)/work/marcelocantos.com/unwisegames/tiltbuggy/bricabrac/Chipmunk2D
DH ?= $(HOME)/think/differential-harness/bin/dh
B  := build

BOX2D := $(ge)/vendor/github.com/erincatto/box2d

CXX ?= c++
CC  ?= cc
CXXFLAGS := -std=c++20 -O2 -w -DNDEBUG \
    -I$(ge)/include -I$(ge)/vendor/include -I$(BOX2D)/include \
    -I$(ge)/headers/sdl3/include -I$(TB)/src
CM_CFLAGS    := -O2 -w -DNDEBUG -I$(CM)/include -I$(CM)/include/chipmunk
BOX2D_CFLAGS := -I$(BOX2D)/include -I$(BOX2D)/src -O2 -std=c17
# Same feature flags Module.mk gives ge's own sqlite3.o.
SQLITE_CFLAGS := -O2 -w -I$(ge)/vendor/include -DSQLITE_ENABLE_SESSION \
    -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE

CM_SRC    := $(shell find $(CM)/src -name '*.c' 2>/dev/null)
CM_OBJ    := $(patsubst $(CM)/src/%.c,$(B)/chipmunk/%.o,$(CM_SRC))
BOX2D_SRC := $(wildcard $(BOX2D)/src/*.c)
BOX2D_OBJ := $(patsubst $(BOX2D)/src/%.c,$(B)/box2d/%.o,$(BOX2D_SRC))

.PHONY: all check selftest run study clean
all: $(B)/ref $(B)/port

check: all
	$(DH) check
selftest: all
	$(DH) selftest
run: all
	$(DH) run
study: all
	$(DH) study $(SEED)

$(B)/ref: ref_adapter.cpp dh_common.h ../oracle.cpp $(CM_OBJ)
	@test -d $(CM) || { echo "Chipmunk 6.2.1 source not found at $(CM) (set CM=)"; exit 1; }
	$(CXX) $(CXXFLAGS) $(CM_CFLAGS) ref_adapter.cpp $(CM_OBJ) -o $@

$(B)/port: port_adapter.cpp dh_common.h $(TB)/src/Scene.cpp $(TB)/src/Scene.h $(BOX2D_OBJ) $(B)/sqlite3.o
	$(CXX) $(CXXFLAGS) port_adapter.cpp $(TB)/src/Scene.cpp $(BOX2D_OBJ) $(B)/sqlite3.o -o $@

$(B)/chipmunk/%.o: $(CM)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CM_CFLAGS) -c $< -o $@

$(B)/box2d/%.o: $(BOX2D)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOX2D_CFLAGS) -c $< -o $@

$(B)/sqlite3.o: $(ge)/vendor/src/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) $(SQLITE_CFLAGS) -c $< -o $@

clean:
	rm -rf $(B)

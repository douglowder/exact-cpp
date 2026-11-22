# exact-stream (the Mac host), vm_check, and the app bundles they run, built
# from this exact-cpp checkout with its unchanged exact_rt core and Mac boards.
# Run ../tools/gen_vm.sh first: the bundles are built from plan2cpp's output
# with PLAN2CPP_VM set.
EXACT_CPP ?= $(abspath ../../..)
M5GFX ?= $(shell arduino-cli config get directories.user 2>/dev/null || echo $(HOME)/Documents/Arduino)/libraries/M5GFX

CXX := clang++
WARN := -Wall -Wextra -Wshadow
INC := -I. -I../proto -I../vm -I$(EXACT_CPP)/rt/src -I$(EXACT_CPP)/rt-mac -I$(EXACT_CPP)/mac \
  -I$(EXACT_CPP)/apps/level/cpp -I$(EXACT_CPP)/apps/badge/cpp -isystem $(M5GFX)/src \
  -DBADGE_FIXTURE=\"$(EXACT_CPP)/apps/badge/fixture\"
CXXFLAGS := -std=c++17 -O2 $(WARN) $(INC)

R := $(EXACT_CPP)/rt/src
CORE := $(R)/rt_animation.cpp $(R)/rt_value.cpp $(R)/rt_view.cpp $(R)/rt_runtime.cpp $(R)/rt_host.cpp \
  $(R)/rt_board.cpp $(R)/rt_canvas.cpp $(R)/rt_route.cpp $(R)/rt_router.cpp $(R)/rt_transport.cpp \
  $(EXACT_CPP)/rt-mac/rt_mac.cpp $(EXACT_CPP)/rt-mac/speaker_mac.cpp $(EXACT_CPP)/rt-mac/gfx_fonts.cpp
LDFLAGS := -framework AudioToolbox -framework CoreFoundation -framework IOKit -lz

A := $(EXACT_CPP)/apps
M := $(EXACT_CPP)/mac
# The apps as plan2cpp writes them with PLAN2CPP_VM set (tools/gen_vm.sh): the
# same C++ plus each body as rt_vm bytecode, so every bundle can also be sent
# as a plan blob.
VMGEN ?= ../build/vmgen
APP_level := $(VMGEN)/level_vm.cpp $(A)/level/cpp/level_glue.cpp $(M)/level_app.cpp
APP_badge := $(VMGEN)/badge_vm.cpp $(A)/badge/cpp/badge_data.cpp $(A)/badge/cpp/sha256.cpp $(M)/badge_app.cpp
APP_metronome := $(VMGEN)/metronome_vm.cpp $(M)/metronome_app.cpp
APP_motion := $(VMGEN)/motion_vm.cpp $(M)/motion_app.cpp
APP_knob := $(VMGEN)/knob_vm.cpp knob_app.cpp
APPS := level badge metronome motion knob

BIN := ../build/host
VM := ../vm/rt_vm.cpp ../vm/plan_blob.cpp
all: $(BIN)/exact-stream $(BIN)/vm_check $(foreach a,$(APPS),$(BIN)/apps/$(a).bundle)

$(BIN)/exact-stream: stream_main.cpp stream_board.cpp stream_board.h ../proto/exact_stream_proto.h $(VM) ../vm/*.h $(CORE)
	mkdir -p $(BIN)/apps
	$(CXX) $(CXXFLAGS) stream_main.cpp stream_board.cpp $(VM) $(CORE) $(LDFLAGS) -o $@

$(BIN)/vm_check: vm_check.cpp $(VM) ../vm/*.h $(CORE)
	$(CXX) $(CXXFLAGS) vm_check.cpp $(VM) $(CORE) $(LDFLAGS) -o $@

define bundle
$(BIN)/apps/$(1).bundle: $(M)/app_entry.cpp vm_entry.cpp $$(APP_$(1)) $(BIN)/exact-stream
	$$(CXX) $$(CXXFLAGS) -bundle -bundle_loader $(BIN)/exact-stream $(M)/app_entry.cpp vm_entry.cpp $$(APP_$(1)) -o $$@
endef
$(foreach a,$(APPS),$(eval $(call bundle,$(a))))

.PHONY: all

# Apps on the Mac boards. `make` builds, for each app (level, badge, metronome, motion),
# build/APP-mac (with a display) and build/APP-headless (no display); `make
# check` builds them with AddressSanitizer and UBSan (-san); `make test` runs
# tools/mac_check.py on the level app against the recorded board run,
# tools/badge_check.py on the badge app against Exact's runner, and
# tools/sound_check.py on the metronome's voices against Exact's runner (no hardware).
#
# The FreeSans faces come from the installed M5GFX library (the same data the
# board draws with); M5GFX sets its path.
M5GFX ?= $(shell arduino-cli config get directories.user)/libraries/M5GFX
ifeq ($(wildcard $(M5GFX)/src/lgfx/Fonts/GFXFF/FreeSans9pt7b.h),)
$(error M5GFX fonts not found under $(M5GFX); install M5GFX 0.2.26 or set M5GFX=<path>)
endif

CXX := clang++
WARN := -Wall -Wextra -Wpedantic -Wshadow -Werror
CXXFLAGS := -std=c++17 -O2 $(WARN) -I. -I../rt/src -I../rt-mac -I../apps/level/cpp \
  -isystem $(M5GFX)/src -I../apps/badge/cpp -DBADGE_FIXTURE=\"$(abspath ../apps/badge/fixture)\"
SAN := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

CORE := \
  ../rt/src/rt_animation.cpp \
  ../rt/src/rt_value.cpp \
  ../rt/src/rt_view.cpp \
  ../rt/src/rt_runtime.cpp \
  ../rt/src/rt_host.cpp \
  ../rt/src/rt_board.cpp \
  ../rt/src/rt_canvas.cpp \
  ../rt/src/rt_route.cpp \
  ../rt/src/rt_router.cpp \
  ../rt/src/rt_transport.cpp \
  ../rt-mac/rt_mac.cpp \
  ../rt-mac/speaker_mac.cpp \
  ../rt-mac/gfx_fonts.cpp
HEADERS := $(wildcard *.h *.hpp ../rt/src/*.h ../rt/src/*.hpp ../rt-mac/*.h ../rt-mac/*.hpp \
  ../apps/level/cpp/*.h ../apps/badge/cpp/*.h)
# The speaker's output (speaker_mac.cpp).
LDFLAGS := -framework AudioToolbox -framework CoreFoundation

LEVEL := ../apps/level/cpp
BADGE := ../apps/badge/cpp
APP_level := $(LEVEL)/app_gen.cpp $(LEVEL)/level_glue.cpp level_app.cpp
APP_badge := $(BADGE)/app_gen.cpp $(BADGE)/badge_data.cpp $(BADGE)/sha256.cpp badge_app.cpp
APP_metronome := ../apps/metronome/cpp/app_gen.cpp metronome_app.cpp
APP_motion := ../apps/motion/cpp/app_gen.cpp motion_app.cpp
APPS := level badge metronome motion

# $(1): app; $(2): program (mac or headless); $(3): its main file.
define program
build/$(1)-$(2): $(3) $$(APP_$(1)) $$(CORE) $$(HEADERS) | build
	$$(CXX) $$(CXXFLAGS) $(3) $$(APP_$(1)) $$(CORE) $$(LDFLAGS) -o $$@
build/$(1)-$(2)-san: $(3) $$(APP_$(1)) $$(CORE) $$(HEADERS) | build
	$$(CXX) $$(CXXFLAGS) $$(SAN) $(3) $$(APP_$(1)) $$(CORE) $$(LDFLAGS) -o $$@
endef
$(foreach a,$(APPS),$(eval $(call program,$(a),mac,display.cpp)))
$(foreach a,$(APPS),$(eval $(call program,$(a),headless,headless.cpp)))

all: $(foreach a,$(APPS),build/$(a)-mac build/$(a)-headless)

check: $(foreach a,$(APPS),build/$(a)-mac-san build/$(a)-headless-san)

test: check
	python3 -I ../tools/mac_check.py build/level-mac-san build/level-headless-san \
	  ../comparison/planb.json ../comparison/planb-final.png \
	  ../comparison/planb-numeric.png build/check
	python3 -I ../tools/badge_check.py build/badge-mac-san build/badge-headless-san \
	  ../comparison/badge-runner.json build/check
	python3 -I ../tools/sound_check.py build/metronome-headless-san \
	  ../comparison/metronome-runner.json

build:
	mkdir -p build

# ExactMac: one window that runs any app, chosen from its App menu (container.mm). Each app
# is a bundle (its app_gen.cpp, glue and app_entry.cpp) linked against the container's core.
CONTAINER := build/ExactMac
container: $(CONTAINER) $(foreach a,$(APPS),build/apps/$(a).bundle)

$(CONTAINER): container.mm $(CORE) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -fobjc-arc container.mm $(CORE) $(LDFLAGS) -framework AppKit -o $@

define bundle
build/apps/$(1).bundle: app_entry.cpp $$(APP_$(1)) $$(HEADERS) $(CONTAINER) | build/apps
	$$(CXX) $$(CXXFLAGS) -bundle -bundle_loader $(CONTAINER) app_entry.cpp $$(APP_$(1)) -o $$@
endef
$(foreach a,$(APPS),$(eval $(call bundle,$(a))))

build/apps:
	mkdir -p build/apps

.PHONY: all check test container

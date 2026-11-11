# Tests of the exact_rt core on the Mac. `make test` builds route_check,
# ext_check and data_check with AddressSanitizer and UBSan, runs Exact's
# route corpus through route_check (tools/route_corpus.py), runs ext_check
# (how the runtime binds host extensions, on the level app's tables), and
# runs data_check (`data-*` props, on the marks app in marks/). EXACT is
# Exact's checkout (the `exact` submodule).
EXACT ?= ../exact

CXX := clang++
WARN := -Wall -Wextra -Wpedantic -Wshadow -Werror
CXXFLAGS := -std=c++17 $(WARN) -I../rt/src
SAN := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

ROUTE := route_check.cpp ../rt/src/rt_route.cpp
CORE := $(wildcard ../rt/src/*.cpp)
LEVEL := ../apps/level/cpp
EXT := ext_check.cpp $(LEVEL)/app_gen.cpp $(CORE)
DATA := data_check.cpp marks/app_gen.cpp $(CORE)

all: build/route_check build/ext_check build/data_check

build/route_check: $(ROUTE) ../rt/src/rt_route.h | build
	$(CXX) $(CXXFLAGS) -O2 $(ROUTE) -o $@

build/route_check-san: $(ROUTE) ../rt/src/rt_route.h | build
	$(CXX) $(CXXFLAGS) $(SAN) $(ROUTE) -o $@

build/ext_check: $(EXT) $(wildcard ../rt/src/*.h ../rt/src/*.hpp) $(LEVEL)/ext_attitude.h | build
	$(CXX) $(CXXFLAGS) -I$(LEVEL) -O2 $(EXT) -o $@

build/ext_check-san: $(EXT) $(wildcard ../rt/src/*.h ../rt/src/*.hpp) $(LEVEL)/ext_attitude.h | build
	$(CXX) $(CXXFLAGS) -I$(LEVEL) $(SAN) $(EXT) -o $@

build/data_check: $(DATA) $(wildcard ../rt/src/*.h ../rt/src/*.hpp) marks/ext_marks.h | build
	$(CXX) $(CXXFLAGS) -Imarks -O2 $(DATA) -o $@

build/data_check-san: $(DATA) $(wildcard ../rt/src/*.h ../rt/src/*.hpp) marks/ext_marks.h | build
	$(CXX) $(CXXFLAGS) -Imarks $(SAN) $(DATA) -o $@

test: build/route_check-san build/ext_check-san build/data_check-san
	python3 -I ../tools/route_corpus.py build/route_check-san $(EXACT)/route/tests/corpus.json
	build/ext_check-san
	build/data_check-san

build:
	mkdir -p build

.PHONY: all test

#!/bin/sh
# Writes each app's C++ with its bodies also as rt_vm bytecode (plan2cpp with
# PLAN2CPP_VM set) into build/vmgen, for the bundles in host/.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
mkdir -p "$here/build/vmgen"
cd "$root"
cargo build --release -q -p plan2cpp
emit() { PLAN2CPP_VM=1 ./target/release/plan2cpp emit "$1" "$here/build/vmgen/$2_vm.cpp"; }
for a in metronome motion badge level; do emit "apps/$a/app.contract" "$a"; done
emit poc/watcher-stream/apps/knob/app.contract knob
ls -l "$here/build/vmgen"

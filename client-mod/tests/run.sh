#!/bin/sh
# Builds and runs the mod's host-side tests (frame decoding and the entity table).
# These need no Windows and no game: any C++17 compiler will do.
set -e
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${TMPDIR:-/tmp}/fable_online_world_tests
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$dir/../src" \
  "$dir/world_test.cpp" "$dir/../src/world/entities.cpp" "$dir/../src/world/snapshot.cpp" -o "$out"
"$out"
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$dir/../src" \
  "$dir/objects_test.cpp" "$dir/../src/world/objects.cpp" -o "$out-objects"
"$out-objects"

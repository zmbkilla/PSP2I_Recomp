#!/bin/zsh
set -euo pipefail
cd "${0:A:h}"
# psprecomp now carries the Apple ARM64 fix (its commit ecc4521). A checkout
# where an earlier build applied the patch below as local changes: undo them,
# so the submodule update can move to that commit (the patch step then finds
# the fix already there). Only while the checked-out commit lacks the fix.
if ! git -C psprecomp grep -q _psp_fiber_switch HEAD -- src/hle/threadman.c 2>/dev/null &&
   git -C psprecomp apply --reverse --check "$PWD/port/macos/psprecomp-apple-arm64.patch" 2>/dev/null; then
    git -C psprecomp apply --reverse "$PWD/port/macos/psprecomp-apple-arm64.patch"
fi
git submodule update --init --recursive
patch_file="$PWD/port/macos/psprecomp-apple-arm64.patch"
if git -C psprecomp apply --check "$patch_file" 2>/dev/null; then
    git -C psprecomp apply "$patch_file"
elif ! git -C psprecomp apply --reverse --check "$patch_file" 2>/dev/null; then
    echo "The macOS runtime patch conflicts with local changes." >&2
    exit 1
fi
cmake -S psprecomp -B psprecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build psprecomp/build --target allegrexrecomp -j 4
mkdir -p port/gen
if [[ ! -f port/gen/recomp_funcs.c || EBOOT.BIN -nt port/gen/recomp_funcs.c || port/hooks.txt -nt port/gen/recomp_funcs.c || port/fixes.txt -nt port/gen/recomp_funcs.c ]]; then
    psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit EBOOT.BIN port/gen recomp --hooks port/hooks.txt --fixes port/fixes.txt
fi
cmake -S port/macos -B port/build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build port/build-macos --target psp2i -j 4

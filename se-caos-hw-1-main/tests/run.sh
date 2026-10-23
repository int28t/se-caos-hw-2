#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$project_dir/build"}
sanitizers=${ENABLE_SANITIZERS:-ON}
compiler=${CC:-}
if [ -z "$compiler" ] && [ "$(uname -s)" = Darwin ]; then
    for candidate in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang; do
        if [ -x "$candidate" ]; then
            compiler=$candidate
            break
        fi
    done
fi
if [ -n "$compiler" ]; then
    cmake -S "$project_dir" -B "$build_dir" "-DCMAKE_C_COMPILER=$compiler"
fi
cmake -S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug "-DENABLE_SANITIZERS=$sanitizers"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure
mkdir -p "$project_dir/results"
for scenario in demo hand emergency compatible; do
    "$build_dir/crossroad" "$project_dir/data/$scenario.conf" "$project_dir/results/$scenario.log" >/dev/null
done

#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mode=${1:-asan}
if [ "$#" -gt 1 ]; then
    printf 'Usage: %s [asan|tsan|none]\n' "$0" >&2
    exit 2
fi
case "$mode" in
    asan) default_build=build; sanitizers=ON; tsan=OFF ;;
    tsan) default_build=build-tsan; sanitizers=OFF; tsan=ON ;;
    none) default_build=build-plain; sanitizers=OFF; tsan=OFF ;;
    *) printf 'Usage: %s [asan|tsan|none]\n' "$0" >&2; exit 2 ;;
esac
build_dir=${BUILD_DIR:-"$project_dir/$default_build"}
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
    set -- "-DCMAKE_C_COMPILER=$compiler"
else
    set --
fi
cmake -S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON "-DENABLE_SANITIZERS=$sanitizers" "-DENABLE_TSAN=$tsan" "$@"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure
results_dir=${RESULTS_DIR:-"$project_dir/results"}
mkdir -p "$results_dir"
for scenario in demo hand emergency compatible; do
    "$build_dir/crossroad" "$project_dir/data/$scenario.conf" "$results_dir/$scenario.log" >/dev/null
done

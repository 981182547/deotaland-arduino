#!/usr/bin/env sh
set -eu
tests_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
library_root=$(dirname "$tests_dir")
mkdir -p "$tests_dir/.build"
"${CC:-cc}" -std=c11 -Wall -Wextra -pedantic -I"$library_root/src" \
    -c "$tests_dir/public_headers.c" -o "$tests_dir/.build/public_headers.o"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -pedantic \
    -I"$library_root/src" "$library_root"/src/*.cpp "$tests_dir/host_tests.cpp" \
    "$tests_dir/.build/public_headers.o" \
    -o "$tests_dir/.build/host_tests"
"$tests_dir/.build/host_tests"

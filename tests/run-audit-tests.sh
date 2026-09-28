#!/usr/bin/env sh
set -eu
tests_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
library_root=$(dirname "$tests_dir")
mkdir -p "$tests_dir/.build"
set --
for source in "$library_root"/src/*.cpp; do
    case "$source" in */agent_link_platform.cpp) continue ;; esac
    set -- "$@" "$source"
done
"${CXX:-c++}" -std=c++11 -Wall -Wextra -pedantic -I"$library_root/src" \
    "$@" "$tests_dir/audit_tests.cpp" -o "$tests_dir/.build/audit_tests"
"$tests_dir/.build/audit_tests"

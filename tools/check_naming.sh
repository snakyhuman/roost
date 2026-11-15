#!/usr/bin/env bash
# The naming gate (ADR 011): each of the two naming styles is enforced where it
# applies, and only there.
#
# tests/gates/naming/ holds one directory per style. In each, accepted.cpp
# follows the style and must pass clang-tidy; rejected.cpp breaks it and must
# fail with a readability-identifier-naming diagnostic, not with something
# else. qml_api/.clang-tidy is a symlink to src/qml_api/.clang-tidy, because
# clang-tidy takes the style from the directory of the file: a copy could drift
# from the real config and the gate would go on testing the copy.
#
# A gate that only checked rejection would also pass a config that rejects
# everything; the accepted files are what rule that out.
#
# Usage: tools/check_naming.sh            (CLANG_TIDY=clang-tidy-20 to pick one)
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
tidy=${CLANG_TIDY:-clang-tidy}
fixtures=$root/tests/gates/naming

if ! command -v "$tidy" >/dev/null 2>&1; then
    echo "check_naming: $tidy not found; set CLANG_TIDY" >&2
    exit 2
fi

failed=0

# The flags after -- stand in for a compile database: the fixtures include
# nothing and need only the language standard.
run_tidy() {
    "$tidy" --quiet "$1" -- -std=c++23 2>&1
}

for style in core qml_api; do
    accepted=$fixtures/$style/accepted.cpp
    rejected=$fixtures/$style/rejected.cpp

    if output=$(run_tidy "$accepted"); then
        echo "ok:   $style/accepted.cpp passes"
    else
        echo "FAIL: $style/accepted.cpp follows the $style style and was rejected:"
        printf '%s\n' "$output" | sed 's/^/    /'
        failed=1
    fi

    if output=$(run_tidy "$rejected"); then
        echo "FAIL: $style/rejected.cpp breaks the $style style and passed"
        failed=1
    elif ! grep -q 'readability-identifier-naming' <<<"$output"; then
        echo "FAIL: $style/rejected.cpp failed, but not on naming:"
        printf '%s\n' "$output" | sed 's/^/    /'
        failed=1
    else
        echo "ok:   $style/rejected.cpp is rejected on naming"
    fi
done

exit "$failed"

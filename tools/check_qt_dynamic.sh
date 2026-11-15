#!/usr/bin/env bash
# Qt is linked dynamically (design.md §10): LGPLv3 lets the user replace the Qt
# libraries, and a static Qt would take that away. Each binary must name QtCore
# among the libraries it loads; a statically linked Qt would not appear there.
#
# Usage: tools/check_qt_dynamic.sh build/dev
set -uo pipefail

build=${1:?usage: tools/check_qt_dynamic.sh <build dir>}
failed=0

for binary in bin/roost bin/roostd; do
    path=$build/$binary
    if [ ! -x "$path" ]; then
        echo "FAIL: $path was not built"
        failed=1
        continue
    fi

    case $(uname -s) in
        Darwin) libs=$(otool -L "$path") ;;
        *) libs=$(ldd "$path") ;;
    esac

    # QtCore.framework on macOS, libQt6Core.so.6 on Linux.
    if grep -qE 'QtCore\.framework|libQt6Core\.so' <<<"$libs"; then
        echo "ok:   $binary loads QtCore dynamically"
    else
        echo "FAIL: $binary does not load QtCore dynamically:"
        printf '%s\n' "$libs" | sed 's/^/    /'
        failed=1
    fi
done

exit "$failed"

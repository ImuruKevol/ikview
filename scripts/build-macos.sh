#!/bin/bash
set -euo pipefail
task_with_gui_tests=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-gui-tests) task_with_gui_tests=1; shift;;
        --help|-h)
            printf '%s\n' 'Usage: scripts/build-macos.sh [--with-gui-tests]' \
                'The default runs tests without opening desktop windows.' \
                '--with-gui-tests also runs native window, input and Metal renderer tests.'
            exit 0;;
        *) printf 'IKView build: unknown option: %s\n' "$1" >&2; exit 64;;
    esac
done
task_root="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$task_root" -B "$task_root/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$task_root/build" --parallel "$(sysctl -n hw.ncpu)"
# Bind the bundle identity and Info.plist for native macOS launches as well as
# terminal launches; the linker's executable-only ad-hoc signature is insufficient.
codesign --force --sign - --identifier io.ikview.desktop "$task_root/build/ikview.app"
codesign --verify --strict "$task_root/build/ikview.app"
if [ "$task_with_gui_tests" -eq 1 ]; then
    printf '%s\n' 'Running all tests, including tests that open desktop windows.'
    ctest --test-dir "$task_root/build" --output-on-failure
else
    printf '%s\n' 'Running tests without desktop windows. Use --with-gui-tests to include native UI tests.'
    ctest --test-dir "$task_root/build" --output-on-failure -LE desktop-gui
fi

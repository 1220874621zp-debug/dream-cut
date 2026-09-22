#!/bin/bash
# Fetch skia's nested deps (gn-src, sfntly) at pinned commits — plain git
# submodule update does not bring these in; friction's skia CMake needs them.
set -u
GIT=/usr/bin/git
SKIA="$(cd "$(dirname "$0")" && pwd)/src/skia"

fetch_nested() {
    local path="$1" url="$2" sha="$3"
    rm -rf "$SKIA/$path"
    mkdir -p "$SKIA/$path"
    cd "$SKIA/$path" || return 1
    $GIT init -q .
    $GIT remote add origin "$url"
    if ! timeout 240 $GIT fetch -q --depth 1 origin "$sha"; then
        echo "FETCH FAILED: $path"
        return 1
    fi
    $GIT checkout -q FETCH_HEAD
    echo "OK: $path @ $($GIT rev-parse --short HEAD)"
}

fetch_nested "gn-src" "https://github.com/friction2d/gn.git" "70a9617aad7c09642457b6296d35638b97375dad" || exit 1
fetch_nested "third_party/externals/sfntly" "https://github.com/friction2d/sfntly.git" "b55ff303ea2f9e26702b514cf6a3196a2e3e2974" || exit 1

ls "$SKIA/gn-src/build/gen.py" "$SKIA/third_party/externals/sfntly/README.md"

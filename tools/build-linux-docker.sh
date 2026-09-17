#!/usr/bin/env bash
# Builds and tests Light Host for Linux, locally, in a container.
#
#   tools/build-linux-docker.sh            # configure, build, run every test
#   tools/build-linux-docker.sh --build    # stop after the build
#   tools/build-linux-docker.sh --shell    # drop into the container
#
# WHY THIS EXISTS
#
# There is no GCC on the development machine, so until this script existed the
# only evidence that Linux still compiled arrived from CI, after the push. That
# is the wrong order, and BACKLOG.md said so: 5.2.0 fell into the gap twice in
# one day -- once on an expectEquals overload MSVC resolved and GCC refused, and
# once on the GUI tests failing under X11. A clean local build and 1131 passing
# tests said nothing about either.
#
# GCC is also much fussier than MSVC, which matters now that warnings are
# errors. Most of what /WX will never catch, -Werror here will.
#
# This is not a substitute for CI. It does not cover macOS, and its CMake and
# Ninja come from apt rather than from lukka/get-cmake. It covers the compiler,
# which is what actually breaks.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
image=lighthost-linux-build

# Git Bash rewrites anything that looks like a Unix path in an argument into a
# Windows one before the process sees it, so `-w /work` arrived at the daemon as
# `C:/Anthropic/.Git/work` and the run died on "needs to be an absolute path".
# MSYS_NO_PATHCONV turns that off, and `pwd -W` gives the Windows spelling of
# the repository that the daemon can actually mount.
hostpath="$here"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        export MSYS_NO_PATHCONV=1
        export MSYS2_ARG_CONV_EXCL='*'
        hostpath=$(cd "$here" && pwd -W)
        ;;
esac

# Its own build directory. Sharing build/ninja-release with anything else would
# mean a CMakeCache.txt holding container paths, which is precisely the failure
# the no-caching comment at the top of ci.yml describes.
builddir=build/linux-docker

mode=${1:-all}

if ! docker info >/dev/null 2>&1; then
    echo "docker is not running" >&2
    exit 2
fi

# Rebuilt only when the dependency list or the Dockerfile changes; docker's
# layer cache decides, so this is cheap on every run after the first.
echo "==> preparing the image"
docker build --quiet \
             -f "$hostpath/tools/Dockerfile.linux-build" \
             -t "$image" \
             "$hostpath"

run() {
    docker run --rm \
        -v "$hostpath:/work" \
        -w /work \
        "$image" \
        bash -eo pipefail -c "$1"
}

if [[ "$mode" == "--shell" ]]; then
    exec docker run --rm -it -v "$hostpath:/work" -w /work "$image" bash
fi

echo "==> configure"
run "cmake --preset ninja-release -B $builddir -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja"

echo "==> build"
run "cmake --build $builddir -j\$(nproc)"

if [[ "$mode" == "--build" ]]; then
    echo "LINUX BUILD OK"
    exit 0
fi

echo "==> test"
# --output-on-failure, and no -L: CI runs every registered test in one
# invocation, so this does too. On Linux that is unit + the three smoke tests;
# unit-gui is deliberately not registered there (see CMakeLists.txt).
run "ctest --test-dir $builddir --output-on-failure"

echo "LINUX BUILD AND TESTS OK"

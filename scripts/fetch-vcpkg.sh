#!/usr/bin/env bash
# Fetches vcpkg pinned to the exact commit used everywhere else in the project
# (vcpkg.json's builtin-baseline, and the vcpkg/ git submodule for local dev).
#
# Used by CI and Docker, neither of which can rely on a submodule checkout the way
# local dev does: Docker's build context can't see a submodule's .git (it's a gitlink
# into the superproject's .git/modules, which lives outside the context), and a CI
# submodule checkout would pull vcpkg's full history for no benefit. A shallow fetch
# of just the pinned commit is faster and avoids both problems.
#
# Keep VCPKG_COMMIT in sync with vcpkg.json's builtin-baseline and
# `git -C vcpkg rev-parse HEAD` on a machine with the submodule checked out.
set -euo pipefail

VCPKG_COMMIT="30ef65cad98f08e7197c9a1656fbd871bcb72f2d"
DEST="${1:-vcpkg}"

git init "$DEST"
git -C "$DEST" remote add origin https://github.com/microsoft/vcpkg.git
git -C "$DEST" fetch --depth 1 origin "$VCPKG_COMMIT"
git -C "$DEST" checkout FETCH_HEAD
"$DEST/bootstrap-vcpkg.sh" -disableMetrics

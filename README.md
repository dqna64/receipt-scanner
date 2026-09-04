# receipt-scanner

Personal receipt-scanning app. See `spec.md` (what/why) and `plan.md` (how/when, step-by-step).

## Prerequisites (macOS, arm64 dev)

```
brew install autoconf autoconf-archive automake libtool cmake
```

These are needed by some vcpkg ports (e.g. libvips' libexif) that build via autotools.

Clang from the Xcode command line tools (C++20) is used locally; the vcpkg toolchain
picks it up automatically.

## First-time setup

```
git submodule update --init --recursive   # fetches vcpkg
git config core.hooksPath .githooks        # build+test before every push
```

vcpkg is a git submodule pinned to a specific commit (see `vcpkg.json`'s `builtin-baseline`),
not a system install — this keeps dependency versions reproducible across machines and CI.

## Build

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
```

The first build compiles Drogon and its dependencies from source via vcpkg — this takes
30-60 minutes. Subsequent builds are incremental. CI caches the vcpkg binary cache so this
cost is paid once, not on every push (see Step 3).

## Test

```
ctest --test-dir build --output-on-failure
```

## Run

```
./build/receipt_scanner_server
curl localhost:8080/api/v1/health
```

## Dual-architecture

Everything must build on arm64 (macOS dev, this machine) and x86_64 (Linux VPS, CI only).
Never cross-build the x86_64 image on the Mac (QEMU emulation of a C++ build is hours slow) —
the x86_64 deploy image is produced only in GitHub Actions.

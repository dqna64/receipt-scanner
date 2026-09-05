# receipt-scanner

Personal receipt-scanning app. See `spec.md` (what/why) and `plan.md` (how/when, step-by-step).

## Prerequisites (macOS, arm64 dev)

```
brew install autoconf autoconf-archive automake libtool cmake dbmate
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

## Database

```
docker compose up -d postgres
dbmate up      # applies db/migrations/ -- reads DATABASE_URL from .env
```

`dbmate up`/`down` are both clean and reversible (plain SQL migrations, no native Postgres
ENUM types — see the schema comments). The app's own `DbClient` reads discrete `DB_*` vars
from `.env` (same credentials as `POSTGRES_*`, used by dbmate's `DATABASE_URL`).

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

Since Step 4 this includes DB integration tests — Postgres must be running and migrated
(see Database, above) or they'll fail with a connection error. The pre-push hook checks for
a reachable Postgres before building and fails fast with a clear message if it isn't.

## Run

```
./build/receipt_scanner_server
curl localhost:8080/api/v1/health
```

## Docker / Compose (dev environment)

```
cp .env.example .env   # first time only
docker compose up -d
curl localhost:8080/api/v1/health   # through Caddy
docker compose down
```

Brings up app + Postgres + MinIO + Caddy. Postgres (`5432`) and MinIO (`9000`/`9001`) are
mapped to the host for dev convenience (psql, mc, the MinIO console); only Caddy (`8080`)
fronts the app, matching the eventual production posture even in dev. The app image builds
fresh from source inside Docker (vcpkg is cloned there directly, not copied from the host
submodule checkout — see the Dockerfile comment) — first build takes ~5-10 min, same as the
native build; a BuildKit cache mount keeps subsequent builds fast even after Dockerfile edits.

## Dual-architecture

Everything must build on arm64 (macOS dev, this machine) and x86_64 (Linux VPS, CI only).
Never cross-build the x86_64 image on the Mac (QEMU emulation of a C++ build is hours slow) —
the x86_64 deploy image is produced only in GitHub Actions.

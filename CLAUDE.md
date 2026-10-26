# Scope of unsupervised action

Decided 2026-10-12, during Step 8 CI troubleshooting.

Default allowed scope: code in this repo, and package dependencies installed and run inside
the Docker containers defined here (docker-compose.yml, Dockerfile).

Beyond that:

- **This repo's GitHub remote (pushes to `main`, triggering/re-running CI)** — allowed
  unsupervised. Still never force-push, never touch repo/branch settings or Actions
  permissions, never delete a workflow run, without asking first.
- **Host machine, outside Docker** — may USE what's already installed (compilers, CLI
  tools, etc.) for diagnosis or builds. May NOT install anything new or upgrade anything
  existing (brew/apt/npm -g/pip/toolchain installs, OS-level config) without explicit
  permission first.
- **External accounts/credentials** (Anthropic API key, email provider, VPS/DNS/TLS for
  the eventual deploy, a container registry for a self-hosted image) — never set up or
  spend against unsupervised. Implement the code path; the user provisions the actual
  account/credential.
  - Specifically: if a self-hosted patched image on a registry (e.g. GHCR) ever becomes
    the right fix for the MinIO Docker Hub situation (see .agent_dqna64/plans/plan.md Step 8 / .agent_dqna64/log.md
    2026-10-05), ask first — the user provisions the registry.

# Local Docker on the dev laptop (8 GB M1)

Docker Desktop's default VM (4 GB / 4 CPUs) lags this laptop, so it is set to **2 GB RAM,
2 CPUs, 1 GB swap** (Settings -> Resources; also logged in the machine log). Within that budget:

- Run only `docker compose up -d postgres minio` - the integration tests and pre-push hook
  need nothing else (the test binary builds natively with Clang and talks to them on localhost).
  Together they use ~125 MB.
- No local `docker build` / `docker compose up` of the app: the GCC build stage needs >1 GB
  per compiler process and OOMs or swaps the VM. GCC coverage comes from CI.
- `docker compose stop` (or quit Docker Desktop) when done, to give the VM's memory back.

# Implementation Plan

Companion to `spec.md` (the what/why). This is the how/when. Built in small steps: each step = one PR = one conceptual thing, reviewed and understood before the next begins.

## TODO

Near-term actionable items only — the full step sequence is below.

- [ ] Create GitHub private repo; `git init` + first push (spec.md, plan.md, log.md)
- [ ] Anthropic API key (needed by Step 11, cheap to grab now)
- [ ] Buy/choose domain for the VPS (needed by Step 18)
- [ ] Pick VPS provider (needed by Step 18)
- [ ] Offsite backup target account, B2 or similar (needed by Step 18)
- [ ] START COLLECTING GOLDEN-SET RECEIPTS — photograph every receipt from normal shopping from now on; need 20-30 varied (faded thermal, crumpled, long dockets, glare). Runs in parallel with everything.
- [ ] Step 1 (below): repo scaffold — CMake + vcpkg, hello Drogon `GET /api/v1/health`, Catch2 wired, clang-format/clang-tidy, README

## Working agreement

- Solo project, no PRs: work lands as commits straight to main. One step = one commit (or a small coherent series).
- Review gate (same spirit, new mechanism): each step is implemented in the working tree; you read the uncommitted diff (`git diff`), ask anything, request changes there; we commit only on your sign-off. Nothing builds on an unreviewed step.
- Build + tests run locally before every commit (pre-push hook from Step 1). CI on main is the Linux/sanitizer safety net, not the first line of defense; if main goes red anyway, fixing it is the next action before anything else.
- Each step below lists: **Deliverable** / **You'll be reviewing** (the concept to understand — this is the C++ curriculum) / **Verify** (how we prove it works).
- Per-step review notes worth keeping (what was learned, decisions made mid-step) go into log.md with that day's entry — they'd have lived in PR descriptions otherwise.
- Estimated diff sizes are targets; C++ boilerplate sometimes inflates them. If a step turns out too big to review comfortably, we split it.

## Phase 0 — Skeleton (everything after this has rails)

**Step 1: Repo + build + hello Drogon.**
Deliverable: CMake + vcpkg project; Drogon serving `GET /api/v1/health`; Catch2 wired with one trivial test; clang-format/clang-tidy configs; pre-push git hook running build + tests; README with build commands.
You'll be reviewing: project layout, CMake targets, how vcpkg pins dependencies, what a Drogon app()/controller is.
Verify: builds + test passes on your Mac (arm64). libvips already verified on arm64-osx (2026-07-11, see spec); confirm the remaining vcpkg deps resolve for both arm64-osx and x64-linux — dual-arch is a hard requirement (spec). README must list macOS host prereq: `brew install autoconf autoconf-archive automake libtool`.

**Step 2: Docker + Compose.**
Deliverable: multi-stage Dockerfile (vcpkg build → slim runtime); docker-compose with app + Postgres + MinIO + Caddy (MinIO not exposed); .env conventions. Dockerfile must build on both arches; on the Mac it builds arm64 natively (dev), the x86_64 deploy image is built only in CI (Step 3) — never QEMU cross-built locally.
You'll be reviewing: the build/runtime image split, Compose networking (who can reach whom), where secrets live.
Verify: `docker compose up` → health endpoint through Caddy.

**Step 3: CI.**
Deliverable: GitHub Actions on push to main — x86_64 Linux build, tests, ASan/UBSan job (this is also the only place the x86_64 deploy image is ever produced); vcpkg binary caching (non-negotiable: a cold Drogon+deps build runs 30-60 min, and private-repo Actions minutes are a finite free pool — cached runs take minutes).
You'll be reviewing: the workflow file; what sanitizers actually catch (demoed with a deliberately-buggy scratch file we compile once and delete).
Verify: green run on main; second push proves the cache hits.

**Step 4: Schema + async DB.**
Deliverable: dbmate setup; migration 001 with the full spec schema (incl. the two-axis receipt state — scan_state enum + proposed_at/reviewed_at timestamps + reclaim_count, no stored review-status column; tags + item_tags; recurring occurrences_generated + unique (recurring_id, occurrence_ordinal); all ON DELETE SET NULL / CASCADE rules per Invariants) + category/payment-method seeds; Drogon async DbClient wired via config; health endpoint now pings DB.
You'll be reviewing: the SQL itself (your data model made real), and your first coroutine — `co_await execSqlCoro` and what suspending actually means.
Verify: `dbmate up/down` clean; integration test queries through the coroutine path.

## Phase 1 — Auth (first vertical slice)

**Step 5: Register/login/logout + session filter.**
Deliverable: first-user-only registration (open only while users table is empty), argon2 via libsodium, sessions table + httpOnly cookie (~30-day sliding expiry — touch expires_at on authenticated requests), Drogon filter guarding all routes, login rate limit.
You'll be reviewing: a complete request→coroutine→DB→response slice; RAII around libsodium; how the auth filter composes with handlers.
Verify: curl script walks the whole flow; integration tests for wrong-password/second-registration/expired-session.

## Phase 2 — Capture core

**Step 6: SigV4 + ImageStore.**
Deliverable: `ImageStore` interface; S3 implementation (Put/Get/Delete) over Drogon HttpClient with hand-rolled SigV4; endpoint/region/credentials/addressing-style from config.
You'll be reviewing: the SigV4 algorithm itself (canonical request → string-to-sign → HMAC chain) — self-contained and worth understanding once in your life.
Verify: integration test round-trips bytes against Compose MinIO; a doc comment shows the config diff that would point it at AWS S3.

**Step 7: Image normalization.**
Deliverable: libvips pipeline (EXIF orient → strip → 2048px → WebP q80) running on a worker thread pool, bridged back to the coroutine.
You'll be reviewing: the CPU-work-off-the-event-loop pattern — the single most important async lesson in the project.
Verify: unit tests on fixture images (rotated, EXIF'd, huge, tiny); assert GPS gone, dimensions right.

**Step 8: Upload + image proxy.**
Deliverable: `POST /receipts` (multipart → normalize → hash (scoped user_id, image_sha256) → exact-dup conflict unless force=true → store → row, scan_state=uploaded); `POST /receipts/:id/image` (attach to imageless receipt — same pipeline incl. the exact-dup hash check (409 {duplicate_of} unless force=true, mirroring upload; force stores an independent copy under its own key), updates row instead of creating; sets scan_state=uploaded so the Step 10 worker verify-scans it — until Step 10 lands it just parks there); `GET /receipts` (offset pagination), `GET /receipts/:id`; `DELETE /receipts/:id` (confirmation-gated on the client; inbound receipt FKs ON DELETE SET NULL; always deletes the stored image object — single-owner by construction); image proxy endpoint with immutable cache headers; ownership authorization on every route (non-owned/missing id → 404, per spec API conventions); server accepts JPEG/PNG/WebP only (HEIC rejected at app layer — client converts); 15MB upload limit enforced at the app layer (Caddy enforces its copy in Step 2/18).
You'll be reviewing: multipart handling, streaming a body through a proxy handler, the zero-loss invariant in code.
Verify: curl a real photo up, fetch it back, row is correct.

**Step 9: Manual entry + reference data.**
Deliverable: `POST /receipts/manual` (all four kinds, negative totals, zero totals allowed, offsets link, born reviewed_at-set / proposed_at NULL → counts immediately); `PATCH /receipts/:id` (edit fields; "mark reviewed" = stamp reviewed_at); items CRUD (`GET /receipts/:id/items` + create/update/delete, incl. per-item tag assignment); categories CRUD; tags CRUD (item-level taxonomy); payment-methods CRUD incl. the stored_value flag.
You'll be reviewing: how the transaction-kind rules from the spec land in validation code.
Verify: integration tests — cash coffee, refund with link, transfer, zero-total warranty swap; invalid kinds rejected.

## Phase 3 — Scan pipeline (the heart, behind a fake first)

**Step 10: Worker + FakeScanProvider.**
Deliverable: `ScanProvider` interface + fake; event-loop-timer worker: claims uploaded rows via FOR UPDATE SKIP LOCKED, scan_state transitions, TWO-counter accounting (scan_attempts = provider/parse errors → cap 3 = scan_failed; reclaim_count = crash/timeout reclaims → high backstop, re-queues without spending a scan_attempt), 10-min reclaim, 2-scan concurrency cap; TRUST-gated mode discriminator per row (reviewed_at NULL → extraction mode; reviewed_at present → verify mode); scan_failed path + retry endpoint; verify-scan transitions for attached images (fill-empty-only, compare total exact / date ± tolerance / merchant fuzzy; completion stamps proposed_at; all-match → re-stamp reviewed_at so it stays reviewed silently; mismatch → proposed_at leads → needs_review with diff; terminal failure touches neither timestamp so a previously-reviewed row stays reviewed). Review/stats state derived from (proposed_at, reviewed_at) per Invariants, never a status column.
You'll be reviewing: async orchestration — the entire reason you picked all-async Drogon. State machine + crash-safety without burning a cent of API money.
Verify: integration tests incl. kill-the-worker-mid-scan → reclaim works.

**Step 11: AnthropicScanProvider + golden-set harness.**
Deliverable: real Haiku 4.5 call (structured outputs; category enum (receipt + item level) AND item-level tag enum injected from DB; kind-aware so return dockets scan as refunds); config plumbing; a harness script that runs the golden set and reports per-field accuracy vs targets (tags/items best-effort, not gated).
You'll be reviewing: the extraction prompt/schema (a spec-level artifact — review it like the spec), and the adapter boundary that makes provider swap trivial.
Verify: harness run on however many golden receipts exist so far.

## Phase 4 — Frontend MVP (first phone-usable moment)

**Step 12: Vite scaffold + auth + upload + history.**
Deliverable: vanilla JS app — login, upload page (camera/file, multi-photo select — one receipt row per image, client HEIC→JPEG downscale), exact-dup "are you sure? [shows existing]" confirm on 409, status polling with live trace incl. "⚠ possible match found" deep-link, history list; Vite proxy for dev.
Verify: upload a receipt from your actual phone on LAN and watch it reach pending review. **The app exists now.**

**Step 13: Review UI.**
Deliverable: pending queue (oldest first, count badge; queue = the needs_review derived state); review screen (image beside fields, item editing incl. category + tags, sum-mismatch warning, <30s-optimized layout); scan_failed handling (retry scan or fall back to manual field entry against the stored image); attach-image button on imageless receipts (manual/recurring); verify-scan mismatch diff (scanned vs recorded values side by side); mark reviewed; edit-after-review (keeps reviewed status); delete with confirmation.
Verify: you review a real receipt end-to-end; friction-ceiling sanity check.

**Step 14: Dedupe.**
Deliverable: `GET /receipts/:id/matches` (candidates computed at read time, no stored match state; served by the (user_id, total_cents, purchase_date) index; ?suggest= pins a force-included candidate — the attach-conflict dialog's "merge instead" deep-link lands here) + `POST /receipts/:id/merge-from`; match panel in the review screen; candidates = all matching receipts (total → ±days date → fuzzy merchant) ranked by match strength, labeled by flavor — has-image = "possible duplicate" (discard upload / keep both, images side by side), imageless (manual + recurring, pending or reviewed) = "matching transaction" (merge / keep both); nothing automatic, suggested action highlighted only; merge reuses the Step 10 verify/fill logic (no re-scan) — image_key + image_sha256 + raw_ocr_json move to the target, items copy only if target has none, inbound receipt FKs (offsets_receipt_id/recurring_id/purchase_receipt_id) repointed source→target, source row deleted (image object retained, ownership transfers) — mismatch resolved inline; discard deletes the upload's row + image object (Step 8 DELETE).
Verify: integration tests + a deliberate double-upload.

## Phase 5 — Recurring + browse

**Step 15: Recurring.**
Deliverable: definitions CRUD (custom interval, finite ends, kind, defaults, purchase link); scheduler tick creating pending receipts (idempotent via unique (recurring_id, occurrence_ordinal); occurrences_generated advances the ordinal; catch-up collapses multiple missed occurrences to the latest); auto-deactivate; merge-on-upload path; frontend pages.
Verify: integration tests with clock control; an Afterpay-shaped 4-payment plan exhausts correctly; a Jan-31 monthly anchor clamps to Feb 28/29 then returns to Mar 31 (no drift); a Feb-29 yearly anchor clamps in non-leap years; a double scheduler tick creates no duplicate; a multi-occurrence downtime gap generates only the latest missed occurrence.

**Step 16: Browse + quick-add polish.**
Deliverable: history filters (status/date/merchant/category/payment/tax-deductible/kind/source), receipt detail with zoomable image, quick-add form with defaults (date=today, currency=AUD, payment=cash) + recently-used merchant autocomplete (<15s ceiling).
Verify: stopwatch a cash coffee entry.

## Phase 6 — Insights

**Step 17: Stats + export.**
Deliverable: stats endpoints implementing the spec's stats rules (counts_in_stats-gated per Invariants, signed sums, transfer + stored-value exclusions, group by currency — never sum across, AU-FY, Sydney bucketing): monthly trend, category breakdown, filtered trend (receipt category / merchant / item category — item trends aggregate item amounts), tax-FY total + its receipt list, payment-method crosscheck, cash view; CSV export (receipts + items, date range); charts UI (GSAP where it earns it).
You'll be reviewing: the SQL — every stats rule from the spec as a query; this PR is where the transaction model proves itself.
Verify: fixture dataset with known totals; every exclusion rule asserted.

## Phase 7 — Production

**Step 18: Deploy + backups.**
Deliverable: VPS provisioning notes, Caddy TLS on your domain, deploy workflow triggered by git tag or manual dispatch — NOT on every push to main (straight-to-main means ordinary commits must not each auto-deploy), nightly pg_dump + rclone offsite, upload limits at Caddy; security-floor checklist walked.
Verify: the restore drill — nuke a scratch environment, restore from backup, app works.

**Step 19: M1 acceptance.**
Deliverable: golden-set run vs accuracy targets; friction ceilings measured; the real month of use begins.
Verify: the success criteria section of spec.md, line by line.

## Sequencing notes

- Steps 6/7 and 12 are parallelizable in principle, but we go serial — review load stays single-threaded on purpose.
- The golden set accumulates from Step 1 (photograph everything); Step 11's harness runs on whatever exists and re-runs as the set grows.
- After Step 12 you can start *using* the app on LAN — real usage feedback then shapes Steps 13-17 before anything is deployed publicly.

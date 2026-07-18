# Implementation Plan

Companion to `spec.md` (the what/why). This is the how/when. Built in small steps: each step = one PR = one conceptual thing, reviewed and understood before the next begins.

## TODO

Near-term actionable items only — the full step sequence is below.

- [ ] Create GitHub private repo; `git init` + first push (spec.md, plan.md, log.md)
- [ ] Anthropic API key (needed by Step 11, cheap to grab now)
- [ ] Buy/choose domain for the VPS (needed by Step 18)
- [ ] Pick VPS provider (needed by Step 18)
- [ ] Offsite backup target account, B2 or similar (needed by Step 18)
- [ ] Decide email infrastructure for open signup (provider: self-hosted SMTP vs SES/Postmark/Resend; and whether email verification + password reset gate the M1 public launch) — needed by Step 5 if gating, else a fast-follow (spec Open Eng Decisions)
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
Deliverable: dbmate setup; migration 001 with the full spec schema (incl. users with UNIQUE case-insensitive email + email_verified_at + the auth_tokens table for verify/reset; receipts as a magnitude+direction ledger — total_cents BIGINT ≥ 0, direction[inflow|outflow] with a CHECK tying it to kind for the fixed kinds, kind[purchase|refund|reimbursement|income|transfer]; the two-axis receipt state — scan_state + proposed_at/reviewed_at timestamps + reclaim_count, no stored review-status column; categories.is_income; tags + item_tags; recurring kind incl. income + occurrences_generated + occurrences_total + unique (recurring_id, occurrence_ordinal); all ON DELETE SET NULL / CASCADE rules per Invariants); Drogon async DbClient wired via config; health endpoint now pings DB. NOTE: category (expense + income) / payment-method seeds are NOT global rows in the migration — they're per-user template constants applied at registration (Step 5), because tenancy is real multi-user. Specifics locked by the spec reviews: (a) bracketed enums (kind, direction, scan_state, interval_unit, source) are TEXT + CHECK constraints, NOT native PG ENUM types — reversible under dbmate up/down; (b) THREE layered SQL VIEWs in 001: `v_receipt_state` (timestamp-only needs_review + is_confirmed — the high-risk ≥/> logic, single-sourced), `v_countable_spend` on it (is_confirmed AND kind IN (purchase,refund,reimbursement) — an ALLOWLIST, NOT "≠transfer", so income can't leak into spend — AND not a stored-value-paid purchase; exposes signed_spend_cents = total_cents × ±1 by kind), and `v_income` (is_confirmed AND kind=income). Spend stats → v_countable_spend, income stats → v_income, reconciliation (cash view, two-sided payment crosscheck) reuse the BASE v_receipt_state and slice by direction; (c) the match index is (user_id, currency, total_cents, purchase_date) to serve both exact-total dup probes and the imageless ± tolerance-band range scan; (d) finite recurrings use occurrences_total (fixed target), deactivation derived as occurrences_generated ≥ occurrences_total.
You'll be reviewing: the SQL itself (your data model made real) incl. the magnitude+direction money model, the three views, and why the spend view is an explicit kind-allowlist (income safety); and your first coroutine — `co_await execSqlCoro` and what suspending actually means.
Verify: `dbmate up/down` clean; integration test queries through the coroutine path AND through the three views (needs_review/counts_in_stats/income membership matches the Invariants truth table; assert an income row is NOT in v_countable_spend).

## Phase 1 — Auth (first vertical slice)

**Step 5: Register/login/logout + session filter (multi-tenant, open signup).**
Deliverable: OPEN self-service registration (email + password; every signup an equal isolated tenant — no first-user/owner special-casing, no bootstrap token), which in one transaction creates the user AND seeds that user's default categories + "cash" payment method; argon2 via libsodium; sessions table storing only token_hash + httpOnly cookie (~30-day sliding expiry — bump expires_at only when >SESSION_TOUCH_INTERVAL (default 1 day) stale, not on every request); Drogon filter guarding all routes; login AND registration rate-limited. Email verification + password reset (verify/reset tokens in auth_tokens, emailed): scaffold the endpoints + token flow here; the actual send depends on the email-infra decision (Open Eng Decision) — if that decision defers email past M1, gate the send behind config and land verify/reset as a fast-follow, but keep the schema + endpoints in place.
You'll be reviewing: a complete request→coroutine→DB→response slice; RAII around libsodium; how the auth filter composes with handlers; per-tenant isolation as a first-class property (the user_id predicate is load-bearing, not future-proofing).
Verify: curl script walks the whole flow; integration tests for wrong-password/expired-session/duplicate-email-rejected/two-independent-signups-are-isolated (user A cannot read user B's rows → 404) /new-signup-gets-its-own-seeded-categories+cash. (The old "second-registration-rejected" and token tests are GONE — multiple registrations now succeed, isolated.)

Sequencing note (decided 2026-07-18): an early manual-capture "usable slice" between Step 5 and Step 6 was considered (to start daily-use earlier per the daily-use-first priority) and DECLINED — the primary user flow is uploading receipt IMAGES, not typing transactions manually, so an early manual-only tool wouldn't exercise or build the habit that actually matters. First real use is therefore at Step 12 (image capture + review), keeping the auth→storage→scan learning progression unbroken. Manual entry remains fully in scope for cash/no-receipt transactions (Step 9), just not the primary flow and not a reason to ship early.

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
Deliverable: `POST /receipts` (multipart → normalize → hash (scoped user_id, image_sha256) → exact-dup conflict unless force=true → store → row, scan_state=uploaded); `POST /receipts/:id/image` (attach to imageless receipt — same pipeline incl. the exact-dup hash check (409 {duplicate_of} unless force=true, mirroring upload; force stores an independent copy under its own key), updates row instead of creating; sets scan_state=uploaded so the Step 10 worker verify-scans it — until Step 10 lands it just parks there); `GET /receipts` (offset pagination), `GET /receipts/:id`; `DELETE /receipts/:id` (confirmation-gated on the client; inbound receipt FKs ON DELETE SET NULL; always deletes the stored image object — single-owner by construction); image proxy endpoint with immutable cache headers; ownership authorization on every route (non-owned/missing id → 404, per spec API conventions); server accepts JPEG/PNG/WebP only (HEIC rejected at app layer — client converts via the canvas path, with lazy libheif WASM fallback for non-Safari; see spec Upload constraints); 15MB upload limit enforced at the app layer (Caddy enforces its copy in Step 2/18).
You'll be reviewing: multipart handling, streaming a body through a proxy handler, the zero-loss invariant in code.
Verify: curl a real photo up, fetch it back, row is correct.

**Step 9: Manual entry + reference data.**
Deliverable: `POST /receipts/manual` (all FIVE kinds incl. income, magnitude total + direction derived/validated from kind, zero totals allowed, offsets link, born reviewed_at-set / proposed_at NULL → counts immediately); `PATCH /receipts/:id` (edit fields; "mark reviewed" = stamp reviewed_at); items CRUD (`GET /receipts/:id/items` + create/update/delete, incl. per-item tag assignment); categories CRUD incl. the is_income flag (expense vs income categories; picker context by kind); tags CRUD (item-level taxonomy); payment-methods CRUD incl. the stored_value flag. (Expense + income categories + "cash" already exist per-user from the Step 5 registration seed; this adds full CRUD.)
You'll be reviewing: how the transaction-kind + direction rules land in validation code (the CHECK-consistency between kind and direction; income → inflow + income category).
Verify: integration tests — cash coffee, refund with link, transfer, income (salary manual entry, inflow, income category, NOT in v_countable_spend but IN v_income), zero-total warranty swap; invalid kind/direction combos rejected.

## Phase 3 — Scan pipeline (the heart, behind a fake first)

**Step 10: Worker + FakeScanProvider.**
Deliverable: `ScanProvider` interface + fake; event-loop-timer worker: claims uploaded rows via FOR UPDATE SKIP LOCKED, scan_state transitions, TWO-counter accounting (scan_attempts = provider/parse errors → cap 3 = scan_failed; reclaim_count = crash/timeout reclaims → high backstop, re-queues without spending a scan_attempt), 10-min reclaim, 2-scan concurrency cap; TRUST-gated mode discriminator per row (reviewed_at NULL → extraction mode; reviewed_at present → verify mode); scan_failed path + retry endpoint; verify-scan transitions for attached images (fill-empty-only, compare total exact / date ± tolerance / merchant fuzzy; completion stamps proposed_at; all-match → re-stamp reviewed_at so it stays reviewed silently; mismatch → proposed_at leads → needs_review with diff; terminal failure touches neither timestamp so a previously-reviewed row stays reviewed). Review/stats state derived from (proposed_at, reviewed_at) per Invariants, never a status column.
You'll be reviewing: async orchestration — the entire reason you picked all-async Drogon. State machine + crash-safety without burning a cent of API money.
Verify: integration tests incl. kill-the-worker-mid-scan → reclaim works.

**Step 11: AnthropicScanProvider + golden-set harness.**
Deliverable: real Haiku 4.5 call (structured outputs; DOCUMENT-TYPE-aware — classifies receipt | return-docket→refund | payslip→income and sets kind + direction; context-appropriate category enum injected from DB, expense subset for receipts / income subset for payslips via is_income, plus item-level tag enum; payslip path extracts employer + pay date + NET pay as the income total, gross/tax/super as best-effort informational items only — super boundary); config plumbing; a harness script that runs the golden set (receipts AND payslips) and reports per-field accuracy vs targets incl. doc-type classification + net-pay (tags/items/breakdown best-effort, not gated).
You'll be reviewing: the extraction prompt/schema (a spec-level artifact — review it like the spec), and the adapter boundary that makes provider swap trivial.
Verify: harness run on however many golden receipts exist so far.

## Phase 4 — Frontend MVP (first phone-usable moment)

**Step 12: Vite scaffold + auth + upload + history.**
Deliverable: vanilla JS app — login, upload page (camera/file, multi-photo select — one receipt row per image; client downscale before upload: canvas decode+downscale+reencode, with lazy libheif WASM fallback for browsers that can't natively decode HEIC — see spec), exact-dup "are you sure? [shows existing]" confirm on 409, status polling with live trace incl. "⚠ possible match found" deep-link, history list; Vite proxy for dev.
Verify: upload a receipt from your actual phone on LAN and watch it reach pending review. **The app exists now.**

**Step 13: Review UI.**
Deliverable: pending queue (oldest first, count badge; queue = the needs_review derived state — includes scanned payslips + recurring income); review screen (image beside fields, item editing incl. category + tags, sum-mismatch warning, <30s-optimized layout; category picker filters expense-vs-income by kind; income rows show employer/pay-date/net); scan_failed handling (retry scan or fall back to manual field entry against the stored image); attach-image button on imageless receipts (manual/recurring); verify-scan mismatch diff (scanned vs recorded values side by side); mark reviewed; edit-after-review (keeps reviewed status); delete with confirmation.
Verify: you review a real receipt AND a scanned payslip end-to-end; friction-ceiling sanity check.

**Step 14: Dedupe.**
Deliverable: `GET /receipts/:id/matches` (candidates computed at read time, no stored match state; served by the (user_id, currency, total_cents, purchase_date) index; ?suggest= pins a force-included candidate — the attach-conflict dialog's "merge instead" deep-link lands here) + `POST /receipts/:id/merge-from`; match panel in the review screen; candidates = all matching receipts ranked by match strength via ONE tolerance-band rule (spec review fix — same currency; total within ± max(match_total_abs_cents=200, 5%·candidate.total); ±days date; fuzzy merchant), the flavor governs only the offered ACTION: has-image = "possible duplicate" (discard upload / keep both, images side by side), imageless (manual + recurring, pending or reviewed) = "matching transaction" (merge / keep both). The band (not exact-only) lets variable bills AND OCR-variance re-photos surface; exact hits rank top; nothing automatic, suggested action highlighted only; merge reuses the Step 10 verify/fill logic (no re-scan) — image_key + image_sha256 + raw_ocr_json move to the target, items copy only if target has none, inbound receipt FKs (offsets_receipt_id/recurring_id/purchase_receipt_id) repointed source→target, source row deleted (image object retained, ownership transfers) — mismatch resolved inline; discard deletes the upload's row + image object (Step 8 DELETE).
Verify: integration tests + a deliberate double-upload.

## Phase 5 — Recurring + browse

**Step 15: Recurring.**
Deliverable: definitions CRUD (custom interval, finite ends via occurrences_total, kind, defaults, purchase link); scheduler tick creating pending receipts (idempotent via unique (recurring_id, occurrence_ordinal); occurrences_generated advances the ordinal; catch-up generates only the latest missed occurrence but advances occurrences_generated by the full elapsed delta); auto-deactivate when end_date passes OR occurrences_generated ≥ occurrences_total; merge-on-upload path; frontend pages.
Verify: integration tests with clock control; an Afterpay-shaped 4-payment plan exhausts correctly AND still ends on count after a downtime gap that skipped an installment (occurrences_generated jumps, plan deactivates after the right ordinal, not 2 cycles long); a Jan-31 monthly anchor clamps to Feb 28/29 then returns to Mar 31 (no drift); a Feb-29 yearly anchor clamps in non-leap years; a double scheduler tick creates no duplicate; a multi-occurrence downtime gap generates only the latest missed occurrence.

**Step 16: Browse + quick-add polish.**
Deliverable: history filters (status/date/merchant/category/payment/tax-deductible/kind/direction/source), receipt detail with zoomable image, quick-add form with defaults (date=today, currency=AUD, payment=cash) + recently-used merchant autocomplete (<15s ceiling). (Quick-add stays spend-oriented; income is a normal manual entry, not the <15s path.)
Verify: stopwatch a cash coffee entry.

## Phase 6 — Insights

**Step 17: Stats + export.**
Deliverable: stats endpoints implementing the spec's stats rules over the Step 4 layered views (group by currency — never sum across, AU-FY, Sydney bucketing). SPEND stats through `v_countable_spend` (SUM(signed_spend_cents): monthly trend, category breakdown, filtered trend (receipt category / merchant / item category — item trends aggregate item amounts), tax-FY total + its receipt list); INCOME stats through `v_income` (income trend, income-by-category); NET CASHFLOW (income − spend per month); RECONCILIATION stats through the base `v_receipt_state` sliced by direction — TWO-SIDED payment-method crosscheck (debits AND credits) + cash view (transfers vs cash purchases); CSV export (receipts + items, date range); charts UI (GSAP where it earns it).
You'll be reviewing: the SQL — every stats rule as a query over the right view layer; why spend/income/reconciliation pick different layers and why spend is a kind-allowlist (income never leaks in). This PR is where the transaction model proves itself; because membership lives in the views, these endpoints stay thin.
Verify: fixture dataset with known totals INCL. income + transfer rows; assert income excluded from spend, included in income stats, cashflow = income−spend, reconciliation balances both sides; every exclusion rule asserted.

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
- First real use is at Step 12 (image capture + review) — decided 2026-07-18, since the primary flow is uploading receipt images, not manual entry. An early manual-only usable slice was considered and declined (it wouldn't build the habit that matters). Real usage feedback from Step 12 onward shapes Steps 13-17 before public deploy.

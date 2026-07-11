# Work Log

Daily record of work done. Newest day first. Companion to `spec.md` (what/why) and `plan.md` (implementation steps + TODO list). Update every working session.

## Sessions

| Date | Session ID | Agent | Working Directory | Machine |
|------|------------|-------|--------------------|---------|
| 2026-07-11 | 70feb2fc-d1cf-46c1-b4b7-6b8dd8efc22d | Claude | /Users/me/receipt-scanner | motorway1.local |
| 2026-07-09 | cfefe238-3b8b-451c-93fe-f6aab5c18730 | Claude | /Users/me/receipt-scanner | motorway1.local |

A context-compaction continuation gets a NEW session id — the 2026-07-11 row continues the 2026-07-09 session. To resume the latest state, use the newest row.

## 2026-07-11

**Done**
- Set up this work log; fixed session ID to the resumable Claude Code UUID.
- Decided dual-architecture as a hard requirement: arm64 (macOS dev, native, no emulation) + x86_64 (VPS); deploy image built only in CI, never QEMU cross-built on the Mac; Recorded in spec.md; plan.md Steps 1-3 updated.
- Added direct image attach WITH verify-scan: POST /receipts/:id/image for any imageless receipt (manual or recurring) — reuses normalize→store pipeline + existing worker; scan verifies rather than extracts (never overwrites user values, fills empty fields only); reviewed row + all-match → silently back to reviewed, mismatch → pending_review with diff; terminal scan failure on reviewed row restores reviewed. Accepted: brief stats blip while scanning. No schema change. plan.md Steps 8, 10, 13 updated.
- Moved the TODO list from log.md into plan.md (both are how/when; log is now purely the daily record).
- Paper-tested the four capture scenarios end-to-end against the spec. Fixed what it found: REMOVED stale "invoices to add info for the same transaction" from out-of-scope (contradicted the attach/merge decisions); defined merge mechanics (image/sha/ocr move to target, items copy if target empty, source row deleted, image retained) and discard mechanics (row + image object deleted); hash check now also runs on attach; worker mode discriminator written down (source=scanned → extract, manual|recurring → verify); added GET /receipts/:id/matches + merge-from to API draft; added (user_id, total_cents, purchase_date) index for the match query. Accepted without spec change: simultaneous double-tap race and gallery re-encode both fall through hash to the field-match backstop.
- Hashed out dedupe interaction flows: dedupe = a match panel in the new receipt's review screen (no separate flow/screens); nothing executes without a click, suggested action highlighted only; upload page trace shows "possible match found" deep-link when scan lands (field matching can't run at upload — needs scan output). Added exact-duplicate upload check: image_sha256 (normalized image, non-unique index) on receipts; POST /receipts conflicts on hash hit unless force=true — the one upload-time "are you sure?" that's physically possible; saves a scan per double-tap. Record-first attach keeps silent-on-match (user-initiated; mismatch always escalates).
- Widened dedupe-merge (upload-first attach): candidates = ALL matching receipts ranked by match strength, two flavors — has-image = likely duplicate (default discard, side-by-side images), imageless (reviewed manual + recurring pending/confirmed) = attach opportunity (default merge); merge reuses verify-scan fill/compare logic with inline mismatch resolution, no re-scan. Both attach directions now symmetric. plan.md Step 14 updated. (Corrected mid-session: first draft wrongly restricted candidates to imageless only, which would have dropped duplicate detection — the original point of dedupe.)
- Price-component extensibility decided: no GST/tax amount in M1; future columns (base/gst/service charge/card surcharge/holiday surcharge) on receipts/items; requires now: totals stay authoritative, NO DB constraint total=sum(items) (UI warning only); docket-line surcharges = items under fees.
- Specified recurring interval semantics: calendar arithmetic (not fixed day counts); occurrence k = anchor + k×interval (no clamp drift); month-end clamping per Stripe/RFC 5545; added `year` to interval_unit enum; billing wobble absorbed by review/dedupe, not the scheduler.
- VERIFIED libvips on arm64-osx: vcpkg port `libvips[jpeg,png,webp,exif]` 8.18.2 builds clean (glib incl.) and produces WebP (smoke-tested q80 conversion). No Homebrew fallback needed. One-time host prereq discovered: `brew install autoconf autoconf-archive automake libtool` (libexif builds via autotools) — installed on this Mac, must go in Step 1 README. x64-linux side needs no check (covered by vcpkg upstream CI).

## 2026-07-10

**Done**
- Defined success criteria in spec.md: priority = daily-use tool first; capture bar = every transaction; friction ceilings (<15s cash entry, <30s review); golden-set scanner accuracy targets (total ≥98%, date ≥95%, merchant ≥90%, category ≥80%); engineering/ops criteria (restart-safe, one-command deploy, tested restore, <$15/mo); all four C++ learning outcomes (async/coroutines, ownership/RAII, build & tooling, testing) subordinate to shipping.
- Closed the transaction model: four kinds (purchase | refund | reimbursement | transfer) with signed-sum stats rules; stored-value convention (top-ups count, redemptions excluded, per-method flag); cash convention (ATM = transfer, spends count); reimbursements as negative entries; bonds = purchase + linked refund; income out of scope M1 but kind enum reserved for it; recurring gained end conditions (end_date/remaining_count), installment-plan purchase link, and its own kind for auto-savings/autopay.
- Decided image storage: self-hosted MinIO (S3-compatible, private Docker network); hand-rolled SigV4 client over Drogon async HttpClient; cloud-portable via config (endpoint/region/creds/addressing style); server-side normalization to 2048px WebP q80 with EXIF/GPS stripped (libvips, on worker thread pool); backend-proxy serving with immutable cache headers; 302-to-presigned documented as the CDN upgrade path.
- Locked remaining pre-implementation decisions: GitHub private + Actions CI (ASan/UBSan), Catch2, dbmate, C++20/clang; API conventions (/api/v1, snake_case, UTC timestamps, Australia/Sydney date bucketing); worker defaults (5s poll, 3 attempts, 10-min reclaim, 2 concurrent); upload constraints (15MB, HEIC client-converted); category seed list; nightly pg_dump + rclone backups.
- Wrote plan.md: 19 steps, 7 phases, one PR per step, review-gated; phone-usable app at Step 12.

**Decided** (all recorded in spec.md with reasoning)

## 2026-07-09

**Done**
- Initial spec critique: caught libpqxx/cpr vs Drogon event-loop conflict, money-as-JSON-doubles trap, recurring-model redundancy, missing stuck-scan recovery, HEIC gap.
- Architecture decisions: single all-async Drogon process (Drogon DbClient + HttpClient, coroutines); integer cents end-to-end; receipts-created-upfront invariant (only reviewed rows count in stats); recurring_charges table dropped; session-cookie auth + argon2/libsodium; scan provider = Claude Haiku 4.5 with structured outputs behind swappable ScanProvider interface (hard requirement: trivial provider/model swap via config); frontend = vanilla JS + Vite + GSAP; hosting = public VPS (Caddy TLS, Docker Compose); currency column added.
- Fleshed out full M1 feature set & user stories in spec.md: auth (first-user-only registration), capture (upload + status trace + manual + quick-add), review (optional line items, dedupe merge), recurring (custom interval), browse, insights; line items extracted + auto-categorized but optional to review; one user-editable category taxonomy embedded as enum in scanner schema; charts incl. filtered trend by category/merchant/item-type; AU FY (Jul-Jun); CSV export.

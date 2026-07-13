# The Project: A personal receipt scanning web app

This project is a receipt scanning app for personal use.

# Project ground rules

We will design it exactly to suit my own personal requirements of a receipt scanning app while also adhering to scalability best practices without the cost
of actually scaling beyond one user account. No cross-account interactions, every account has fully isolated
access.

# Features & User Stories (M1)

Single user ("I") throughout. Stories marked (open) depend on decisions still being discussed.

## Auth & accounts
- I can register the first account (email + password). Registration is only open while the users table is empty (public VPS — no open signup); further accounts via invite flow (post-M1).
- I can log in and stay logged in on my phone and laptop (session cookie, ~30-day sliding expiry) and log out.
- Login endpoint is rate-limited.

## Capture
- I can upload one or more receipt photos from my phone camera or a file picker. Client re-encodes/downscales before upload (handles HEIC, cuts vision-token cost).
- Each image immediately creates a receipt row (status=uploaded) — nothing is ever lost in a waiting state. I see it appear in history with a live status trace: uploaded → scanning → pending review (or scan failed).
- On scan failure I can retry the scan or fall back to entering the fields manually against the stored image.
- I can record a cash/no-receipt transaction via a manual entry form (merchant, date, total, currency, payment method defaulting to cash, category, tax-deductible, note, optional items). Manual entries are born status=reviewed — I typed them, no review needed.
- Quick-add (required by the <15s friction ceiling): the manual form defaults everything defaultable (date=today, currency=AUD, payment=cash) so a coffee is amount + merchant + category and done; recently-used merchants autocomplete.
- I can record a refund/return: scan the return docket (they're receipts) or manual entry; negative total, optional link to the original receipt.
- I can record a reimbursement I receive (PayID/Beem etc.): a negative manual entry. These usually arrive with no purchase context — I annotate which expense they offset (same category as the original so category charts net correctly; optional link).
- I can record an ATM withdrawal as a transfer — visible for cash reconciliation, never counted as spend (the individual cash purchases are the spend).

## Transaction kinds & stats rules (decided)
- receipts.kind: purchase (default) | refund | reimbursement | transfer. The enum is designed to extend: `income` is reserved for a post-M1 extension (would be recorded + excluded from spend stats, like transfer). M1 records no incoming money except reimbursements; the reconciliation success criterion applies to the statement's DEBIT side only.
- Transfer covers all own-money movement, none of it spend: ATM withdrawals, credit-card bill payments (the card's purchases were already counted), moves to savings/investments, and loan/BNPL principal repayments (the interest/fee portion is a real purchase, category "fees").
- Expenses incurred for others still count in full as my expense; incoming reimbursements offset them naturally as negative entries.
- Refundable deposits/bonds (rental bond, hire deposits): purchase (category "bonds/deposits") + linked refund when returned. If the bond is docked, the books are already correct.
- Stored-value convention: gift card/Opal/prepaid TOP-UPS count as the expense at top-up time (kind=purchase, categorized e.g. "transport", "gifts"). Purchases PAID WITH a stored-value method are recorded (receipt, items, history, dedupe) but excluded from spend stats — the money was already counted. Requires payment methods to carry a stored_value flag.
- Cash is the opposite: ATM withdrawal (kind=transfer) excluded; each cash purchase counts at spend time.
- The stored-value convention is chosen PER payment method via the stored_value flag: flag an Opal/gift card (spend books at top-up), leave a travel money card (Wise) unflagged and record its loads as transfers to keep per-purchase categories abroad.
- Spend stats = signed sum of purchases + refunds + reimbursements, excluding kind=transfer and excluding purchases paid with stored-value methods.
- Small conventions: pre-auth holds ($1 petrol, hotel holds) are not transactions — reconcile settled lines only; cashback/reward credits = refund with category "rewards" (or ignore); bank fees/interest/fines/ATO = ordinary manual purchases; zero-total receipts (warranty swaps) are allowed.

## Review
- I have a "pending review" queue (oldest first, with a count badge) of everything machine-generated: scanned receipts and auto-created recurring charges.
- The review screen shows the receipt image alongside the extracted fields; I can correct merchant, date, total, currency, payment method, category, tax-deductible, note, and line items, then mark reviewed.
- Line items are extracted and auto-categorized by the scanner but OPTIONAL to review (decided): marking reviewed only requires confirming receipt-level fields — a 40-item grocery shop is not a chore. Item-level data is best-effort: as good as the auto-labels plus whatever I choose to correct.
- If line items don't sum to the total, I get a warning, not a block (discounts/rounding are real).
- Categorization (decided): one user-editable category taxonomy (seeded with sensible defaults), used at both receipt level ("groceries") and item level ("junk food"). The scanner's structured-output JSON schema embeds the current taxonomy as an enum, so the LLM assigns categories from MY list at scan time; review corrects. New category = edit the list; next scans pick it up automatically.
- Dedupe (decided flow): dedupe is not a separate flow — it's a match panel inside the new receipt's review screen, the screen every scanned receipt already passes through. Field matching (total → ±days date on the recorded transaction date → fuzzy merchant) runs post-scan; candidates are ALL matching receipts ranked by match strength, in two labeled flavors: WITH image = "possible duplicate" (both images side by side; actions: discard this upload / keep both) and imageless (pending/confirmed recurring, reviewed manual entries) = "matching transaction" (actions: merge into it / keep both). NOTHING executes without an explicit click — the suggested action is only visually highlighted. Merge uses verify-scan semantics (fill empty fields only, never overwrite; the upload's existing scan is reused, no re-scan), mismatches resolved inline in the panel. Merge mechanics (decided): image_key + image_sha256 + raw_ocr_json move to the target, scanned items copy over only if the target has none, then the source row is deleted (the image object is retained — ownership transfers). Discarding a duplicate deletes both the row and its stored image object. The upload page's live status trace shows "⚠ possible match found" when the scan lands, deep-linking to review — near-immediate feedback without blocking upload (field matching physically can't run at upload time; it needs scan output).
- Exact-duplicate upload check (decided): receipts store image_sha256 (of the normalized image; plain index, not unique). POST /receipts computes it after normalization and, on a hash hit, returns a conflict with the existing receipt ("are you sure? [shows existing]") WITHOUT creating a row; client re-submits with force=true to proceed. The only dedupe check that can run at upload time; also saves a wasted scan per double-tap. Cross-libvips-version hash drift just degrades to the field-match backstop.
- I can re-open and edit a reviewed receipt later without it losing reviewed status, and delete any receipt (with confirmation).

## Recurring
- I can create/edit/pause/delete recurring transaction definitions: name, merchant, expected amount + currency, cadence = custom interval (decided: every N days/weeks/months/years, anchored to a start date — covers weekly through annual and odd billing cycles; calendar arithmetic with month-end clamping, see Recurring interval semantics), default payment method/category/tax-deductible.
- Recurring definitions can be finite (decided): optional end_date or remaining_count; auto-deactivates when exhausted. Installment plans (Afterpay/Zip: 4 fortnightly payments) are finite recurrings with an optional link to the originating purchase receipt.
- Recurring definitions carry a kind [purchase|transfer] (decided): auto-savings and credit-card autopay are recurring transfers — generated receipts inherit the kind and stay out of spend stats.
- Dedupe date-matching is tolerant (± a few days) for bills whose billing date drifts (utilities).
- On the expected date a pending receipt is auto-created. Confirming = reviewing it (adjusting the actual amount if it differs); if the charge didn't happen I delete the pending receipt; if I cancelled the subscription I pause/delete the definition.
- If I upload the real invoice for a charge that already has a pending recurring receipt, dedupe offers the merge.
- Direct attach with verify-scan (decided): from any imageless receipt (source=manual or recurring, image_key NULL) I can attach an image — POST /receipts/:id/image, same normalize→store pipeline as upload, sets image_key and status=uploaded; the exact-dup hash check runs here too, mirroring upload semantics (decided): on a hash hit the attach is rejected with a conflict {duplicate_of: S} before anything is stored; the client dialog shows S and offers (a) re-submit force=true to attach anyway (stores an independent second copy under its own image_key — the sha index is non-unique precisely to permit this) or (b) merge instead — deep-link to S's review screen with the match panel open and this receipt force-included as a pinned candidate (?suggest= on the matches endpoint; field matching alone might not rank it if the entered fields are off). The merge is the standard merge-from with the imageless receipt as TARGET (its user-entered fields are the trusted ones) and S as SOURCE (donates image/ocr/items, then deleted — also un-double-counts stats if both were reviewed). Direction is forced: the imageless row would donate nothing as a source. The existing worker scans it (kind-aware, using the row's known kind). Worker mode discriminator (decided): source=scanned → extraction mode (scan writes structured fields); source=manual|recurring → verify mode (fill-empty-only + compare, below). Scan is verification, not extraction: it NEVER overwrites user-entered values — it fills only empty fields (items, store_name_raw, raw_ocr_json), then compares scanned vs recorded fields (total exact; date ± dedupe tolerance; merchant fuzzy). On a previously-reviewed row (reviewed_at non-null): all match → back to reviewed silently, no queue trip; any mismatch → pending_review with the diff shown at review. On a still-pending row it flows to pending_review as normal, with scan prefills. Terminal scan failure on a previously-reviewed row restores reviewed (attach degrades to evidence-only) — a confirmed transaction must never land in scan_failed over an attach glitch. Accepted consequence: during the brief uploaded→scanning window a reviewed receipt drops out of stats (seconds; single-user). Upload-first dedupe→merge remains available.

## Browse
- "Upload history": all receipts, chronological, filterable by status, date range, merchant, category, payment method, tax-deductible, source (scanned/manual/recurring).
- Receipt detail view with zoomable image.

## Insights (all decided; reviewed receipts only, never summed across currencies)
- Monthly spend trend: total reviewed spend per month.
- Category breakdown: spend by category for a selected month/range.
- Filtered spend trend: the monthly trend narrowed by dimension selections — receipt category, merchant, and/or item category (e.g. "junk food over time"). Item-category trends aggregate item amounts, not receipt totals, and are best-effort (auto-labeled).
- Tax-deductible running total for the current Australian FY (Jul 1 – Jun 30, decided), with its receipt list. All "this year" filters use AU FY.
- Payment-method crosscheck: monthly totals per payment method to reconcile against bank statements.
- Cash view: cash withdrawn (transfers) vs cash spent per month — the reconciliation story for statement ATM lines.
- All stats follow the Transaction kinds & stats rules (signed sums, transfer + stored-value-payment exclusions).
- FX note: foreign-currency receipts never exactly match the AUD card charge (rate + fee); reconciliation for those is approximate by design.
- CSV export of receipts + items for a date range, for tax time.

## Out of scope for M1
- split payments
- multi-user invite flow
- email-in receipt ingestion, PWA/offline

# Architecture & Stack (decided)

- Backend: C++ (chosen deliberately as a learning vehicle for the language; not the "natural" fit for a CRUD-heavy app, but acceptable given low scale/low risk)
    - Architecture: single all-async Drogon process, coroutines throughout (decided — maximizes C++ async learning; no separate worker binary)
    - Framework: Drogon (batteries-included: routing, JSON, async/coroutines, ORM-lite) to avoid hand-building HTTP plumbing
    - DB access: Drogon's async DbClient with coroutines (`co_await execSqlCoro`). libpqxx rejected: blocking, would stall the event loop
    - JSON: nlohmann/json
    - Outbound HTTP (to scan provider): Drogon's async HttpClient (`sendRequestCoro`). cpr/Boost.Beast rejected: blocking / too low-level
    - Known tradeoffs to budget for: async orchestration of the scan pipeline (upload -> background job -> status transitions) is more manual than in Python/Node; less JSON/schema-validation/ORM convenience; slower iteration (compile times, verbose error handling); no native LLM/OCR SDKs so provider calls are hand-rolled REST+JSON.
- Money: integer cents end-to-end (decided) — BIGINT in Postgres, int64 in C++, plain integers in JSON. Avoids float drift through nlohmann/json doubles; makes dedupe total-matching exact. Frontend formats for display.
- Database: Postgres
- Object storage (decided): self-hosted MinIO (S3-compatible), running in Docker Compose on the private network, never publicly exposed
    - Access from C++: hand-rolled SigV4 client over Drogon's async HttpClient (PutObject/GetObject/DeleteObject only; ~150 lines of OpenSSL HMAC). AWS SDK rejected: huge build tax, own thread model, 99% unused. minio-cpp rejected: blocking.
    - Cloud-portability requirement (decided): the S3 client takes endpoint, region, credentials, and addressing style (path-style for MinIO, virtual-hosted for AWS) from config — swapping MinIO for AWS S3/R2/B2 later is a config change plus `rclone sync`/`mc mirror` of the objects. ImageStore interface above it keeps even non-S3 backends possible.
    - Image normalization (decided): server-side canonical version regardless of what the client uploads — decode → apply EXIF orientation → STRIP all EXIF (phone photos embed GPS) → downscale to 2048px long edge → encode WebP q80 (~150-350KB/receipt, ~0.5GB/yr; crisp above Haiku's 1568px vision cap, headroom for future models). Original discarded. Library: libvips.
    - CPU-bound rule: normalization runs on a small worker thread pool, resuming the coroutine after — never on a Drogon IO loop.
    - Serving (decided): backend proxy — GET /receipts/:id/image streams from MinIO through Drogon, guarded by the session cookie. Presigned URLs rejected for now: would force MinIO onto the public internet and re-tie the frontend to whichever storage host is in use, breaking swap-invisibility.
    - Images are immutable after normalization → proxy responds with `Cache-Control: private, max-age=31536000, immutable`; the browser cache is the only cache that matters at one user.
    - CDN note / upgrade path: proxying does forgo CDN caching of a future cloud bucket — irrelevant here (private per-user content, view-once access pattern, audience of one). If that ever changes, the documented upgrade is the same endpoint 302-redirecting to a short-lived presigned URL: auth stays in the backend, frontend unchanged, CDN/edge takes over delivery. ~20-line change inside the same handler.
- Scan pipeline: async, decoupled from upload request
    - Core invariant (decided): a receipt row is created upfront the moment work exists (upload, manual entry, recurring charge date) so no intermediate "waiting" state can lose work. All non-manual receipts require human review before counting toward stats/analytics — stats queries filter status=reviewed only.
    - POST /receipts saves image + creates receipt row (status=uploaded), returns immediately
    - Background worker: in-process event-loop timer (`getLoop()->runEvery`) polling the status column (no Redis/BullMQ at personal scale). Sets status=scanning, calls scan service. Cap concurrent scans (~2-3).
    - Stuck-scan recovery (required, since in-flight scans die on process restart): receipts get scan_attempts + scan_started_at; worker reclaims rows stuck in status=scanning past a timeout, up to a max attempt count, then marks scan_failed.
    - Atomic job claim: worker claims jobs via `UPDATE ... WHERE id IN (SELECT ... WHERE status='uploaded' FOR UPDATE SKIP LOCKED) RETURNING`, so the process stays stateless (DB is the queue's source of truth) and a second instance could never double-claim. Statelessness invariant: all in-process state (in-flight coroutines, timers, counters) is disposable and reconstructible from Postgres + MinIO.
    - Scan service (decided): multimodal LLM extraction, launch provider = Claude Haiku 4.5 (`claude-haiku-4-5`) via the Claude API with structured outputs (JSON-schema-constrained response) — ~0.5c/receipt, guaranteed-parseable JSON.
    - HARD REQUIREMENT — provider swappability: the scanner controller must make it trivially easy to swap any model from any LLM provider. Design: abstract `ScanProvider` interface (`scanReceipt(imageBytes/imageKey) -> ScanResult`), one adapter class per provider (Anthropic first; OpenAI/local/etc. later). Each adapter owns its own request shape, prompt, schema enforcement, and response parsing; the rest of the app only ever sees the normalized ScanResult struct. Provider + model + endpoint + API key come from config, not code, so a model swap within a provider is a config change and a new provider is one new adapter class.
    - On success: status=pending_review, structured fields + items written
    - On failure: status=scan_failed, user can retry or enter manually
    - Frontend polls for status updates (simplest option at this scale; SSE/websocket possible later)
- Recurring transactions (decided): when a recurring charge's expected date arrives, the worker immediately creates a receipt row (status=pending_review, source=recurring, recurring_id FK). The receipt itself is the confirmation-queue entry: confirm = mark reviewed, cancelled/paused = delete. No separate recurring_charges table. Dedupe should offer to merge an uploaded receipt image into a matching pending recurring receipt rather than flag a duplicate.
- Recurring interval semantics (decided): month/year units use calendar arithmetic, not fixed day counts — monthly = (1, month), quarterly = (3, month), yearly = (1, year). Occurrence k is always computed from the anchor (anchor + k×interval), never from the previous occurrence, so clamping can't drift. Month-end clamping: if the anchor's day-of-month doesn't exist in a target month, clamp to that month's last day (Jan 31 → Feb 28/29 → Mar 31; Feb 29 anchor → Feb 28 in non-leap years) — matches Stripe/RFC 5545 behavior. All dates in Australia/Sydney. Real-world billing wobble (±days) is handled at review/dedupe-merge, not by the scheduler.
- Auth (decided): session cookie — random token in a sessions table, httpOnly + Secure + SameSite=Lax, SPA served same-origin as API. Password hashing via libsodium argon2 (crypto_pwhash). JWT rejected: extra crypto machinery, awkward revocation, no benefit at this scale. First-party cookies are not affected by browser third-party-cookie blocking. Modeled as multi-user (users table, receipts scoped by user_id) even though single-user today, to future-proof for household/shared use
- Frontend (decided): vanilla JS + Vite + GSAP. Pure REST/JSON client, no server-side coupling to backend. No framework reactivity, so DOM updates for the status-polling UI are hand-rolled — keep a small render helper per view; a later framework rewrite stays possible since the API contract is the only coupling.
- Deployment (decided): public VPS. Consequences: auth is load-bearing, not defense-in-depth — Drogon behind a reverse proxy (Caddy recommended: automatic Let's Encrypt TLS), rate-limit the login endpoint, argon2 password hashing (already decided), enforce upload size limits at the proxy AND app layer, Docker Compose (app + Postgres + MinIO + Caddy) with vcpkg multi-stage build; MinIO not exposed through Caddy.
- API conventions (defaults, veto anytime): /api/v1 prefix; snake_case JSON; ISO-8601 UTC timestamps; error envelope {error: {code, message}}; offset pagination. All date bucketing (months, "today", FY) in Australia/Sydney, not UTC.
- Ownership authorization (decided): every resource route — reads, writes, and especially DELETE/merge-from — checks the row belongs to the session user; non-owned or missing ids return 404 (not 403, no existence leak). Single-user in practice, but the check is load-bearing on a public VPS and costs nothing (every query already carries the user_id predicate).
- Worker defaults: poll 5s; max 3 scan attempts; reclaim status=scanning after 10 min; 2 concurrent scans.
- Upload constraints: 15MB max (enforced at Caddy AND app); accept JPEG/PNG/WebP/HEIC; client converts HEIC + downscales before upload; server normalizes regardless (see Object storage).
- Category seed list (editable in-app): groceries, dining/takeaway, transport, fuel, household, utilities, subscriptions, insurance, health, clothing, entertainment, gifts, fees, bonds/deposits, rewards, work-expenses, junk food, other.
- Payment-method seed: "cash" (stored_value=false) only — required so quick-add's payment=cash default resolves on a fresh install; all other methods are user-created in-app.
- Backups: nightly pg_dump + rclone of dump and MinIO data to offsite target (target chosen at VPS setup; B2 default); restore drill per success criteria.
- Dev environment: macOS (clang) for development, Linux (Docker) for deploy; Compose file doubles as dev env (Postgres + MinIO); Vite dev server proxies /api to Drogon; Linux CI catches platform drift.
- Dual-architecture (decided, hard requirement): everything must work on arm64 (macOS local dev) AND x86_64 (Linux VPS). Consequences: all deps must be available on both (Postgres/MinIO/Caddy images are multi-arch; vcpkg deps must build on arm64-osx and x64-linux — libvips[jpeg,png,webp,exif] 8.18.2 VERIFIED building + producing WebP on arm64-osx 2026-07-11; host prerequisite: `brew install autoconf autoconf-archive automake libtool`); local dev runs the app natively on macOS against arm64 containers (no emulation); the x86_64 deploy image is built ONLY in CI (Step 3) — never on the Mac, where QEMU cross-building a C++ image would take hours; no arch-specific code (intrinsics, asm) without a portable fallback.
- API shape (draft):
    - POST /receipts (upload image, kicks off async scan; 409-style conflict {duplicate_of} on exact image-hash hit unless force=true)
    - GET /receipts?status=pending_review|reviewed|...
    - PATCH /receipts/:id (edit fields, mark reviewed)
    - POST /receipts/manual (cash transactions, no image)
    - POST /receipts/:id/image (attach image to an imageless receipt; triggers verify-scan; 409-style conflict {duplicate_of} on exact image-hash hit unless force=true, mirroring POST /receipts)
    - GET /receipts/:id/items, item CRUD
    - POST /recurring, GET /recurring/upcoming (confirmation queue)
    - categories CRUD (user-editable taxonomy)
    - payment methods CRUD (user-managed list with stored_value flag)
    - GET /stats/monthly, GET /stats/trend?category=|merchant=|item_category= (filtered trend), GET /stats/tax?fy=2026 (AU FY)
    - GET /export.csv?from=&to= (receipts + items)
    - GET /receipts/:id/matches (candidates for the review-screen match panel; computed at read time, no stored match state; ?suggest=<id> force-includes a pinned candidate — used by the attach-conflict deep-link)
    - POST /receipts/:id/merge-from (apply merge mechanics from a source receipt), DELETE /receipts/:id (discard; always deletes the receipt's image object — objects are single-owner by construction: merge transfers ownership, force-attach stores an independent copy)
- Schema (draft):
    - users(id, email, password_hash, created_at)
    - sessions(id, user_id, token_hash, expires_at, created_at)
    - categories(id, user_id, name) — one shared user-editable taxonomy for both receipt- and item-level; embedded as enum in scanner output schema
    - payment_methods(id, user_id, name, stored_value BOOL) — user-managed list ("Amex", "Visa …1234", "cash", "Opal", "gift card"); stored_value=true methods are excluded from spend stats at payment time
    - receipts(id, user_id, image_key, image_sha256 nullable (normalized image; indexed, non-unique — force=true permits genuine re-uploads), store_name_raw, merchant, purchase_date, total_cents BIGINT (negative for refunds/reimbursements), currency CHAR(3) DEFAULT 'AUD', kind[purchase|refund|reimbursement|transfer] (extensible — 'income' reserved post-M1), status[uploaded|scanning|pending_review|reviewed|scan_failed], scan_attempts, scan_started_at, reviewed_at, payment_method_id FK nullable, tax_deductible, category_id FK nullable, note, source[scanned|manual|recurring], recurring_id FK nullable, offsets_receipt_id FK nullable (refund/reimbursement → original expense), raw_ocr_json, created_at, updated_at)
      - stats/charts group by (or filter to) currency — never sum across currencies
      - index (user_id, total_cents, purchase_date) — serves the match-panel candidate query across full history
      - store_name_raw = what the scan extracted ("WOOLWORTHS 1234 CHATSWOOD"); merchant = normalized name for dedupe/charts, editable at review
    - items(id, receipt_id, name, quantity, unit_price_cents, amount_cents, category_id FK nullable)
    - Price-component extensibility (decided): no GST/tax amount stored in M1, but amounts must be splittable later into components (base + gst + service charge + card surcharge + holiday surcharge, …) as added columns on receipts/items. What this requires NOW: total_cents/amount_cents stay the single authoritative charged amounts (future component columns reconcile to them); NO DB constraint tying receipt total to sum(items) — the sum-mismatch check is a review-UI warning only. Docket-line surcharges are representable today as items categorized under fees.
    - recurring_transactions(id, user_id, name, merchant, kind[purchase|transfer], expected_amount_cents, currency, interval_n, interval_unit[day|week|month|year], anchor_date, next_expected_date (always derived as anchor + k×interval — see interval semantics), end_date nullable, remaining_count nullable, purchase_receipt_id FK nullable (installment plans → what was bought), default_payment_method_id, default_category_id, default_tax_deductible, active)
      - auto-deactivates when end_date passes or remaining_count hits 0
    - (recurring_charges table removed — the auto-created pending receipt is the confirmation-queue entry)

# Success Criteria

Priority (decided): **daily-use tool first**. Success = still using it in 6 months. When instructive-but-slow conflicts with shipping, ship — C++ learning is pursued opportunistically within that constraint, never as a reason to stall a feature.

## Product — the app is genuinely used
- Capture bar (decided): EVERY transaction — receipts, cash, cards, subscriptions. The app is the complete spending record; bank reconciliation should fully balance, not approximately.
- Habit: after M1 ships, transactions are captured within 3 days of purchase, sustained for 3 consecutive months without reverting to spreadsheets/nothing.
- Friction ceilings (these make the every-transaction bar survivable): recording a cash transaction <15s (implies a quick-add form with smart defaults, not a full page of fields); typical scanned receipt reviewed in <30s; pending-review queue hits zero at least weekly.
- Zero-loss invariant holds in practice: no receipt ever disappears into a waiting state; every upload is accounted for as reviewed, pending, or failed-with-image.
- Trust: monthly payment-method totals reconcile against bank statements fully on the debit side (income lines out of scope in M1); discrepancies explainable in minutes, not hours.
- Tax time test: at FY end, producing the deductible receipt list + CSV export takes <5 minutes.

## Scanner quality (measured against the golden set)
- Golden set: 20-30 real receipt photos (varied: faded thermal, crumpled, long dockets, glare) with hand-verified expected values, kept as fixtures. Re-run on every prompt/model/provider change — this is the regression harness that makes provider swapping safe, not just easy.
- Field accuracy targets (proposed): total ≥98%, purchase date ≥95%, merchant ≥90%, receipt category ≥80%. Items best-effort, no gate.
- Hard scan failures (scan_failed after retries) <5% of uploads.

## Engineering & ops
- Restart-safe: killing the process at any moment loses no data; stuck scans self-recover via the reclaim rule (verified by test).
- Deploy is one command (docker compose up on the VPS); a fresh-machine rebuild from repo works, proving the vcpkg/CMake setup is reproducible.
- Backups: Postgres + images backed up automatically; a restore drill has actually been performed once (an untested backup doesn't count).
- Security floor on the public VPS: TLS via Caddy, rate-limited login, argon2 hashing, registration closed after first user, no secrets in repo.
- Running cost: (proposed) <$15/month all-in (VPS + LLM; LLM share expected <$1).

## C++ learning outcomes (decided: all four, subordinate to shipping)
- Async & coroutines: can add a new endpoint end-to-end (route → coroutine handler → async SQL → JSON response) without copying from references; can explain why blocking the event loop is wrong and demonstrate the failure mode.
- Ownership & RAII: no raw owning pointers in the codebase; can justify each lifetime/ownership decision (value vs unique_ptr vs shared_ptr) rather than defaulting to shared_ptr everywhere.
- Build & tooling: owns the CMake + vcpkg setup rather than cargo-culting it; dev builds run with warnings-as-errors and ASan/UBSan; fresh-machine rebuild from repo works.
- Testing discipline: unit tests (Catch2 or GoogleTest) + integration tests against Postgres are a habit, not an afterthought; FakeScanProvider exercises the review pipeline without LLM calls; tests run before every deploy.

## M1 definition of done
- All M1 user stories implemented and exercised with real data: one full month of real receipts/transactions processed through the system.
- Golden-set accuracy targets met; restore drill passed; a full FY-style CSV export produced.

# Tooling (decided)

- Repo: GitHub private; CI: GitHub Actions — Linux build + Catch2 tests + ASan/UBSan on every push; tests-before-deploy gate
- Tests: Catch2 (unit) + integration tests against real Postgres/MinIO in Compose; FakeScanProvider for pipeline tests; golden set runs against normalized (2048px WebP) images
- Migrations: dbmate — plain up/down SQL files
- Language/toolchain: C++20 (coroutines require it), clang locally (macOS), gcc or clang in Linux CI/Docker; clang-format + clang-tidy from day one

# Pre-implementation checklist (user-supplied prerequisites)

- [ ] Buy/choose a domain (Caddy needs it for TLS)
- [ ] Pick a VPS provider + provision
- [ ] Anthropic API key
- [ ] GitHub private repo created; git init locally
- [ ] Start collecting golden-set receipts NOW (20-30 varied real photos: faded thermal, crumpled, long dockets, glare) — this takes weeks of normal shopping, so it should run in parallel with development
- [ ] Offsite backup target account (B2 or similar) — needed by first deploy, not first commit

# Open Eng Decisions (TODO)

- Prompt/schema design for structured extraction (provider + model decided: Claude Haiku 4.5, structured outputs; schema embeds the user's category taxonomy as an enum; must be kind-aware — return dockets scan as refunds)
- Multi-account invite/creation flow (post-M1; auth mechanism decided: session cookie)
- Image retention/cleanup policy (probably: keep forever — 0.5GB/yr is nothing)

# Implementation Plan

See `plan.md` — 19 steps across 7 phases, one PR per step, review-gated. spec.md stays the what/why; plan.md tracks the how/when + the running TODO list; `log.md` tracks daily work done.

-- migrate:up

-- citext: case-insensitive UNIQUE email (spec Auth). pg_trgm: fuzzy merchant matching for
-- dedupe (spec Dedupe, decided 3a) -- the first MerchantMatcher implementation.
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_trgm;

-- Every bracketed enum in this schema (kind, direction, scan_state, interval_unit, source,
-- purpose) is TEXT + CHECK, never a native PG ENUM type: ALTER TYPE ... ADD VALUE can't run
-- inside a transaction on older PG and is irreversible -- hostile to dbmate's plain up/down
-- files. A CHECK constraint is dropped/recreated in an ordinary reversible migration.

CREATE TABLE users (
  id BIGSERIAL PRIMARY KEY,
  email CITEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  email_verified_at TIMESTAMPTZ,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE sessions (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash TEXT NOT NULL,
  expires_at TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_sessions_user_id ON sessions(user_id);
CREATE UNIQUE INDEX idx_sessions_token_hash ON sessions(token_hash);

-- verify/reset tokens (spec Auth: email_verified_at + auth_tokens table)
CREATE TABLE auth_tokens (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash TEXT NOT NULL,
  purpose TEXT NOT NULL CHECK (purpose IN ('verify', 'reset')),
  expires_at TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX idx_auth_tokens_token_hash ON auth_tokens(token_hash);
CREATE INDEX idx_auth_tokens_user_id ON auth_tokens(user_id);

-- Categorization is ITEM-LEVEL ONLY (decided 2a) -- no category_id on receipts. is_income
-- splits one taxonomy into expense vs income categories; picker/scanner filter by context.
CREATE TABLE categories (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  is_income BOOLEAN NOT NULL DEFAULT false
);
CREATE INDEX idx_categories_user_id ON categories(user_id);

-- Second taxonomy, item-level only, many-per-item (item_tags below).
CREATE TABLE tags (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name TEXT NOT NULL
);
CREATE INDEX idx_tags_user_id ON tags(user_id);

CREATE TABLE payment_methods (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  stored_value BOOLEAN NOT NULL DEFAULT false
);
CREATE INDEX idx_payment_methods_user_id ON payment_methods(user_id);

-- recurring_transactions.purchase_receipt_id -> receipts is added via ALTER after receipts
-- exists (circular reference: receipts.recurring_id -> recurring_transactions).
CREATE TABLE recurring_transactions (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  merchant TEXT,
  kind TEXT NOT NULL CHECK (kind IN ('purchase', 'transfer', 'income')),
  direction TEXT NOT NULL CHECK (direction IN ('inflow', 'outflow')),
  expected_amount_cents BIGINT NOT NULL CHECK (expected_amount_cents >= 0),
  currency CHAR(3) NOT NULL DEFAULT 'AUD',
  interval_n INTEGER NOT NULL CHECK (interval_n > 0),
  interval_unit TEXT NOT NULL CHECK (interval_unit IN ('day', 'week', 'month', 'year')),
  anchor_date DATE NOT NULL,
  -- Single mutable progress counter (spec Recurring interval semantics) -- next date =
  -- anchor + (occurrences_generated+1)*interval, clamped. Not re-derived from a clamped
  -- date, which isn't cleanly invertible once month-end clamping applies.
  occurrences_generated INTEGER NOT NULL DEFAULT 0,
  next_expected_date DATE NOT NULL,
  end_date DATE,
  -- Fixed target, NOT a decrementing counter (spec Invariants) -- avoids two counters
  -- drifting against each other during catch-up.
  occurrences_total INTEGER,
  purchase_receipt_id BIGINT,
  default_payment_method_id BIGINT REFERENCES payment_methods(id) ON DELETE SET NULL,
  default_category_id BIGINT REFERENCES categories(id) ON DELETE SET NULL,
  default_tax_deductible BOOLEAN NOT NULL DEFAULT false,
  active BOOLEAN NOT NULL DEFAULT true,
  CONSTRAINT chk_recurring_direction_by_kind CHECK (
    (kind = 'income' AND direction = 'inflow') OR (kind IN ('purchase', 'transfer'))
  )
);
CREATE INDEX idx_recurring_user_id ON recurring_transactions(user_id);

-- The general TRANSACTIONS ledger (receipts, cash, income, transfers) -- "receipts" is a
-- legacy name; an income/interest/salary row is a normal zero-image row or a scanned payslip.
CREATE TABLE receipts (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  store_name_raw TEXT,
  merchant TEXT,
  purchase_date DATE,
  -- NON-NEGATIVE magnitude (spec Money) -- direction is the separate column below, never
  -- the amount's sign. Per-kind stat signs live in the views (signed_spend_cents), not here.
  total_cents BIGINT NOT NULL DEFAULT 0 CHECK (total_cents >= 0),
  direction TEXT NOT NULL CHECK (direction IN ('inflow', 'outflow')),
  currency CHAR(3) NOT NULL DEFAULT 'AUD',
  -- Manual FX rate to AUD (decided 4a) -- user-entered, point-in-time, feeds only the
  -- opt-in Combined insight; never live-recomputed from a market feed.
  fx_rate_to_aud NUMERIC,
  aud_equivalent_cents BIGINT,
  kind TEXT NOT NULL CHECK (kind IN ('purchase', 'refund', 'reimbursement', 'income', 'transfer')),
  scan_state TEXT NOT NULL DEFAULT 'idle' CHECK (scan_state IN ('idle', 'uploaded', 'scanning', 'scan_failed')),
  -- TWO separate budgets (spec Invariants): scan_attempts = real provider/parse errors only
  -- (cap 3); reclaim_count = crash/timeout reclaims (high backstop). A reclaim must not
  -- spend a scan_attempt, so deploy churn can't fail a good receipt.
  scan_attempts INTEGER NOT NULL DEFAULT 0,
  reclaim_count INTEGER NOT NULL DEFAULT 0,
  scan_started_at TIMESTAMPTZ,
  -- proposed_at = last machine proposal (scan completion OR recurring auto-creation);
  -- reviewed_at = last human confirmation. NO stored status column -- needs_review /
  -- is_confirmed are DERIVED from these two timestamps alone (see the views below).
  proposed_at TIMESTAMPTZ,
  reviewed_at TIMESTAMPTZ,
  payment_method_id BIGINT REFERENCES payment_methods(id) ON DELETE SET NULL,
  tax_deductible BOOLEAN NOT NULL DEFAULT false,
  note TEXT,
  source TEXT NOT NULL CHECK (source IN ('scanned', 'manual', 'recurring')),
  recurring_id BIGINT REFERENCES recurring_transactions(id) ON DELETE SET NULL,
  -- Set only on recurring-generated rows; unique (recurring_id, occurrence_ordinal) below
  -- makes generation idempotent (a double timer tick or crash-recovery can't double-create).
  occurrence_ordinal INTEGER,
  offsets_receipt_id BIGINT REFERENCES receipts(id) ON DELETE SET NULL,
  raw_ocr_json JSONB,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  -- Canonical direction for the fixed kinds (spec Money); transfer sets it freely (a
  -- transfer still has a physical bank side -- ATM withdrawal=outflow, share-sale=inflow).
  CONSTRAINT chk_direction_by_kind CHECK (
    (kind = 'purchase' AND direction = 'outflow') OR
    (kind IN ('refund', 'reimbursement', 'income') AND direction = 'inflow') OR
    (kind = 'transfer')
  ),
  -- Decided 2b: a refund/income/transfer marked deductible is meaningless.
  CONSTRAINT chk_tax_deductible_purchase_only CHECK (NOT tax_deductible OR kind = 'purchase'),
  -- NULLs are distinct under UNIQUE, so ordinary (non-recurring) rows -- both columns
  -- NULL -- never collide with each other here.
  CONSTRAINT uq_recurring_occurrence UNIQUE (recurring_id, occurrence_ordinal)
);
CREATE INDEX idx_receipts_user_id ON receipts(user_id);
-- Serves both the exact-total dup probe and the imageless tolerance-band range scan (spec
-- Dedupe): currency leads (total_cents is currency-relative, must not false-match across
-- currencies), then total_cents supports the bounded range scan, then purchase_date narrows
-- the +/- day window.
CREATE INDEX idx_receipts_match ON receipts(user_id, currency, total_cents, purchase_date);
CREATE INDEX idx_receipts_scan_state ON receipts(scan_state) WHERE scan_state IN ('uploaded', 'scanning');
CREATE INDEX idx_receipts_merchant_trgm ON receipts USING gin (merchant gin_trgm_ops);

ALTER TABLE recurring_transactions
  ADD CONSTRAINT fk_recurring_purchase_receipt
  FOREIGN KEY (purchase_receipt_id) REFERENCES receipts(id) ON DELETE SET NULL;

-- A receipt owns a SET of images (decided 1a, multi-image receipts) -- replaces a single
-- image_key/image_sha256 column on receipts. user_id is denormalized so the exact-duplicate
-- hash check can scan across every image a user owns without joining through receipts.
CREATE TABLE receipt_images (
  id BIGSERIAL PRIMARY KEY,
  receipt_id BIGINT NOT NULL REFERENCES receipts(id) ON DELETE CASCADE,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  image_key TEXT NOT NULL,
  image_sha256 TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_receipt_images_receipt_id ON receipt_images(receipt_id);
-- Plain (non-unique) index scoped by user: force=true permits genuine re-uploads, and
-- scoping by user_id means the conflict check can't leak another account's receipt existence.
CREATE INDEX idx_receipt_images_dup_check ON receipt_images(user_id, image_sha256);

-- Load-bearing for categorization (decided 2a): manual/quick-add entry auto-creates one
-- implicit item per receipt, so category breakdown stays item-sourced everywhere. Item
-- REVIEW stays optional; item EXISTENCE does not.
CREATE TABLE items (
  id BIGSERIAL PRIMARY KEY,
  receipt_id BIGINT NOT NULL REFERENCES receipts(id) ON DELETE CASCADE,
  name TEXT,
  quantity NUMERIC,
  unit_price_cents BIGINT,
  amount_cents BIGINT NOT NULL DEFAULT 0,
  category_id BIGINT REFERENCES categories(id) ON DELETE SET NULL,
  -- Payslip gross/tax/super breakdown lines etc. (super boundary) -- excluded from every
  -- amount-sum, category-breakdown aggregation, and the sum-mismatch warning.
  informational BOOLEAN NOT NULL DEFAULT false
);
CREATE INDEX idx_items_receipt_id ON items(receipt_id);
CREATE INDEX idx_items_category_id ON items(category_id);

CREATE TABLE item_tags (
  item_id BIGINT NOT NULL REFERENCES items(id) ON DELETE CASCADE,
  tag_id BIGINT NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (item_id, tag_id)
);

-- Derived predicates live in ONE place, not inlined per query -- but LAYERED, because
-- "confirmed & current" and "counts as spend" are different memberships and some queries
-- need the former without the latter (spec Invariants, two-axis state).

-- Base view: every receipts column plus the two timestamp-only derived booleans. This is
-- the highest-risk correctness surface in the app (the >=/> boundary between the two) --
-- single-sourcing it here is the whole point.
CREATE VIEW v_receipt_state AS
SELECT
  r.*,
  (r.proposed_at IS NOT NULL AND (r.reviewed_at IS NULL OR r.proposed_at > r.reviewed_at)) AS needs_review,
  (r.reviewed_at IS NOT NULL AND (r.proposed_at IS NULL OR r.reviewed_at >= r.proposed_at)) AS is_confirmed
FROM receipts r;

-- Receipt-level spend membership (monthly trend, tax-FY total, reconciliation inputs --
-- stats that don't need category). An explicit spend-kind ALLOWLIST, NOT "kind <> transfer"
-- (income pivot, 2026-07-18): "<> transfer" would wrongly pull salary into spend.
CREATE VIEW v_countable_spend AS
SELECT
  vrs.*,
  (vrs.total_cents * CASE WHEN vrs.kind = 'purchase' THEN 1 ELSE -1 END) AS signed_spend_cents
FROM v_receipt_state vrs
LEFT JOIN payment_methods pm ON pm.id = vrs.payment_method_id
WHERE vrs.is_confirmed
  AND vrs.kind IN ('purchase', 'refund', 'reimbursement')
  AND NOT (vrs.kind = 'purchase' AND COALESCE(pm.stored_value, false));

-- Receipt-level income membership.
CREATE VIEW v_income AS
SELECT vrs.*
FROM v_receipt_state vrs
WHERE vrs.is_confirmed AND vrs.kind = 'income';

-- Item-level counterparts (decided 2a, item-only-categorization pivot): ALL category-based
-- insights (category breakdown, income-by-category, item-category filtered trend) select
-- through these, not through any receipt-level category column (there is none).
CREATE VIEW v_countable_spend_items AS
SELECT
  i.id AS item_id,
  i.receipt_id,
  i.category_id,
  i.amount_cents,
  (i.amount_cents * CASE WHEN vcs.kind = 'purchase' THEN 1 ELSE -1 END) AS signed_amount_cents,
  vcs.user_id,
  vcs.currency,
  vcs.purchase_date,
  vcs.kind
FROM items i
JOIN v_countable_spend vcs ON vcs.id = i.receipt_id
WHERE NOT i.informational;

CREATE VIEW v_income_items AS
SELECT
  i.id AS item_id,
  i.receipt_id,
  i.category_id,
  i.amount_cents,
  vi.user_id,
  vi.currency,
  vi.purchase_date
FROM items i
JOIN v_income vi ON vi.id = i.receipt_id
WHERE NOT i.informational;

-- migrate:down

DROP VIEW IF EXISTS v_income_items;
DROP VIEW IF EXISTS v_countable_spend_items;
DROP VIEW IF EXISTS v_income;
DROP VIEW IF EXISTS v_countable_spend;
DROP VIEW IF EXISTS v_receipt_state;

DROP TABLE IF EXISTS item_tags;
DROP TABLE IF EXISTS items;
DROP TABLE IF EXISTS receipt_images;

ALTER TABLE recurring_transactions DROP CONSTRAINT IF EXISTS fk_recurring_purchase_receipt;

DROP TABLE IF EXISTS receipts;
DROP TABLE IF EXISTS recurring_transactions;
DROP TABLE IF EXISTS payment_methods;
DROP TABLE IF EXISTS tags;
DROP TABLE IF EXISTS categories;
DROP TABLE IF EXISTS auth_tokens;
DROP TABLE IF EXISTS sessions;
DROP TABLE IF EXISTS users;

DROP EXTENSION IF EXISTS pg_trgm;
DROP EXTENSION IF EXISTS citext;

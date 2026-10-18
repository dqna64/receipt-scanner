#!/usr/bin/env bash
# Walks the Step 9 HTTP surface -- manual entry, PATCH, items, reference data -- against a
# running server (default: localhost:8080). Covers what the service-level tests can't: routing
# (incl. POST /receipts/manual next to /receipts/{id}), PATCH binding, and the error envelope.
# Assumes Postgres is up and migrated, and the server is already running.
#
# Usage: ./scripts/smoke-test-ledger.sh [base_url]
set -euo pipefail

BASE_URL="${1:-http://localhost:8080}/api/v1"
EMAIL="ledger-smoke-$(date +%s)@example.com"
COOKIES="$(mktemp)"
BODY="$(mktemp)"
trap 'rm -f "$COOKIES" "$BODY"' EXIT

pass() { echo "  OK: $1"; }
fail() { echo "  FAIL: $1 (expected $2, got $3)" >&2; cat "$BODY" >&2; echo >&2; exit 1; }
expect_status() { if [ "$3" = "$2" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi; }
# Field from the last response body, e.g. `field id` or `field 0.id`.
field() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): v=v[int(k)] if isinstance(v,list) else v[k]
print(json.dumps(v) if isinstance(v,(dict,list,bool)) or v is None else v)' "$BODY" "$1"; }
# Dynamic JSON bodies go through a variable first: macOS's bash 3.2 brace-expands an
# escaped-quote "{...,...}" nested inside "$(...)", splitting it at the commas.
call() { # method path [json]
  curl -s -b "$COOKIES" -o "$BODY" -w '%{http_code}' -X "$1" "$BASE_URL$2" -H 'Content-Type: application/json' \
    ${3:+-d "$3"}
}

echo "== register + login =="
curl -s -o /dev/null -X POST "$BASE_URL/auth/register" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$EMAIL\",\"password\":\"correct-horse-battery-staple\"}"
code=$(curl -s -c "$COOKIES" -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/auth/login" \
  -H 'Content-Type: application/json' -d "{\"email\":\"$EMAIL\",\"password\":\"correct-horse-battery-staple\"}")
expect_status "login" 200 "$code"

echo "== reference data =="
expect_status "list categories" 200 "$(call GET /categories)"
expect_status "22 seeded categories" 22 "$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))))' "$BODY")"
DINING=$(python3 -c 'import json,sys; print(next(c["id"] for c in json.load(open(sys.argv[1])) if c["name"]=="dining/takeaway"))' "$BODY")
expect_status "create tag" 201 "$(call POST /tags '{"name":"work"}')"
TAG=$(field id)
expect_status "duplicate tag rejected" 409 "$(call POST /tags '{"name":"WORK"}')"
expect_status "create payment method" 201 "$(call POST /payment-methods '{"name":"Opal","stored_value":true}')"
expect_status "stored_value echoed" true "$(field stored_value)"

echo "== manual entry =="
coffee_json="{\"merchant\":\"Cafe\",\"total_cents\":450,\"category_id\":$DINING}"
expect_status "cash coffee" 201 "$(call POST /receipts/manual "$coffee_json")"
COFFEE=$(field id)
expect_status "GET /receipts/manual is not a receipt id" 405 "$(call GET /receipts/manual)"
expect_status "purchase+inflow rejected" 400 "$(call POST /receipts/manual '{"total_cents":1,"direction":"inflow"}')"
expect_status "error envelope code" invalid_request "$(field error.code)"
expect_status "malformed JSON rejected" 400 "$(call POST /receipts/manual '{not json')"

echo "== receipt read + PATCH =="
expect_status "get coffee" 200 "$(call GET /receipts/$COFFEE)"
expect_status "born confirmed" true "$(field is_confirmed)"
REVIEWED_AT=$(field reviewed_at)
expect_status "patch merchant" 200 "$(call PATCH /receipts/$COFFEE '{"merchant":"Cafe Uno"}')"
expect_status "merchant updated" "Cafe Uno" "$(field merchant)"
expect_status "reviewed_at unchanged by edit" "$REVIEWED_AT" "$(field reviewed_at)"
expect_status "patch missing receipt" 404 "$(call PATCH /receipts/999999999 '{"note":"x"}')"

echo "== items =="
expect_status "list items" 200 "$(call GET /receipts/$COFFEE/items)"
expect_status "implicit item amount" 450 "$(field 0.amount_cents)"
ITEM=$(field 0.id)
muffin_json="{\"name\":\"muffin\",\"amount_cents\":350,\"tag_ids\":[$TAG]}"
expect_status "add item" 201 "$(call POST /receipts/$COFFEE/items "$muffin_json")"
expect_status "tag attached" "[$TAG]" "$(field tag_ids)"
expect_status "patch item" 200 "$(call PATCH /receipts/$COFFEE/items/$ITEM '{"name":"flat white"}')"
expect_status "item renamed" "flat white" "$(field name)"
expect_status "delete item" 200 "$(call DELETE /receipts/$COFFEE/items/$ITEM)"
expect_status "delete again" 404 "$(call DELETE /receipts/$COFFEE/items/$ITEM)"

echo "== unauthenticated =="
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/categories")
expect_status "no session -> 401" 401 "$code"

echo ""
echo "All ledger smoke checks passed for $EMAIL"

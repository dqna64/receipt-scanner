#!/usr/bin/env bash
# Walks the full Step 5 auth flow against a running server (default: localhost:8080).
# Assumes Postgres is up and migrated, and the server is already running.
#
# Usage: ./scripts/smoke-test-auth.sh [base_url]
set -euo pipefail

BASE_URL="${1:-http://localhost:8080}"
EMAIL="smoke-test-$(date +%s)@example.com"
COOKIES="$(mktemp)"
trap 'rm -f "$COOKIES"' EXIT

pass() { echo "  OK: $1"; }
fail() { echo "  FAIL: $1 (expected $2, got $3)" >&2; exit 1; }

expect_status() {
  local desc=$1 expected=$2 actual=$3
  if [ "$actual" = "$expected" ]; then pass "$desc"; else fail "$desc" "$expected" "$actual"; fi
}

echo "== register =="
code=$(curl -s -o /tmp/smoke_body.json -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/register" \
  -H 'Content-Type: application/json' -d "{\"email\":\"$EMAIL\",\"password\":\"correct-horse-battery-staple\"}")
expect_status "register succeeds" 201 "$code"

echo "== duplicate register rejected =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/register" \
  -H 'Content-Type: application/json' -d "{\"email\":\"$EMAIL\",\"password\":\"something-else\"}")
expect_status "duplicate email rejected" 409 "$code"

echo "== wrong password rejected =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/login" \
  -H 'Content-Type: application/json' -d "{\"email\":\"$EMAIL\",\"password\":\"wrong-password\"}")
expect_status "wrong password rejected" 401 "$code"

echo "== login succeeds, sets session cookie =="
code=$(curl -s -c "$COOKIES" -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/login" \
  -H 'Content-Type: application/json' -d "{\"email\":\"$EMAIL\",\"password\":\"correct-horse-battery-staple\"}")
expect_status "login succeeds" 200 "$code"
grep -q session "$COOKIES" || fail "session cookie present" "cookie" "none"
pass "session cookie present"

echo "== logout without cookie rejected =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/logout")
expect_status "logout without cookie rejected" 401 "$code"

echo "== logout with valid cookie succeeds =="
code=$(curl -s -b "$COOKIES" -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/logout")
expect_status "logout succeeds" 200 "$code"

echo "== reusing the now-deleted session fails =="
code=$(curl -s -b "$COOKIES" -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/v1/auth/logout")
expect_status "deleted session rejected" 401 "$code"

echo ""
echo "All auth smoke checks passed for $EMAIL"

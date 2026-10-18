#!/usr/bin/env bash
# Fails if any test passes a bare `co_await` as a Catch2 assertion macro argument, e.g.
# `REQUIRE(co_await f())`. Under GCC the co_await re-executes inside the macro's wrapper
# (f runs 2-3x); Clang never reproduces it, so a Clang-only local build can't catch it.
# Bind the result to a local first. Full writeup: .agent_dqna64/log.md 2026-10-12.
# Used by the pre-push hook and CI. Usage: check-test-patterns.sh [dir...] (default: tests/)
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
dirs=("$@")
[ ${#dirs[@]} -eq 0 ] && dirs=("$repo_root/tests")

# Slurps each file so a macro split across lines (`REQUIRE(\n  co_await ...`) still matches;
# [^;]* stops the match at the end of the assertion statement.
hits=$(find "${dirs[@]}" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 |
  xargs -0 perl -0777 -ne '
    while (/\b((?:REQUIRE|CHECK)\w*)\s*\([^;]*?\bco_await\b/g) {
      my $line = 1 + (substr($_, 0, $-[0]) =~ tr/\n//);
      print "$ARGV:$line: $1(... co_await ...)\n";
    }')

if [ -n "$hits" ]; then
  echo "co_await inside a Catch2 assertion macro (unsafe under GCC -- bind to a local first):" >&2
  echo "$hits" >&2
  exit 1
fi

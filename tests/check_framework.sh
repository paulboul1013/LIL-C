#!/bin/sh
# Verifies that the unit-test framework reports failures correctly.
# Usage: sh tests/check_framework.sh <selftest-binary>

set -u

binary=$1
temp_dir=$(mktemp -d)
trap 'rm -rf -- "$temp_dir"' EXIT HUP INT TERM

"$binary" >"$temp_dir/stdout" 2>"$temp_dir/stderr"
status=$?
failures=0

expect() {
    if ! grep -F -q -- "$2" "$temp_dir/$1"; then
        echo "framework self-test: missing in $1: $2" >&2
        failures=$((failures + 1))
    fi
}

if test "$status" -eq 0; then
    echo "framework self-test: expected nonzero exit status" >&2
    failures=$((failures + 1))
fi

expect stderr "test_framework_selftest.c:"
expect stderr "test 'selftest_int_mismatch' failed"
expect stderr "expected: 42 (42)"
expect stderr "actual:   41 (41)"
expect stderr 'expected: "expected text" ("expected text")'
expect stderr 'actual:   "actual\ttext"'
expect stderr "actual:   NULL"
expect stdout "[ FAIL ] selftest_assert_stops_test"
expect stdout "[  OK  ] selftest_passing"
expect stderr "expected: 18446744073709551615 (UINT64_MAX)"
expect stderr "actual:   18446744073709551614 ((uint64_t)UINT64_MAX - 1)"
expect stdout "[ FAIL ] selftest_unsigned_mismatch"
expect stdout "6 tests run, 1 passed, 5 failed"

if test "$failures" -ne 0; then
    cat "$temp_dir/stdout" "$temp_dir/stderr" >&2
    exit 1
fi

echo "Framework self-test passed"

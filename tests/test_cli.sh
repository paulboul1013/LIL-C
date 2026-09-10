#!/bin/sh

set -u

main=$1
fixture_dir=tests/fixtures/cli
temp_dir=$(mktemp -d)
failures=0

cleanup() {
    rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

for source in "$fixture_dir"/*.lil; do
    name=${source%.lil}
    expected_status=$(tr -d '[:space:]' <"$name.status")

    "$main" "$source" >"$temp_dir/stdout" 2>"$temp_dir/stderr"
    actual_status=$?

    if test "$actual_status" -ne "$expected_status"; then
        echo "$source: expected status $expected_status, got $actual_status" >&2
        failures=$((failures + 1))
    fi

    if ! cmp -s "$name.stdout" "$temp_dir/stdout"; then
        echo "$source: stdout differs" >&2
        diff -u "$name.stdout" "$temp_dir/stdout" >&2
        failures=$((failures + 1))
    fi

    if ! cmp -s "$name.stderr" "$temp_dir/stderr"; then
        echo "$source: stderr differs" >&2
        diff -u "$name.stderr" "$temp_dir/stderr" >&2
        failures=$((failures + 1))
    fi
done

if test "$failures" -ne 0; then
    exit 1
fi

echo "CLI tests passed"

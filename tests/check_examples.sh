#!/bin/sh
# Stage 1-4 gates (LIC_C_compiler_plan.md): --dump-tokens, --dump-ast and
# --dump-ir must handle every file in examples/ and examples/accept/ --
# exit 0 and produce a complete dump (an End token; a program node; an
# entry block). --dump-tokens prints nothing on stderr; --dump-ast and
# --dump-ir (which run semantic analysis) may print §6.2 warnings but
# nothing else. When /dev/full exists, writing a dump there must fail
# with status 1 and "failed to write output" (spec §6.4.2).
#
# M4 gate: every example must also run correctly on the IR interpreter.
# The expected results live in tests/golden/interp/ (hand-verified), whose
# fixture example_NAME.lil (for examples/NAME.lil) or accept_NAME.lil (for
# examples/accept/NAME.lil) must be an exact copy of the example, so the
# golden run checks the example itself.
#
# Usage: sh tests/check_examples.sh <compiler>

set -u

if test $# -ne 1; then
    echo "usage: $0 <compiler>" >&2
    exit 2
fi

compiler=$1
root=$(cd "$(dirname "$0")/.." && pwd)
temp_dir=$(mktemp -d)
trap 'rm -rf -- "$temp_dir"' EXIT HUP INT TERM
failures=0
checked=0

for example in "$root"/examples/*.lil "$root"/examples/accept/*.lil; do
    test -f "$example" || continue
    checked=$((checked + 1))

    case $example in
        */accept/*) golden="$root/tests/golden/interp/accept_$(basename "$example")" ;;
        *) golden="$root/tests/golden/interp/example_$(basename "$example")" ;;
    esac

    if ! cmp -s "$example" "$golden"; then
        echo "FAIL $example: $golden is missing or not an exact copy" >&2
        failures=$((failures + 1))
    fi

    "$compiler" --dump-tokens "$example" >"$temp_dir/out" 2>"$temp_dir/err"
    status=$?

    if test "$status" -ne 0 || test -s "$temp_dir/err" ||
        ! tail -n 1 "$temp_dir/out" | grep -q '^End [0-9]*:[0-9]*$'; then
        echo "FAIL --dump-tokens $example (status $status)" >&2
        sed 's/^/    /' "$temp_dir/err" >&2
        failures=$((failures + 1))
    fi

    "$compiler" --dump-ast "$example" >"$temp_dir/out" 2>"$temp_dir/err"
    status=$?

    if test "$status" -ne 0 ||
        grep -v -q ': warning: ' "$temp_dir/err" ||
        ! head -n 1 "$temp_dir/out" | grep -q '^(program @1:1+[0-9]*'; then
        echo "FAIL --dump-ast $example (status $status)" >&2
        sed 's/^/    /' "$temp_dir/err" >&2
        failures=$((failures + 1))
    fi

    "$compiler" --dump-ir "$example" >"$temp_dir/out" 2>"$temp_dir/err"
    status=$?

    if test "$status" -ne 0 ||
        grep -v -q ': warning: ' "$temp_dir/err" ||
        ! grep -q '^entry:$' "$temp_dir/out"; then
        echo "FAIL --dump-ir $example (status $status)" >&2
        sed 's/^/    /' "$temp_dir/err" >&2
        failures=$((failures + 1))
    fi
done

# Spec §6.4.2: a failed write to standard output is an error, never exit 0.
if test -w /dev/full && test "$checked" -ne 0; then
    "$compiler" --dump-tokens "$example" >/dev/full 2>"$temp_dir/err"
    status=$?

    if test "$status" -ne 1 ||
        ! grep -qx 'failed to write output' "$temp_dir/err"; then
        echo "FAIL --dump-tokens $example >/dev/full (status $status)" >&2
        failures=$((failures + 1))
    fi

    for mode in --dump-ast --dump-ir; do
        "$compiler" $mode "$example" >/dev/full 2>"$temp_dir/err"
        status=$?

        if test "$status" -ne 1 ||
            ! grep -qx 'failed to write output' "$temp_dir/err"; then
            echo "FAIL $mode $example >/dev/full (status $status)" >&2
            failures=$((failures + 1))
        fi
    done
fi

if test "$checked" -eq 0; then
    echo "examples check: no examples found" >&2
    exit 1
fi

if test "$failures" -ne 0; then
    echo "examples check: $failures of $checked failed" >&2
    exit 1
fi

echo "examples check passed ($checked files, --dump-tokens, --dump-ast," \
    "--dump-ir; interpreter results in tests/golden/interp)"

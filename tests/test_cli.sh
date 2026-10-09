#!/bin/sh
#
# Golden test runner.
#
# Usage:
#   sh tests/test_cli.sh [--update] [--interp-flag FLAG] <compiler>
#                        [fixture.lil | directory]...
#
# Without fixture arguments every fixture under tests/golden/ is run.
# Each fixture is a NAME.lil file (or a NAME.ir file of IR text, docs/spec.md
# §8.3, which is run with --ir-input added before the other arguments)
# with expectations next to it:
#
#   NAME.stdout   expected standard output
#   NAME.stderr   expected standard error
#   NAME.status   expected exit status (decimal; 128+N for death by signal N)
#   NAME.args     optional extra command-line arguments (whitespace-separated,
#                 no quoting), placed before the source path
#   NAME.cmdline  optional (cli/ only): the complete argument list
#                 (whitespace-separated, no quoting; may be empty). When it
#                 exists the source path is NOT appended, so fixtures can test
#                 a missing argument or an unreadable file. NAME.lil is then
#                 only the fixture's anchor and documentation.
#
# The directory a fixture lives in decides how it is run:
#
#   tokens/  <compiler> --dump-tokens [args] NAME.lil
#   ast/     <compiler> --dump-ast    [args] NAME.lil
#   ir/      <compiler> --dump-ir     [args] NAME.lil
#   interp/  <compiler> --interp      [args] NAME.lil (the IR interpreter
#            runs the program: its putc output, the compiler's warnings
#            and the program's status -- 136 for SIGFPE -- are compared)
#   asm/     <compiler> --dump-asm    [args] NAME.lil
#   cli/     <compiler>               [args] NAME.lil
#   run/     <compiler> [args] NAME.lil -o PROGRAM must exit 0, and its
#            stderr (for example warnings) must equal NAME.compile-stderr;
#            then PROGRAM is executed and *its* stdout, stderr and status
#            are compared.
#
# Fixtures are run from the repository root with a repository-relative
# path, so diagnostics in .stderr files read "tests/golden/<dir>/NAME.lil".
#
# --interp-flag FLAG (for example -O1) reruns interp/ fixtures with FLAG
# added after their arguments, against the SAME expectations (spec §5.7:
# every optimization level behaves like the interpreter at -O0). Fixtures
# whose NAME.args already choose -O0/-O1, --passes or --step-limit (the
# step count depends on the optimization) are skipped.
#
# --update rewrites the expectation files from the actual results instead
# of comparing (used by 'make update-golden'; review the diff afterwards).
# It exits nonzero if any fixture could not be updated (for example a run/
# fixture that fails to compile).
#
# Every command is limited to GOLDEN_TIMEOUT seconds (default 10) when
# timeout(1) is available; a timed-out command has status 124.

set -u

update=0
interp_flag=""

while test $# -gt 0; do
    case $1 in
        --update) update=1; shift ;;
        --interp-flag)
            if test $# -lt 2; then
                echo "$0: --interp-flag needs a flag" >&2
                exit 2
            fi
            interp_flag=$2
            shift 2
            ;;
        *) break ;;
    esac
done

if test $# -lt 1; then
    echo "usage: $0 [--update] [--interp-flag FLAG] <compiler> [fixture.lil | directory]..." >&2
    exit 2
fi

compiler=$1
shift

case $compiler in
    /*) ;;
    *) compiler=$(pwd)/$compiler ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

if ! test -x "$compiler"; then
    echo "$0: compiler '$compiler' is not executable" >&2
    exit 2
fi

timeout_seconds=${GOLDEN_TIMEOUT:-10}

if command -v timeout >/dev/null 2>&1; then
    limit="timeout $timeout_seconds"
else
    limit=""
fi

temp_dir=$(mktemp -d)
passed=0
failed=0
skipped=0
updated=0

cleanup() {
    rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

# run_limited STDOUT-FILE STDERR-FILE COMMAND [ARG]...
# Runs COMMAND with its stdout and stderr captured and returns its status
# (128+N when killed by signal N). Only COMMAND's own output is captured:
# timeout(1)'s "dumped core" note and the shell's "Floating point
# exception"-style report go to /dev/null.
run_limited() {
    out_file=$1
    err_file=$2
    shift 2

    (
        if test -n "$limit"; then
            # shellcheck disable=SC2016
            $limit sh -c 'exec "$@" 2>"$0"' "$err_file" "$@" >"$out_file"
        else
            "$@" >"$out_file" 2>"$err_file"
        fi
        status=$?
        exit "$status"
    ) 2>/dev/null
}

fail() {
    echo "FAIL $1: $2" >&2
    fixture_failed=1
}

compare_file() {
    source=$1
    expected=$2
    actual=$3
    label=$4

    if ! test -f "$expected"; then
        fail "$source" "missing $expected (run 'make update-golden')"
        return
    fi

    if ! cmp -s "$expected" "$actual"; then
        fail "$source" "$label differs"
        diff -u "$expected" "$actual" | sed 's/^/    /' >&2
    fi
}

# update_file ACTUAL EXPECTATION: copy ACTUAL over EXPECTATION if they differ.
update_file() {
    if ! test -f "$2" || ! cmp -s "$1" "$2"; then
        cp -- "$1" "$2"
        echo "updated $2"
        updated=$((updated + 1))
    fi
}

run_fixture() {
    source=$1
    kind=$(basename "$(dirname "$source")")
    args=""
    fixture_failed=0

    case $source in
        *.ir)
            name=${source%.ir}
            args="--ir-input"

            if test -f "$name.lil"; then
                fail "$source" "$name.lil and $name.ir share expectations"
                failed=$((failed + 1))
                return
            fi
            ;;
        *) name=${source%.lil} ;;
    esac

    if test -f "$name.args"; then
        args="$args $(tr '\n' ' ' <"$name.args")"
    fi

    if test -n "$interp_flag" && test "$kind" = "interp"; then
        case " $args " in
            *" -O0 "*|*" -O1 "*|*" --passes="*|*" --step-limit "*)
                skipped=$((skipped + 1))
                return
                ;;
        esac

        args="$args $interp_flag"
    fi

    if test -f "$name.cmdline" && test "$kind" != "cli"; then
        fail "$source" "$name.cmdline is only supported in cli/"
        failed=$((failed + 1))
        return
    fi

    case $kind in
        tokens) mode_args="--dump-tokens" ;;
        ast) mode_args="--dump-ast" ;;
        ir) mode_args="--dump-ir" ;;
        interp) mode_args="--interp" ;;
        asm) mode_args="--dump-asm" ;;
        cli|run) mode_args="" ;;
        *)
            fail "$source" "unknown golden directory '$kind'"
            failed=$((failed + 1))
            return
            ;;
    esac

    if test "$kind" = "run"; then
        program="$temp_dir/program"
        rm -f -- "$program"

        # shellcheck disable=SC2086
        run_limited "$temp_dir/compile.stdout" "$temp_dir/compile.stderr" \
            "$compiler" $args "$source" -o "$program"
        compile_status=$?

        if test "$compile_status" -ne 0 || ! test -x "$program"; then
            fail "$source" "compilation failed with status $compile_status"
            sed 's/^/    /' "$temp_dir/compile.stderr" >&2
            failed=$((failed + 1))
            return
        fi

        if test "$update" -eq 1; then
            update_file "$temp_dir/compile.stderr" "$name.compile-stderr"
        else
            compare_file "$source" "$name.compile-stderr" \
                "$temp_dir/compile.stderr" "compiler stderr"
        fi

        run_limited "$temp_dir/stdout" "$temp_dir/stderr" "$program"
        actual_status=$?
    elif test -f "$name.cmdline"; then
        cmdline=$(tr '\n' ' ' <"$name.cmdline")

        # shellcheck disable=SC2086
        run_limited "$temp_dir/stdout" "$temp_dir/stderr" \
            "$compiler" $cmdline
        actual_status=$?
    else
        # shellcheck disable=SC2086
        run_limited "$temp_dir/stdout" "$temp_dir/stderr" \
            "$compiler" $mode_args $args "$source"
        actual_status=$?
    fi

    echo "$actual_status" >"$temp_dir/status"

    # Refuse to record a command-line rejection as the expectation of a
    # tokens/, ast/, ir/, interp/, asm/ or run/ fixture: that usually means the
    # compiler does not support the mode flag yet, and the golden would
    # enshrine the usage error instead of the real output.
    if test "$kind" != "cli" && grep -q '^usage:' "$temp_dir/stderr"; then
        fail "$source" "compiler rejected the command line (usage error); is '$mode_args' supported?"
        sed 's/^/    /' "$temp_dir/stderr" >&2
        failed=$((failed + 1))
        return
    fi

    if test "$update" -eq 1; then
        for extension in stdout stderr status; do
            update_file "$temp_dir/$extension" "$name.$extension"
        done
        passed=$((passed + 1))
        return
    fi

    if ! test -f "$name.status"; then
        fail "$source" "missing $name.status (run 'make update-golden')"
    else
        expected_status=$(tr -d '[:space:]' <"$name.status")

        if test "$expected_status" != "$actual_status"; then
            fail "$source" "expected status $expected_status, got $actual_status"
        fi
    fi

    compare_file "$source" "$name.stdout" "$temp_dir/stdout" "stdout"
    compare_file "$source" "$name.stderr" "$temp_dir/stderr" "stderr"

    if test "$fixture_failed" -eq 0; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
}

if test $# -eq 0; then
    set -- tests/golden
fi

# Fixture paths are split on newlines only, with globbing off, so paths
# containing spaces or glob characters stay intact.
newline='
'
set -f

for target in "$@"; do
    if test -d "$target"; then
        fixtures=$(find "$target" -type f \( -name '*.lil' -o -name '*.ir' \) |
            LC_ALL=C sort)
    elif test -f "$target"; then
        fixtures=$target
    else
        echo "$0: no such fixture or directory: $target" >&2
        exit 2
    fi

    saved_ifs=$IFS
    IFS=$newline

    for source in $fixtures; do
        IFS=$saved_ifs
        run_fixture "$source"
        IFS=$newline
    done

    IFS=$saved_ifs
done

if test "$update" -eq 1; then
    echo "Golden expectations: $passed fixtures checked, $updated files updated"

    if test "$failed" -ne 0; then
        echo "Golden expectations: $failed fixtures could not be updated" >&2
        exit 1
    fi

    exit 0
fi

if test "$failed" -ne 0; then
    echo "Golden tests${interp_flag:+ (interp/ at $interp_flag)}:" \
        "$passed passed, $failed failed" >&2
    exit 1
fi

if test "$passed" -eq 0; then
    echo "Golden tests: no fixtures found" >&2
    exit 1
fi

if test -n "$interp_flag"; then
    echo "Golden tests passed ($passed fixtures with interp/ at $interp_flag," \
        "$skipped skipped)"
else
    echo "Golden tests passed ($passed fixtures)"
fi

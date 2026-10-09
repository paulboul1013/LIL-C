#!/bin/sh
# Verifies tests/test_cli.sh itself against a fake compiler, so behaviour
# that no real fixture exercises yet (run/ fixtures, --update failures,
# unusual paths, .cmdline fixtures) stays correct.
#
# Usage: sh tests/check_golden_runner.sh

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
runner="$root/tests/test_cli.sh"
temp_dir=$(mktemp -d)
trap 'rm -rf -- "$temp_dir"' EXIT HUP INT TERM
failures=0

check() {
    description=$1
    expected_status=$2
    shift 2

    "$@" >"$temp_dir/out" 2>&1
    status=$?

    if test "$status" -ne "$expected_status"; then
        echo "golden runner self-test: $description:" \
            "expected status $expected_status, got $status" >&2
        sed 's/^/    /' "$temp_dir/out" >&2
        failures=$((failures + 1))
    fi
}

expect_output() {
    if ! grep -F -q -- "$1" "$temp_dir/out"; then
        echo "golden runner self-test: output lacks: $1" >&2
        sed 's/^/    /' "$temp_dir/out" >&2
        failures=$((failures + 1))
    fi
}

# Fake compiler: 'SOURCE -o PROGRAM' writes a program that prints "hello"
# and exits 3; any other call prints its arguments. A source containing
# FAILCOMPILE fails to compile; one containing KILLME dies by SIGTERM.
compiler="$temp_dir/fake compiler"
cat >"$compiler" <<'EOF'
#!/bin/sh
out=""
source=""
printf 'args:'
for argument in "$@"; do printf ' [%s]' "$argument"; done
printf '\n'
while test $# -gt 0; do
    case $1 in
        -o) out=$2; shift 2 ;;
        *) source=$1; shift ;;
    esac
done
if test -n "$source" && grep -q FAILCOMPILE "$source" 2>/dev/null; then
    echo "fake: cannot compile" >&2
    exit 1
fi
if test -n "$source" && grep -q USAGE "$source" 2>/dev/null; then
    echo "usage: fake <source.lil>" >&2
    exit 1
fi
if test -n "$source" && grep -q KILLME "$source" 2>/dev/null; then
    kill -TERM $$
fi
if test -n "$out"; then
    echo "fake: warning" >&2
    printf '#!/bin/sh\necho hello\necho oops >&2\nexit 3\n' >"$out"
    chmod +x "$out"
fi
exit 0
EOF
chmod +x "$compiler"

golden="$temp_dir/golden dir"
mkdir -p "$golden/run" "$golden/cli" "$golden/interp"
printf 'r0 = 3;\n' >"$golden/run/exit three.lil"
printf 'x;\n' >"$golden/cli/plain [x].lil"
printf '%% no source path\n' >"$golden/cli/no_args.lil"
printf -- '--flag\n' >"$golden/cli/no_args.cmdline"
printf 'KILLME\n' >"$golden/cli/killed.lil"
printf 'x;\n' >"$golden/interp/prog.lil"
printf 'entry:\n' >"$golden/interp/text.ir"
printf -- '-O1\n' >"$golden/interp/text.args"

# Paths with spaces and glob characters, run/ fixtures, .cmdline.
check "update creates expectations" 0 sh "$runner" --update "$compiler" "$golden"
check "fresh expectations pass" 0 sh "$runner" "$compiler" "$golden"
expect_output "Golden tests passed (6 fixtures)"

check "run/ program status recorded" 0 \
    grep -q '^3$' "$golden/run/exit three.status"
check "run/ program stdout recorded" 0 \
    grep -q '^hello$' "$golden/run/exit three.stdout"
check "run/ compiler stderr recorded" 0 \
    grep -q '^fake: warning$' "$golden/run/exit three.compile-stderr"
check "cmdline replaces the argument list" 0 \
    grep -q '^args: \[--flag\]$' "$golden/cli/no_args.stdout"
check "death by signal is 128+N" 0 \
    grep -q '^143$' "$golden/cli/killed.status"
check "interp/ runs --interp" 0 \
    grep -q '^args: \[--interp\] \[.*/interp/prog.lil\]$' \
    "$golden/interp/prog.stdout"
check ".ir fixtures add --ir-input before .args" 0 \
    grep -q '^args: \[--interp\] \[--ir-input\] \[-O1\] \[.*/interp/text.ir\]$' \
    "$golden/interp/text.stdout"

# --interp-flag reruns interp/ fixtures with the flag against the same
# expectations (the fake compiler echoes its arguments, so prog.lil now
# differs) and skips fixtures whose .args choose an -O level (text.ir).
check "--interp-flag adds the flag" 1 \
    sh "$runner" --interp-flag -O1 "$compiler" "$golden/interp"
expect_output "+args: [--interp] [-O1] ["
expect_output "Golden tests (interp/ at -O1): 0 passed, 1 failed"

# NAME.lil and NAME.ir cannot share one set of expectations.
printf 'x;\n' >"$golden/interp/text.lil"
check "NAME.lil next to NAME.ir is rejected" 1 sh "$runner" "$compiler" "$golden"
expect_output "share expectations"
rm -f "$golden/interp/text.lil"

# A changed compiler warning is a failure.
printf 'other warning\n' >"$golden/run/exit three.compile-stderr"
check "compiler stderr is compared" 1 sh "$runner" "$compiler" "$golden"
expect_output "compiler stderr differs"

# --update must fail when a fixture cannot be updated.
printf 'FAILCOMPILE\n' >"$golden/run/broken.lil"
check "update fails on uncompilable run/ fixture" 1 \
    sh "$runner" --update "$compiler" "$golden"
expect_output "compilation failed with status 1"
expect_output "1 fixtures could not be updated"
rm -f "$golden/run/broken.lil"

# --update must fail on an unknown fixture directory.
mkdir -p "$golden/bogus"
printf 'x;\n' >"$golden/bogus/x.lil"
check "update fails on unknown directory" 1 \
    sh "$runner" --update "$compiler" "$golden"
rm -rf "$golden/bogus"

# .cmdline outside cli/ is rejected.
printf '\n' >"$golden/run/exit three.cmdline"
check "cmdline only in cli/" 1 sh "$runner" "$compiler" "$golden"
rm -f "$golden/run/exit three.cmdline"

# A usage error is never recorded as the golden of a non-cli/ fixture
# (for example a tokens/ fixture added before --dump-tokens exists).
mkdir -p "$golden/tokens"
printf 'USAGE\n' >"$golden/tokens/unsupported.lil"
check "update refuses a usage error outside cli/" 1 \
    sh "$runner" --update "$compiler" "$golden"
expect_output "compiler rejected the command line"
check "usage error is not recorded" 1 \
    test -f "$golden/tokens/unsupported.stderr"
rm -rf "$golden/tokens"

if test "$failures" -ne 0; then
    exit 1
fi

echo "Golden runner self-test passed"

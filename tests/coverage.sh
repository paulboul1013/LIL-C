#!/bin/sh
#
# Line-coverage summary from gcov (no lcov needed).
#
# Usage: sh tests/coverage.sh [-f <file-min-percent>] [-g <source.c>]...
#            <object-dir> <min-percent> <source.c>...
#
# <object-dir> mirrors the source tree: src/lexer.c -> <object-dir>/src/lexer.o
# (.gcno/.gcda next to it). Prints one line per source file and the total;
# exits nonzero if the total is below <min-percent>, or if any file given
# with -g (a "gated" file, repeatable) is below <file-min-percent>
# (default 90). Gated files must also be among the reported <source.c>.

set -u

usage() {
    echo "usage: $0 [-f <file-min-percent>] [-g <source.c>]..." \
        "<object-dir> <min-percent> <source.c>..." >&2
    exit 2
}

file_minimum=90
gated=" "

while getopts f:g: option; do
    case $option in
        f) file_minimum=$OPTARG ;;
        g) gated="$gated$OPTARG " ;;
        *) usage ;;
    esac
done

shift $((OPTIND - 1))

if test $# -lt 3; then
    usage
fi

object_dir=$1
minimum=$2
shift 2

for file in $gated; do
    case " $* " in
        *" $file "*) ;;
        *)
            echo "$0: gated file $file is not among the reported sources" >&2
            exit 2
            ;;
    esac
done

temp_dir=$(mktemp -d)
trap 'rm -rf -- "$temp_dir"' EXIT HUP INT TERM

: >"$temp_dir/counts"

for source in "$@"; do
    gcov_dir="$object_dir/$(dirname "$source")"

    if ! gcov -t -o "$gcov_dir" "$source" >"$temp_dir/out" 2>"$temp_dir/err"; then
        echo "$0: gcov failed for $source" >&2
        cat "$temp_dir/err" >&2
        exit 1
    fi

    # Count only the section that belongs to $source (gcov -t may also emit
    # sections for headers). Lines are "count:lineno:text"; "-" means not
    # executable, "#####"/"=====" means never executed.
    awk -v want="$source" '
        BEGIN { FS = ":"; active = 0; total = 0; covered = 0 }
        {
            count = $1; gsub(/[ \t]/, "", count)
            lineno = $2; gsub(/[ \t]/, "", lineno)
            if (lineno == "0" && $3 == "Source") {
                name = $4
                for (i = 5; i <= NF; i++) name = name ":" $i
                active = (name == want)
                next
            }
            if (!active || count == "-" || lineno == "0") next
            total++
            if (count != "#####" && count != "=====") covered++
        }
        END { printf "%s %d %d\n", want, covered, total }
    ' "$temp_dir/out" >>"$temp_dir/counts"
done

awk -v minimum="$minimum" -v file_minimum="$file_minimum" \
    -v gated="$gated" '
    {
        covered += $2; total += $3
        percent = $3 ? 100 * $2 / $3 : 100
        is_gated = index(gated, " " $1 " ") > 0
        printf "%-24s %6.2f%%  (%d/%d lines)%s\n", $1, percent, $2, $3,
            is_gated ? "  [gate " file_minimum "%]" : ""
        if (is_gated && percent + 0 < file_minimum + 0) {
            failed = failed sprintf("coverage of %s %.2f%% is below the required %s%%\n",
                $1, percent, file_minimum)
        }
    }
    END {
        percent = total ? 100 * covered / total : 100
        printf "%-24s %6.2f%%  (%d/%d lines)\n", "TOTAL", percent, covered, total
        if (percent + 0 < minimum + 0) {
            failed = failed sprintf("coverage %.2f%% is below the required %s%%\n",
                percent, minimum)
        }
        if (failed != "") {
            printf "%s", failed > "/dev/stderr"
            exit 1
        }
    }
' "$temp_dir/counts"

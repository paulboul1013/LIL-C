#!/usr/bin/env python3
"""Differential test of the LIL-C compiler (plan "差分測試", docs/spec.md
§5.7, §8.7).

Usage:
  diff_test.py LILC [--count N] [--seed S] [--keep DIR] [FILE.lil ...]

Without FILE arguments, N random programs (default 300) are generated with
gen_lil.py from seed S (default 20261009). For every program:

  1. three-way: the independent oracle (lil_oracle.py, written from the
     spec only), 'LILC f --interp' (-O0) and 'LILC f --interp -O1' must
     give the same stdout bytes and status (136 = SIGFPE).
  2. -O0 round trip: 'LILC f --dump-ir' parsed back with --ir-input and
     dumped again is byte-identical, and interprets the same.
  3. -O1 is a fixpoint: the -O1 dump, read back with --ir-input and
     optimized with -O1 again, is byte-identical (idempotence and
     termination of the pass pipeline), and interprets the same.
  4. -O1 never has more IR instructions than -O0.

Mismatching programs are copied to --keep DIR (when given). Prints a
summary and exits 1 on any mismatch.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lil_oracle import run_source  # noqa: E402
from gen_lil import gen_program  # noqa: E402

sys.setrecursionlimit(100000)


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, timeout=120)
    st = p.returncode if p.returncode >= 0 else 128 - p.returncode
    return p.stdout, st, p.stderr


def instruction_count(ir):
    return sum(1 for line in ir.splitlines() if line.startswith(b"  "))


def check(lilc, path, tmpd, stats):
    """Returns a list of problems (empty when the program passes)."""
    problems = []
    src = open(path, "rb").read()
    oout, ost = run_source(src, 50_000_000)
    if ost == "error":
        return ["oracle rejects the program"]
    if ost == "steps":
        return ["oracle step limit (the generator must make programs end)"]

    results = {}
    for level in ("-O0", "-O1"):
        out, st, err = run([lilc, path, "--interp", level])
        results[level] = (out, st)
        if (out, st) != (oout, ost):
            problems.append("%s: oracle %r status %r, lilc %r status %r %r"
                            % (level, oout[:120], ost, out[:120], st,
                               err[:200]))
    if problems:
        return problems

    stats["traps"] += ost == 136
    stats["exits"] += ost != 136
    stats["bytes"] += len(oout)

    counts = {}
    for level in ("-O0", "-O1"):
        ir, st1, err1 = run([lilc, path, "--dump-ir", level])
        if st1 != 0:
            problems.append("%s --dump-ir failed: %r" % (level, err1[:200]))
            continue
        counts[level] = instruction_count(ir)
        irp = os.path.join(tmpd, "p.ir")
        with open(irp, "wb") as f:
            f.write(ir)
        ir2, st2, err2 = run([lilc, irp, "--ir-input", "--dump-ir", level])
        out3, st3, err3 = run([lilc, irp, "--ir-input", "--interp"])
        if st2 != 0 or ir2 != ir:
            problems.append("%s: dumped IR is not a fixpoint of %s "
                            "--ir-input (status %d) %r" % (level, level, st2,
                                                           err2[:200]))
        if (out3, st3) != results[level]:
            problems.append("%s: dumped IR interprets differently: %r/%r"
                            % (level, (out3[:60], st3),
                               (results[level][0][:60], results[level][1])))
    if len(counts) == 2:
        stats["o0"] += counts["-O0"]
        stats["o1"] += counts["-O1"]
        if counts["-O1"] > counts["-O0"]:
            problems.append("-O1 has more instructions (%d) than -O0 (%d)"
                            % (counts["-O1"], counts["-O0"]))
    return problems


def main():
    args = sys.argv[1:]
    if not args:
        sys.stderr.write(__doc__)
        sys.exit(2)
    lilc = os.path.abspath(args.pop(0))
    count, seed, keep, files = 300, 20261009, None, []
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--keep":
            keep = args.pop(0)
        else:
            files.append(a)

    tmpd = tempfile.mkdtemp(prefix="lil_diff_")
    try:
        if not files:
            rng = random.Random(seed)
            for i in range(count):
                path = os.path.join(tmpd, "r%04d.lil" % i)
                with open(path, "w") as f:
                    f.write(gen_program(rng))
                files.append(path)

        stats = {"traps": 0, "exits": 0, "bytes": 0, "o0": 0, "o1": 0}
        bad = 0
        for path in files:
            problems = check(lilc, path, tmpd, stats)
            if problems:
                bad += 1
                print("MISMATCH %s" % path)
                for p in problems:
                    print("  " + p)
                if keep:
                    os.makedirs(keep, exist_ok=True)
                    shutil.copy(path, keep)
        print("difftest: %d programs (seed %d), %d mismatches; %d exited, "
              "%d trapped, %d output bytes; IR instructions -O0 %d, -O1 %d"
              % (len(files), seed, bad, stats["exits"], stats["traps"],
                 stats["bytes"], stats["o0"], stats["o1"]))
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

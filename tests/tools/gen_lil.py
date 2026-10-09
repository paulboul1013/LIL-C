#!/usr/bin/env python3
"""Random terminating LIL-C program generator (differential testing, plan
"隨機程式產生器").

Usage: gen_lil.py OUTDIR COUNT [SEED]
Writes OUTDIR/rNNNN.lil. As a library: gen_program(random.Random(seed)).

Programs always terminate: every generated loop has its own counter (cN,
never touched by random code) that is incremented first thing in the body
and bounded in the condition, and the only other loops are 'while (0)' and
'do ... while (0)'. Division may divide by zero or INT64_MIN / -1 (traps
are part of what is tested). Expressions include short-circuit operators
with putc side effects, nested assignments, wraparound constants, char and
octal literals and mixed-case names.

To exercise the -O1 optimizer (docs/spec.md §8.7) the programs also contain
constant subexpressions, algebraic identities (x+0, 0+x, x-0, x*1, x*0,
x-x, x/1, x&0, x|0, x==x), constant conditions (if (1), if (0),
while (0), do ... while (0), constant && and ||), stores followed by loads
of the same variable, and dead code after an unconditional break or
continue.
"""
import os
import random
import sys

VARS = ["r0", "a", "b", "c", "x", "y", "Z", ".t", "_u"]
CONSTS = ["0", "1", "2", "3", "7", "-1", "10", "255", "256", "0777", "010",
          "9223372036854775807", "(-9223372036854775807 - 1)",
          "4611686018427387904", "'A'", "'\\n'", "'z'", "'\\0'", "1000003"]
# Identities -O1 simplifies; %s is any expression, %v a variable.
IDENTITIES = ["(%s + 0)", "(0 + %s)", "(%s - 0)", "(%s * 1)", "(1 * %s)",
              "(%s * 0)", "(0 * %s)", "(%s / 1)", "(%s & 0)", "(%s | 0)",
              "(%s & -1)", "(%s | -1)", "(%s * -1)", "(0 - %s)",
              "(%v - %v)", "(%v == %v)", "(%v <= %v)", "(%v ~= %v)",
              "(%v & %v)", "(%v | %v)"]


class Gen:
    def __init__(self, rng):
        self.r = rng
        self.loop_id = 0

    def var(self):
        v = self.r.choice(VARS)
        # names are case-insensitive: randomly re-case them
        if self.r.random() < 0.2:
            v = "".join(ch.upper() if self.r.random() < 0.5 else ch.lower()
                        for ch in v)
        return v

    def const_expr(self, d=0):
        """A constant subexpression (folds to one constant at -O1)."""
        r = self.r
        if d >= 2 or r.random() < 0.4:
            return r.choice(CONSTS)
        op = r.choice(["+", "-", "*", "&", "|", "<", "==", "/"])
        if op == "/":
            # a nonzero, non -1 divisor: folds; 0 or -1 also appear below
            return "(%s / %s)" % (self.const_expr(d + 1),
                                  r.choice(["2", "3", "7", "-5", "10"]))
        return "(%s %s %s)" % (self.const_expr(d + 1), op,
                               self.const_expr(d + 1))

    def identity(self, d):
        form = self.r.choice(IDENTITIES)
        if "%v" in form:
            v = self.var()
            return form.replace("%v", v)
        return form % self.expr(d + 1)

    def expr(self, d=0):
        r = self.r
        if d >= 4 or r.random() < 0.25:
            k = r.random()
            if k < 0.4:
                return self.var()
            if k < 0.8:
                return r.choice(CONSTS)
            if k < 0.9:
                return self.const_expr()
            return str(r.randint(0, 50))
        k = r.random()
        if k < 0.38:
            op = r.choice(["+", "-", "*", "&", "|", "<", "<=", ">", ">=",
                           "==", "~=", "+", "-", "*"])
            return "(%s %s %s)" % (self.expr(d + 1), op, self.expr(d + 1))
        if k < 0.46:
            # division: sometimes surely nonzero divisor, sometimes anything
            if r.random() < 0.9:
                return "(%s / (%s | 1))" % (self.expr(d + 1), self.expr(d + 1))
            if r.random() < 0.5:
                # constant divisors 0 and -1: never folded, never deleted
                return "(%s / %s)" % (self.expr(d + 1),
                                      r.choice(["0", "-1", "(1 - 1)"]))
            return "(%s / %s)" % (self.expr(d + 1), self.expr(d + 1))
        if k < 0.56:
            op = r.choice(["&&", "||"])
            if r.random() < 0.3:
                # a constant left operand decides (or not) at compile time
                return "(%s %s %s)" % (r.choice(["0", "1", "(2 - 2)"]), op,
                                       self.expr(d + 1))
            return "(%s %s %s)" % (self.expr(d + 1), op, self.expr(d + 1))
        if k < 0.66:
            return self.identity(d)
        if k < 0.72:
            return "%s%s" % (r.choice(["-", "~", "- ", "~-"]),
                             self.expr(d + 1))
        if k < 0.84:
            # putc with a printable-ish byte so output is easy to read
            if r.random() < 0.7:
                return "putc('a' + (%s & 15))" % self.expr(d + 1)
            return "putc(%s)" % self.expr(d + 1)
        return "(%s = %s)" % (self.var(), self.expr(d + 1))

    def cond(self):
        """A condition: sometimes a constant one."""
        r = self.r
        if r.random() < 0.25:
            return r.choice(["1", "0", "(3 > 2)", "(1 - 1)", "(0 && x)",
                             "(1 || x)", "(2 * 0)"])
        return self.expr()

    def stmt(self, depth, in_loop):
        r = self.r
        k = r.random()
        if depth >= 3:
            k = min(k, 0.59)
        if k < 0.30:
            return "%s = %s;" % (self.var(), self.expr())
        if k < 0.36:
            # store, then load the same variable (load forwarding)
            v = self.var()
            return "%s = %s; %s = %s + %s;" % (v, self.expr(1), self.var(),
                                               v, v)
        if k < 0.43:
            return "%s;" % self.expr()
        if k < 0.50:
            return "putc(%s);" % self.expr()
        if k < 0.59:
            if in_loop and r.random() < 0.6:
                jump = r.choice(["break;", "continue;"])
                if r.random() < 0.4:
                    # unconditional, with dead code after it
                    return "{ %s %s }" % (jump, self.stmt(depth + 1, in_loop))
                return "if (%s) %s" % (self.expr(2), jump)
            return ";"
        if k < 0.74:
            s = "if (%s) %s" % (self.cond(), self.block(depth + 1, in_loop))
            if r.random() < 0.5:
                s += " else " + (self.block(depth + 1, in_loop)
                                 if r.random() < 0.7
                                 else self.stmt(depth + 1, in_loop))
            return s
        if k < 0.80:
            return self.block(depth + 1, in_loop)
        if k < 0.84:
            # constant-false loops: the body is dead (while) or runs once
            body = self.stmts(depth + 1, True, r.randint(0, 3))
            if r.random() < 0.5:
                return "while (%s) { %s }" % (r.choice(["0", "(1 - 1)"]), body)
            return "do { %s } while (%s);" % (body, r.choice(["0", "(2 < 1)"]))
        self.loop_id += 1
        c = "c%d" % self.loop_id
        bound = r.randint(0, 6)
        body_inner = self.stmts(depth + 1, True, r.randint(0, 4))
        cond = self.cond() if r.random() < 0.3 else self.expr(2)
        if r.random() < 0.55:
            return "%s = 0; while (%s < %d && %s) { %s = %s + 1; %s }" % (
                c, c, bound, cond, c, c, body_inner)
        return "%s = 0; do { %s = %s + 1; %s } while (%s < %d && %s);" % (
            c, c, c, body_inner, c, bound, cond)

    def block(self, depth, in_loop):
        return "{ %s }" % self.stmts(depth, in_loop, self.r.randint(0, 3))

    def stmts(self, depth, in_loop, n):
        return " ".join(self.stmt(depth, in_loop) for _ in range(n))


def gen_program(rng):
    g = Gen(rng)
    n = rng.randint(4, 14)
    lines = [g.stmt(0, False) for _ in range(n)]
    return "\n".join(lines) + "\n"


def main():
    out = sys.argv[1]
    count = int(sys.argv[2])
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261009
    rng = random.Random(seed)
    os.makedirs(out, exist_ok=True)
    for i in range(count):
        with open("%s/r%04d.lil" % (out, i), "w") as f:
            f.write(gen_program(rng))


if __name__ == "__main__":
    main()

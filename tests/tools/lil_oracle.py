#!/usr/bin/env python3
"""Independent LIL-C v1 reference evaluator, written from docs/spec.md only
(§2 lexing, §3 statements, §4 expressions, §5 execution semantics).

Usage: lil_oracle.py PROG.lil [--step-limit N]
  Writes putc bytes to stdout; exits with r0 & 0xff; on a division trap
  flushes stdout and kills itself with SIGFPE (shell status 136).
  Lexical/syntax/semantic errors -> message on stderr, exit 1 (coarse; no
  attempt to match the compiler's diagnostics).
  Step limit exceeded -> exit 3 (so harnesses can tell it apart).

As a library: run_source(src_bytes, step_limit) -> (stdout_bytes, status)
where status is the exit code, or 136 for SIGFPE, or 'error'/'steps'.
"""
import os
import signal
import sys

MASK = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1


def wrap(v):
    v &= MASK
    return v - (1 << 64) if v >> 63 else v


class LilError(Exception):
    pass


class Trap(Exception):
    pass


class StepLimit(Exception):
    pass


KEYWORDS = {"if", "else", "while", "do", "break", "continue"}
OPS = ["&&", "||", "<=", ">=", "==", "~=", "(", ")", "{", "}", ";", ",",
       "=", "+", "-", "*", "/", "&", "|", "~", "<", ">"]
ESC = {"0": 0, "n": 10, "r": 13, "t": 9, "\\": 92, "'": 39}


def lex(src):
    toks = []
    i, n = 0, len(src)
    while i < n:
        c = chr(src[i])
        if c in " \t\r\n":
            i += 1
            continue
        if c == "%":
            while i < n and src[i] != 10:
                i += 1
            continue
        if c.isascii() and (c.isalpha() or c in "_."):
            j = i
            while j < n and chr(src[j]).isascii() and (chr(src[j]).isalnum() or chr(src[j]) in "_."):
                j += 1
            s = src[i:j].decode("ascii")
            if s.lower() in KEYWORDS:
                toks.append(("kw", s.lower()))
            else:
                toks.append(("id", s.lower()))
            i = j
            continue
        if c.isdigit():
            j = i
            while j < n and chr(src[j]).isdigit():
                j += 1
            s = src[i:j].decode()
            if len(s) > 1 and s[0] == "0":
                if any(ch in "89" for ch in s):
                    raise LilError("invalid digit in octal literal")
                v = int(s, 8)
            else:
                v = int(s)
            if v > INT64_MAX:
                raise LilError("integer literal out of range")
            toks.append(("num", v))
            i = j
            continue
        if c == "'":
            j = i + 1
            if j >= n or src[j] == 10:
                raise LilError("unterminated character literal")
            if src[j] == ord("'"):
                raise LilError("empty character literal")
            if src[j] == ord("\\"):
                if j + 1 >= n or src[j + 1] == 10:
                    raise LilError("unterminated character literal")
                e = chr(src[j + 1])
                if e not in ESC:
                    raise LilError("unsupported escape sequence")
                v = ESC[e]
                j += 2
            else:
                v = src[j]
                j += 1
            if j >= n or src[j] == 10:
                raise LilError("unterminated character literal")
            if src[j] != ord("'"):
                raise LilError("character literal contains more than one character")
            toks.append(("num", v))
            i = j + 1
            continue
        for op in OPS:
            if src[i:i + len(op)] == op.encode():
                toks.append(("op", op))
                i += len(op)
                break
        else:
            raise LilError("unexpected character")
    toks.append(("end", None))
    return toks


# AST: statements are tuples, expressions are tuples.
BINPREC = [  # lowest .. highest (excluding = which is handled separately)
    ["||"], ["&&"], ["|"], ["&"], ["==", "~="], ["<", "<=", ">", ">="],
    ["+", "-"], ["*", "/"],
]


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.p = 0

    def peek(self, k=0):
        return self.t[self.p + k]

    def eat(self, kind, val=None):
        tk = self.peek()
        if tk[0] != kind or (val is not None and tk[1] != val):
            raise LilError("syntax error: expected %s %s got %r" % (kind, val, tk))
        self.p += 1
        return tk

    def is_(self, kind, val=None):
        tk = self.peek()
        return tk[0] == kind and (val is None or tk[1] == val)

    def program(self):
        out = []
        while not self.is_("end"):
            out.append(self.stmt())
        return out

    def stmt(self):
        if self.is_("op", ";"):
            self.p += 1
            return ("empty",)
        if self.is_("op", "{"):
            self.p += 1
            body = []
            while not self.is_("op", "}"):
                if self.is_("end"):
                    raise LilError("expected '}'")
                body.append(self.stmt())
            self.p += 1
            return ("block", body)
        if self.is_("kw", "if"):
            self.p += 1
            self.eat("op", "(")
            c = self.expr()
            self.eat("op", ")")
            th = self.stmt()
            el = None
            if self.is_("kw", "else"):
                self.p += 1
                el = self.stmt()
            return ("if", c, th, el)
        if self.is_("kw", "while"):
            self.p += 1
            self.eat("op", "(")
            c = self.expr()
            self.eat("op", ")")
            return ("while", c, self.stmt())
        if self.is_("kw", "do"):
            self.p += 1
            body = self.stmt()
            self.eat("kw", "while")
            self.eat("op", "(")
            c = self.expr()
            self.eat("op", ")")
            self.eat("op", ";")
            return ("do", body, c)
        if self.is_("kw", "break"):
            self.p += 1
            self.eat("op", ";")
            return ("break",)
        if self.is_("kw", "continue"):
            self.p += 1
            self.eat("op", ";")
            return ("continue",)
        if self.is_("kw"):
            raise LilError("unexpected keyword")
        e = self.expr()
        self.eat("op", ";")
        return ("expr", e)

    def expr(self):
        # assignment = identifier "=" assignment | logical-or
        if self.is_("id") and self.peek(1) == ("op", "=") :
            name = self.eat("id")[1]
            self.p += 1
            return ("assign", name, self.expr())
        e = self.binary(0)
        if self.is_("op", "="):
            raise LilError("invalid assignment target")
        return e

    def binary(self, level):
        if level == len(BINPREC):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek()[0] == "op" and self.peek()[1] in BINPREC[level]:
            op = self.eat("op")[1]
            right = self.binary(level + 1)
            left = ("bin", op, left, right)
        return left

    def unary(self):
        if self.is_("op", "-") or self.is_("op", "~"):
            op = self.eat("op")[1]
            return ("un", op, self.unary())
        return self.primary()

    def primary(self):
        tk = self.peek()
        if tk[0] == "num":
            self.p += 1
            return ("num", tk[1])
        if tk[0] == "id":
            self.p += 1
            if self.is_("op", "("):
                self.p += 1
                args = []
                if not self.is_("op", ")"):
                    args.append(self.expr())
                    while self.is_("op", ","):
                        self.p += 1
                        args.append(self.expr())
                self.eat("op", ")")
                if tk[1] != "putc" or len(args) != 1:
                    raise LilError("semantic: bad call")
                return ("putc", args[0])
            if tk[1] == "putc":
                raise LilError("semantic: putc as variable")
            return ("var", tk[1])
        if tk == ("op", "("):
            self.p += 1
            e = self.expr()
            self.eat("op", ")")
            return e
        raise LilError("expected expression")


def check_assign_putc(e):
    pass


class BreakEx(Exception):
    pass


class ContinueEx(Exception):
    pass


class Machine:
    def __init__(self, step_limit=10_000_000):
        self.g = {}
        self.out = bytearray()
        self.steps = 0
        self.limit = step_limit

    def tick(self):
        self.steps += 1
        if self.limit and self.steps > self.limit:
            raise StepLimit()

    def ev(self, e):
        k = e[0]
        self.tick()
        if k == "num":
            return e[1]
        if k == "var":
            return self.g.get(e[1], 0)
        if k == "assign":
            if e[1] == "putc":
                raise LilError("assign to putc")
            v = self.ev(e[2])
            self.g[e[1]] = v
            return v
        if k == "putc":
            v = self.ev(e[1])
            self.out.append(v & 0xFF)
            return v
        if k == "un":
            v = self.ev(e[2])
            return wrap(-v) if e[1] == "-" else wrap(~v)
        if k == "bin":
            op = e[1]
            if op == "&&":
                a = self.ev(e[2])
                if a == 0:
                    return 0
                return 1 if self.ev(e[3]) != 0 else 0
            if op == "||":
                a = self.ev(e[2])
                if a != 0:
                    return 1
                return 1 if self.ev(e[3]) != 0 else 0
            a = self.ev(e[2])
            b = self.ev(e[3])
            if op == "+":
                return wrap(a + b)
            if op == "-":
                return wrap(a - b)
            if op == "*":
                return wrap(a * b)
            if op == "/":
                if b == 0 or (a == INT64_MIN and b == -1):
                    raise Trap()
                q = abs(a) // abs(b)
                return wrap(q if (a < 0) == (b < 0) else -q)
            if op == "&":
                return wrap(a & b)
            if op == "|":
                return wrap(a | b)
            return int({"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b,
                        "==": a == b, "~=": a != b}[op])
        raise AssertionError(k)

    def ex(self, s, in_loop=False):
        k = s[0]
        self.tick()
        if k == "empty":
            return
        if k == "expr":
            self.ev(s[1])
        elif k == "block":
            for t in s[1]:
                self.ex(t, in_loop)
        elif k == "if":
            if self.ev(s[1]) != 0:
                self.ex(s[2], in_loop)
            elif s[3] is not None:
                self.ex(s[3], in_loop)
        elif k == "while":
            while self.ev(s[1]) != 0:
                try:
                    self.ex(s[2], True)
                except BreakEx:
                    break
                except ContinueEx:
                    pass
        elif k == "do":
            while True:
                try:
                    self.ex(s[1], True)
                except BreakEx:
                    break
                except ContinueEx:
                    pass
                if self.ev(s[2]) == 0:
                    break
        elif k == "break":
            raise BreakEx()
        elif k == "continue":
            raise ContinueEx()


def check_loops(stmts, in_loop=False):
    for s in stmts:
        k = s[0]
        if k in ("break", "continue") and not in_loop:
            raise LilError("%s outside of a loop" % k)
        if k == "block":
            check_loops(s[1], in_loop)
        elif k == "if":
            check_loops([s[2]] + ([s[3]] if s[3] else []), in_loop)
        elif k == "while":
            check_loops([s[2]], True)
        elif k == "do":
            check_loops([s[1]], True)


def run_source(src, step_limit=10_000_000):
    """Returns (stdout bytes, status). status: int exit code, 136 for SIGFPE,
    'error' for a compile-time error, 'steps' if the step limit ran out."""
    try:
        prog = Parser(lex(src)).program()
        check_loops(prog)
    except LilError:
        return b"", "error"
    m = Machine(step_limit)
    try:
        for s in prog:
            m.ex(s)
    except Trap:
        return bytes(m.out), 136
    except StepLimit:
        return bytes(m.out), "steps"
    return bytes(m.out), m.g.get("r0", 0) & 0xFF


def main():
    sys.setrecursionlimit(100000)
    path = sys.argv[1]
    limit = 10_000_000
    if "--step-limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--step-limit") + 1])
    with open(path, "rb") as f:
        src = f.read()
    out, st = run_source(src, limit)
    sys.stdout.buffer.write(out)
    sys.stdout.flush()
    if st == "error":
        sys.stderr.write("oracle: compile error\n")
        sys.exit(1)
    if st == "steps":
        sys.stderr.write("oracle: step limit\n")
        sys.exit(3)
    if st == 136:
        signal.signal(signal.SIGFPE, signal.SIG_DFL)
        os.kill(os.getpid(), signal.SIGFPE)
    sys.exit(st)


if __name__ == "__main__":
    main()

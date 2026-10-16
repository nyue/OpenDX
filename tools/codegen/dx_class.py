#!/usr/bin/env python3.11
"""Generate a libdx class header or source file from a .X class description.

    dx_class.py (-h | -c) INPUT.X [-o OUTPUT]

-h  write the .h file: method and class vector declarations plus the text of
    INPUT.X and every .X file it INCLUDEs
-c  write the .c file: class vectors, method dispatch routines,
    "not implemented" routines and DXGet...Class routines
-o  where to write; default: stdout

INCLUDE lines name .X files in the same folder as INPUT.X. This is a port of
src/exec/libdx/class and class.awk and produces byte-identical output, quirks
included (awk's concatenated array keys, uninitialised values reading as 0 or
"", and so on). Standard library only. Needs Python 3.11+.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ALPHA_LC = "abcdefghijklmnopqrstuvwxyz"
ALPHA_UC = ALPHA_LC.upper()
NOP = "_dxfno_"
CLASS_OVERHEAD = 2
ROOT_OVERHEAD = 3

# Files are handled as latin-1 so every byte passes through unchanged.
ENC = "latin-1"

_STRNUM = re.compile(r"[ \t\n]*[-+]?(\d+\.?\d*([eE][-+]?\d+)?|\.\d+([eE][-+]?\d+)?)[ \t\n]*")


def _basename_x(path: Path) -> str:
    name = path.name
    return name[:-2] if name.endswith(".X") and name != ".X" else name


def _wrap(path: Path) -> str:
    x = _basename_x(path)
    return f"#ifndef __{x}\n#define __{x}\n{path.read_text(ENC)}#endif\n"


def _includes(path: Path, srcdir: Path) -> str:
    # sed -n 's/^INCLUDE[ \t]*\(.*\)/\1/p', then `echo $incl` word-splits
    found = []
    for line in path.read_text(ENC).split("\n"):
        m = re.match(r"INCLUDE[ \t]*(.*)", line)
        if m:
            found.append(m.group(1))
    words = " ".join(found).split()
    if not words:
        return ""
    if len(words) > 1:
        sys.exit(f"{path}: more than one INCLUDE is not supported: {words}")
    inc = srcdir / words[0]
    return _includes(inc, srcdir) + _wrap(inc)


def build_stream(flag: str, path: Path) -> str:
    srcdir = path.parent
    text = "#do_h\n" + _includes(path, srcdir)
    if flag == "-c":
        text += "#do_c\n"
    return text + _wrap(path)


def _records(text: str) -> list[str]:
    recs = text.split("\n")
    if recs and recs[-1] == "":
        recs.pop()
    return recs


def _num(v) -> int:
    # an uninitialised awk value: "" in a key, 0 in arithmetic
    return 0 if v == "" else v


def _true(v) -> bool:
    # awk truth: numbers and numeric-looking field strings by value
    if isinstance(v, (int, float)):
        return v != 0
    if _STRNUM.fullmatch(v):
        return float(v) != 0
    return v != ""


class Awk:
    """State and rules of class.awk; arrays keep awk's concatenated keys."""

    def __init__(self) -> None:
        self.out: list[str] = []
        self.do_h = self.do_c = 0
        self.nclasses = 0
        self.cls = ""
        self.classes: dict[int, str] = {}
        self.do_class_c: dict[int, int] = {}
        self.super_names: dict[str, object] = {}
        self.uc_names: dict[str, str] = {}
        self.lc_names: dict[str, str] = {}
        self.first: dict[str, int] = {}
        self.nvalues: dict[str, int] = {}
        self.class_vector: dict[str, str] = {}
        self.method_name: dict[str, str] = {}
        self.method_number: dict[str, int] = {}
        self.returns: dict[str, str] = {}
        self.nargs: dict[str, int] = {}
        self.argtype: dict[str, str] = {}
        self.p("/*\n * Automatically generated - DO NOT EDIT!\n */\n\n")

    def p(self, s: str) -> None:
        self.out.append(s)

    def args(self, method: str, named: bool) -> str:
        na = self.nargs.get(method, 0)
        parts = []
        for k in range(na):
            t = self.argtype.get(method + str(k), "")
            parts.append(f"{t} {ALPHA_LC[k:k + 1]}" if named else t)
        return ",".join(parts)

    def record(self, line: str) -> None:
        fields = re.split(r"[ \t\n]+", line.strip(" \t\n")) if line.strip(" \t\n") else []
        nf = len(fields)
        f = lambda i: fields[i - 1] if 0 < i <= nf else ""

        if line == "#do_h":
            self.do_h = 1
        elif line == "#do_c":
            self.do_c = 1
        elif line.startswith("INCLUDE"):
            pass
        elif line.startswith("CLASS") or re.match(r"SUBCLASS.* OF ", line):
            self.define_class(f(2), f(4) if nf == 4 else 0)
        elif line.startswith("IMPLEMENTS"):
            cls = self.cls
            for i in range(2, nf + 1):
                method = f(i)
                n = self.method_number.get(cls + method, "")
                self.class_vector[cls + str(n)] = f"_dxf{cls}_{method}"
                if self.do_h:
                    na = self.nargs.get(method, 0)
                    a = [cls] + [self.argtype.get(method + str(k), "") for k in range(1, na)]
                    a = a if na > 0 else []
                    self.p(f"{self.returns.get(method, '')} _dxf{cls}_{method}({','.join(a)});\n")
        elif line.startswith("DEFINES"):
            self.define_method(line)
        elif not re.match(r"[ \t]*//", line):
            if self.do_h:
                self.p(line + "\n")

    def define_class(self, cls: str, sup) -> None:
        self.cls = cls
        self.do_class_c[self.nclasses] = self.do_c
        self.classes[self.nclasses] = cls
        self.nclasses += 1
        self.super_names[cls] = sup

        uc = "".join(ALPHA_UC[ALPHA_LC.index(c)] if c in ALPHA_LC else c for c in cls)
        lc = "".join(ALPHA_LC[ALPHA_UC.index(c)] if c in ALPHA_UC else c for c in cls)
        self.uc_names[cls] = uc
        self.lc_names[cls] = lc
        cv = self.class_vector

        if _true(sup):
            fst = self.first[cls] = self.nvalues.get(sup, "")
            for i in range(_num(fst)):
                cv[cls + str(i)] = cv.get(sup + str(i), "")
                method = self.method_name[cls + str(i)] = self.method_name.get(sup + str(i), "")
                self.method_number[cls + method] = i
            fs = self.first.get(sup, "")
            cv[cls + str(fst)] = cv[cls + str(fs)] = "CLASS_" + uc
            cv[cls + str(_num(fst) + 1)] = cv[cls + str(_num(fs) + 1)] = f'"{lc}"'
            self.nvalues[cls] = _num(fst) + CLASS_OVERHEAD
        else:
            fst = self.first[cls] = ROOT_OVERHEAD
            cv[cls + str(fst)] = "CLASS_" + uc
            cv[cls + str(fst + 1)] = f'"{lc}"'
            self.nvalues[cls] = fst + CLASS_OVERHEAD
            if self.do_h:
                self.p("struct _root {\n"
                       "    int size;\n"
                       "    Class class;\n"
                       "    char *name;\n"
                       "};\n"
                       "#define CLASS_SIZE(x) (((struct _root *)(x))->size)\n"
                       "#define CLASS_CLASS(x) (((struct _root *)(x))->class)\n"
                       "#define CLASS_NAME(x) (((struct _root *)(x))->name)\n"
                       "\n")

        if ROOT_OVERHEAD > 0:
            cv[cls + "0"] = f"sizeof(struct {lc})"
            cv[cls + "1"] = "CLASS_" + uc
            cv[cls + "2"] = f'"{lc}"'

        if self.do_h:
            self.p(f"\nextern struct {lc}_class _dxd{lc}_class;\n")

    def define_method(self, line: str) -> None:
        aaa = line.split("(")
        bbb = re.split(r"[ \t\n]+", aaa[0].strip(" \t\n")) if aaa[0].strip(" \t\n") else []
        typ = bbb[1] if len(bbb) > 1 else ""
        method = bbb[2] if len(bbb) > 2 else ""
        ccc = aaa[1].split(")") if len(aaa) > 1 else [""]
        args = ccc[0].split(",") if ccc[0] else []
        na = len(args)

        cls = self.cls
        nv = self.nvalues.get(cls, "")
        self.method_number[cls + method] = nv
        self.method_name[cls + str(nv)] = method
        self.class_vector[cls + str(nv)] = NOP + method
        self.nvalues[cls] = _num(nv) + 1

        self.returns[method] = typ
        self.nargs[method] = na
        for i, a in enumerate(args):
            self.argtype[method + str(i)] = a

        if self.do_h:
            a = self.args(method, named=False)
            self.p(f"{typ} _dxf{method}({a});\n")
            self.p(f"{typ} {NOP}{method}({a});\n")

    def end(self) -> None:
        if not self.do_c:
            return
        self.p("\n")
        for i in range(self.nclasses):
            cls = self.classes[i]
            lc = self.lc_names[cls]
            nv = self.nvalues.get(cls, 0)
            sup = self.super_names[cls]
            fst = self.first[cls] + CLASS_OVERHEAD

            self.p(f"struct {lc}_class {{\n")
            if _true(sup):
                self.p(f"    struct {self.lc_names.get(sup, '')}_class super;\n")
            elif ROOT_OVERHEAD > 0:
                self.p("    struct _root root;\n")
            self.p("    Class class;\n    char *name;\n")
            for j in range(fst, nv):
                method = self.method_name.get(cls + str(j), "")
                self.p(f"    {self.returns.get(method, '')} (*{method})();\n")
            self.p("};\n\n")

            if not self.do_class_c[i]:
                continue

            for j in range(fst, nv):
                method = self.method_name.get(cls + str(j), "")
                ret = self.returns.get(method, "")
                fail = "ERROR" if "Error" in ret else "NULL"
                self.p(f"{ret} {NOP}{method}({self.args(method, named=True)}) {{\n"
                       "    char *name;\n"
                       f"    name = (*(struct {self.lc_names[cls]}_class **)a)->name;\n"
                       f'    DXSetError(ERROR_BAD_PARAMETER, "#12130", "DX{method}", name);\n'
                       f"    return {fail};\n"
                       "}\n\n")

            self.p(f"struct {lc}_class _dxd{lc}_class = {{\n")
            for j in range(nv):
                self.p(f"    {self.class_vector.get(cls + str(j), '')},\n")
            self.p("};\n")

            for j in range(fst, nv):
                method = self.method_name.get(cls + str(j), "")
                ret = self.returns.get(method, "")
                fail = "ERROR" if "Error" in ret else "NULL"
                na = self.nargs.get(method, 0)
                call = ([f"({cls})a"] + [ALPHA_LC[k:k + 1] for k in range(1, na)]) if na > 0 else []
                self.p(f"\n{ret} _dxf{method}({self.args(method, named=True)}) {{\n"
                       "    if (!a)\n"
                       f"        return {fail};\n"
                       f"    return (*(struct {lc}_class **)a)->{method}({','.join(call)});\n"
                       "}\n")
            self.p("\n")

            self.p(f"Class DXGet{cls}Class({cls} o) {{\n"
                   f"    return o? (*(struct {lc}_class **)o)->class: CLASS_MIN;\n"
                   "}\n\n")


def generate(flag: str, path: Path) -> str:
    awk = Awk()
    for rec in _records(build_stream(flag, path)):
        awk.record(rec)
    awk.end()
    return "".join(awk.out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], add_help=False)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("-h", dest="flag", action="store_const", const="-h")
    mode.add_argument("-c", dest="flag", action="store_const", const="-c")
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", dest="output", type=Path)
    ap.add_argument("--help", action="help")
    a = ap.parse_args()

    text = generate(a.flag, a.input)
    if a.output:
        a.output.write_text(text, ENC)
    else:
        sys.stdout.buffer.write(text.encode(ENC))
    return 0


if __name__ == "__main__":
    sys.exit(main())

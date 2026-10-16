#!/usr/bin/env python3.11
"""Generate the module-registration C file from a .mdf file.

    mdf2c.py [--dynamic] [-o OUT] [MDF]

A line-for-line port of lib/mdf2c.awk; `awk -f lib/mdf2c.awk < dx.mdf`
becomes `mdf2c.py dx.mdf -o user.c`.

MDF        input .mdf file (default stdin)
-o         output .c file (default stdout)
--dynamic  emit DXEntry() for a run-time loadable module instead of
           _dxf_user_modules() (the awk script's `-v dynamic=1`)

Behaviour of GNU awk that the output depends on, reproduced here:
  - records are split on "\\n" only; a final line without a newline is still a
    record, and "\\r" stays part of the last field
  - default field splitting is on runs of space, tab and newline, ignoring
    leading and trailing ones (not Python's wider str.split())
  - split() with "[", ";" or '"' splits on that literal character; only the
    first two pieces of the "[" split are used, so "a[b[c" keeps "[b"
  - `$2+0` takes the longest leading decimal number (hex is not converted,
    as in gawk without --non-decimal-data); a non-numeric value is 0
  - %d truncates toward zero, and `j < count` loops use the unrounded count
  - unset array elements (e.g. a negative or fractional index when REPEAT
    exceeds the inputs) are "", and arrays are not cleared between modules
  - the header "{" is written on the first record of any kind, so an empty
    input gives only the prologue and the closing "}"
Bytes are read and written as Latin-1 so any input passes through unchanged.
Needs Python 3.11+.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PROLOGUE = """\
/* Automatically generated - may need to edit! */

#include <dx/dx.h>
#include <dx/modflags.h>

#if defined(intelnt) || defined(WIN32)
#include <windows.h>
#endif

#if defined(__cplusplus)
extern "C" Error DXAddModule (char *, ...);
#else
extern Error DXAddModule (char *, ...);
#endif

"""

DYNAMIC_ENTRY = """\
#if defined(__cplusplus)
extern "C" Error m_%s(Object*, Object*);
#endif
#if defined(intelnt) || defined(WIN32)
void FAR WINAPI DXEntry()
#else
  #if defined(__cplusplus)
    extern "C" void DXEntry()
  #else
    void DXEntry()
  #endif
#endif
"""

AWK_WS = " \t\n"
AWK_FIELDS = re.compile(r"[ \t\n]+")
AWK_NUMBER = re.compile(r"[ \t\n\r\f\v]*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)")
QUOTED = re.compile(r'".+"')


def fields(s: str) -> list[str]:
    s = s.strip(AWK_WS)
    return AWK_FIELDS.split(s) if s else []


def split_literal(s: str, sep: str) -> list[str]:
    return s.split(sep) if s else []


def awk_number(s: str) -> int | float:
    m = AWK_NUMBER.match(s)
    if not m:
        return 0
    v = float(m.group(1))
    return int(v) if v.is_integer() else v


def awk_d(v: int | float) -> str:
    return str(int(v))


class Mdf2C:
    def __init__(self, dynamic: bool) -> None:
        self.dynamic = dynamic
        self.out: list[str] = [PROLOGUE]
        self.firsttime = True
        self.module = ""
        self.funcname = ""
        self.inputs: dict = {}
        self.inputattrs: dict = {}
        self.outputs: dict = {}
        self.outputattrs: dict = {}
        self.ninputs = self.noutputs = 0
        self.input_repeat: int | float = 0
        self.output_repeat: int | float = 0
        self.flags = ""
        self.outboard = False
        self.outboard_exec = ""
        self.outboard_host = ""

    def record(self, line: str) -> None:
        f = fields(line)
        if self.firsttime:
            if self.dynamic:
                self.out.append(DYNAMIC_ENTRY % (f[1] if len(f) > 1 else ""))
            else:
                self.out.append("void\n _dxf_user_modules()\n")
            self.out.append("{\n")
            self.firsttime = False

        if line.startswith("#"):
            return
        if line.startswith("MODULE"):
            if self.module != "":
                self.emit(last=False)
            self.ninputs = self.noutputs = 0
            self.input_repeat = self.output_repeat = 0
            self.flags = ""
            self.outboard = False
            self.outboard_exec = self.outboard_host = ""
            name = f[1] if len(f) > 1 else ""
            self.module = name
            self.funcname = "m_" + name
        elif line.startswith("INPUT"):
            name, attrs = self.parameter(line)
            self.inputs[self.ninputs] = name
            self.inputattrs[self.ninputs] = attrs
            self.ninputs += 1
        elif line.startswith("OUTPUT"):
            name, attrs = self.parameter(line)
            self.outputs[self.noutputs] = name
            self.outputattrs[self.noutputs] = attrs
            self.noutputs += 1
        elif line.startswith("FLAGS"):
            for flag in f[1:]:
                self.add_flag("MODULE_" + flag)
        elif line.startswith("OUTBOARD"):
            self.outboard_line(line)
        elif line.startswith("REPEAT"):
            count = awk_number(f[1]) if len(f) >= 2 else 1
            if self.noutputs == 0:
                self.input_repeat = count
            else:
                self.output_repeat = count

    @staticmethod
    def parameter(line: str) -> tuple[str, str]:
        a = split_literal(line, ";")
        b = fields(a[0] if a else "")
        c = "".join(b[1:])
        d = split_literal(c, "[")
        name = d[0] if d else ""
        attrs = d[1] if len(d) > 1 else ""
        if attrs != "":
            attrs = "[" + attrs
        return name, attrs

    def add_flag(self, flag: str) -> None:
        self.flags = f"{self.flags} | {flag}" if self.flags != "" else flag

    def outboard_line(self, line: str) -> None:
        self.outboard = True
        self.funcname = "DXOutboard"
        self.add_flag("MODULE_OUTBOARD")
        a = split_literal(line, ";")
        if len(a) > 1:
            b = fields(a[1])
            self.outboard_host = b[0] if b else ""
        a1 = a[0] if a else ""
        if QUOTED.search(a1):
            b = split_literal(a1, '"')
            ns = len(b)
            exe = b[1] if ns > 1 else ""
            for i in range(2, ns - 1):
                exe = exe + '"' + b[i]
            self.outboard_exec = exe
        else:
            b = fields(a1)
            self.outboard_exec = b[1] if len(b) > 1 else ""

    def emit(self, last: bool) -> None:
        w = self.out.append
        w("    {\n")
        if last:
            w("#ifndef __cplusplus\n")
            w(f"        extern Error {self.funcname}(Object *, Object *);\n")
            w("#endif\n")
        else:
            w("#if defined(__cplusplus)\n")
            w(f'        extern "C" Error {self.funcname}(Object *, Object *);\n')
            w("#else\n")
            w(f"        extern Error {self.funcname}(Object *, Object *);\n")
            w("#endif\n")
        w(f'        DXAddModule("{self.module}", {self.funcname}, ')
        w("0" if self.flags == "" else f"\n            {self.flags}")
        self.emit_params(self.ninputs, self.input_repeat,
                         self.inputs, self.inputattrs)
        self.emit_params(self.noutputs, self.output_repeat,
                         self.outputs, self.outputattrs)
        if self.outboard:
            w(f',\n            "{self.outboard_exec}", "{self.outboard_host}"')
        w(");\n")
        w("    }\n")

    def emit_params(self, n: int, repeat: int | float,
                    names: dict, attrs: dict) -> None:
        w = self.out.append
        w(",\n            " + awk_d(n + repeat * 20))
        for i in range(n):
            w(f', "{names.get(i, "")}{attrs.get(i, "")}"')
        for i in range(20):
            j = 0
            while j < repeat:
                k = n - repeat + j
                w(f', "{names.get(k, "")}{i + 1}{attrs.get(k, "")}"')
                j += 1

    def finish(self) -> str:
        if self.module != "":
            self.emit(last=True)
        self.out.append("}\n")
        return "".join(self.out)


def convert(text: str, dynamic: bool = False) -> str:
    lines = text.split("\n")
    if lines[-1] == "":
        lines.pop()
    conv = Mdf2C(dynamic)
    for line in lines:
        conv.record(line)
    return conv.finish()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Generate the module-registration C file from a .mdf.")
    ap.add_argument("mdf", nargs="?", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--dynamic", action="store_true")
    args = ap.parse_args(argv)

    data = args.mdf.read_bytes() if args.mdf else sys.stdin.buffer.read()
    result = convert(data.decode("latin-1"), args.dynamic).encode("latin-1")
    if args.output:
        args.output.write_bytes(result)
    else:
        sys.stdout.buffer.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())

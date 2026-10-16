#!/usr/bin/env python3.11
"""Preprocess dxmdf.src into dx.mdf or dxcm.mdf.

    mdf_preprocess.py --mode exec|cm (--cpp CMD | --cpp-arg ARG ...)
                      [-I DIR ...] [-D NAME[=VAL] ...] [-U NAME ...]
                      [--source-name NAME] [-o OUT] SOURCE

Replaces the dx.mdf / dxcm.mdf rules of src/exec/dxmods/local.mk.in.

--mode exec  dx.mdf: adds -DCPLUSPLUS_UI -DEXECUTIVE_MODS after the given
             flags and drops every line starting with '#'
--mode cm    dxcm.mdf: the preprocessor output unchanged, line markers and
             all (mdf2c skips them)
--cpp        the preprocessor command, split shell-style (e.g. "gcc -E")
--cpp-arg    one word of the command; repeat it instead of --cpp to keep
             words containing spaces intact (write --cpp-arg=-E for words
             starting with '-')
-I -D -U     passed through in the order given, before the mode's defines
--source-name
             the file name shown in line markers (default tmp.c, what the
             Makefile copied the source to)
-o           output file (default stdout)

The source is fed on stdin with `-x c -` rather than copied to tmp.c, so two
modes can run in parallel in one directory; "<stdin>" in line markers is then
renamed to --source-name. With the autotools flags and the same working
directory this gives byte-identical dx.mdf and dxcm.mdf. Relative -I paths are
resolved, and shown in line markers, relative to the working directory, and
`#include "..."` in the source searches the working directory first, as it did
for tmp.c in the build directory.

The Makefile also ran dx.mdf through a sed script meant to collapse runs of
blank lines, /^ *$/{ N; /^ *\\n *$/D }, but wrote it with `echo '...\\\\n...'`;
bash's echo keeps both backslashes, so the D test looks for a literal "\\n"
and never fires, and the script copies its input unchanged. The golden dx.mdf
keeps every blank line, so no squeezing is done here.

Only GCC/Clang (`cc -E`, `cpp`) are handled. MSVC (`cl /nologo /E`) would
differ in: no stdin input (write a temporary .c file and pass its path);
`-x c -` replaced by `/TC <file>`; /I and /D spelling for the flags; line
markers written as `#line N "path"` with the path as cl saw it (so the
--source-name rename would key on the temporary path); output to stdout with
the file name echoed to stderr. The exec-mode filtering is unchanged, since
both marker styles start with '#'. Needs Python 3.11+.
"""
from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

EXEC_DEFINES = ["-DCPLUSPLUS_UI", "-DEXECUTIVE_MODS"]
STDIN_MARKER = re.compile(r'^(# \d+ )"<stdin>"', re.MULTILINE)


def run_cpp(cpp: list[str], flags: list[str], source: bytes,
            source_name: str) -> str:
    cmd = [*cpp, *flags, "-x", "c", "-"]
    proc = subprocess.run(cmd, input=source, capture_output=True)
    sys.stderr.buffer.write(proc.stderr)
    if proc.returncode != 0:
        raise SystemExit(f"mdf_preprocess: {shlex.join(cmd)} "
                         f"failed ({proc.returncode})")
    text = proc.stdout.decode("latin-1")
    name = source_name.replace("\\", "\\\\").replace('"', '\\"')
    return STDIN_MARKER.sub(lambda m: f'{m.group(1)}"{name}"', text)


def filter_exec(text: str) -> str:
    return "".join(s for s in re.findall(r"[^\n]*\n|[^\n]+$", text)
                   if not s.startswith("#"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Preprocess dxmdf.src into dx.mdf or dxcm.mdf.")
    ap.add_argument("source", type=Path)
    ap.add_argument("--mode", choices=("exec", "cm"), required=True)
    ap.add_argument("--cpp")
    ap.add_argument("--cpp-arg", action="append", default=[])
    ap.add_argument("-I", dest="flags", action="append",
                    type=lambda v: "-I" + v)
    ap.add_argument("-D", dest="flags", action="append",
                    type=lambda v: "-D" + v)
    ap.add_argument("-U", dest="flags", action="append",
                    type=lambda v: "-U" + v)
    ap.add_argument("--source-name", default="tmp.c")
    ap.add_argument("-o", "--output", type=Path)
    args = ap.parse_args(argv)

    cpp = (shlex.split(args.cpp) if args.cpp else []) + args.cpp_arg
    if not cpp:
        ap.error("give the preprocessor with --cpp or --cpp-arg")
    flags = list(args.flags or [])
    if args.mode == "exec":
        flags += EXEC_DEFINES

    text = run_cpp(cpp, flags, args.source.read_bytes(), args.source_name)
    if args.mode == "exec":
        text = filter_exec(text)

    data = text.encode("latin-1")
    if args.output:
        args.output.write_bytes(data)
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3.11
"""Expand one of libdx's macro-template sources (*.Z) into C.

    zexpand.py NAME --srcdir DIR -o OUT [--cpp "cc -E"] [--work DIR]

NAME is buffer, lbshade, plane, triangle or quad. The .Z files are C written
as preprocessor macros, with `$` marking where a line break belongs in the
output. Each is run through the C preprocessor (once, or three times for
triangle and quad with -DPASS1/2/3), `$` becomes a newline, and placeholder
tokens such as INCLUDE2 and DEFINE3 become real directives that survive into
the generated file. This reproduces the rules in src/exec/libdx/local.mk.in.

--cpp   the preprocessor command (default "cc -E"); it must write to stdout
--work  folder for the intermediate files (default: the folder of OUT)

The squeeze of newlines matches `tr -s "\\044" "\\012"`: every run of newlines
collapses to one, so the output has no blank lines. Needs Python 3.11+.
"""
from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

WS_BEFORE_DOLLAR = re.compile(r"[ \t][ \t]*\$")


def squeeze(text: str) -> str:
    """tr -s '$' '\\n': translate, then squeeze runs of newlines."""
    return re.sub(r"\n+", "\n", text.replace("$", "\n"))


def sed(text: str, *subs: tuple[str, str], drop_hash: bool = True) -> str:
    """Delete lines starting with '#', then apply first-match substitutions."""
    out = []
    for line in text.splitlines(keepends=True):
        if drop_hash and line.startswith("#"):
            continue
        for pattern, repl in subs:
            line = re.sub(pattern, repl, line, count=1)
        out.append(line)
    return "".join(out)


def strip_ws(text: str) -> str:
    """sed 's/[ \\t][ \\t]*\\$/ $/': one space before the first `$` per line."""
    return "".join(WS_BEFORE_DOLLAR.sub(" $", line, count=1)
                   for line in text.splitlines(keepends=True))


def cpp(cmd: list[str], path: Path, *args: str) -> str:
    proc = subprocess.run([*cmd, *args, str(path)], capture_output=True,
                          text=True, cwd=path.parent)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"preprocessor failed on {path}")
    return proc.stdout


def expand(name: str, srcdir: Path, work: Path, cmd: list[str]) -> str:
    source = (srcdir / f"{name}.Z").read_text()
    inc = f"-I{srcdir}"
    pragma = (r"[^#]pragma", "#pragma")

    if name in ("buffer", "lbshade"):
        tmp = work / f"__{name}.c"
        tmp.write_text(strip_ws(source))
        text = squeeze(cpp(cmd, tmp))
        if name == "buffer":
            return sed(text, ("INCLUDE2", "#include"), (r"^pragma", "#pragma"))
        return sed(text, ("DEFINE2", "#define"), ("INCLUDE2", "#include"), pragma)

    if name == "plane":
        tmp = work / "_plane.c"
        tmp.write_text(source)
        return sed(squeeze(cpp(cmd, tmp)), ("INCLUDE2", "#include"), pragma)

    if name in ("triangle", "quad"):
        tmp = work / f"__{name}.c"
        tmp.write_text(strip_ws(source))
        p1 = work / f"{name}1.c"
        p1.write_text(squeeze(sed(cpp(cmd, tmp, inc, "-DPASS1"),
                                  ("INCLUDE1", "#include"))))
        p2 = work / f"{name}2.c"
        p2.write_text(squeeze(sed(cpp(cmd, p1, inc, "-DPASS2"),
                                  ("INCLUDE2", "#include"))))
        return sed(squeeze(cpp(cmd, p2, inc, "-DPASS3")),
                   ("DEFINE3", "#define"), ("INCLUDE3", "#include"), pragma)

    raise SystemExit(f"unknown template: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("name", choices=("buffer", "lbshade", "plane", "triangle", "quad"))
    parser.add_argument("--srcdir", type=Path, required=True)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--cpp", default="cc -E")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()

    work = (args.work or args.output.parent).resolve()
    work.mkdir(parents=True, exist_ok=True)
    text = expand(args.name, args.srcdir.resolve(), work, shlex.split(args.cpp))
    args.output.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

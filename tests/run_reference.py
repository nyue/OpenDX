#!/usr/bin/env python3.11
"""Render the OpenDX reference scripts and compare them with stored images.

    run_reference.py --dxroot DIR [--update] [--only NAME ...] [--out DIR]
                     [--tolerance N] [--max-fraction F] [--dxexec PATH]

Each script in tests/reference/scripts/ ends with
`WriteImage(img, "out", "rgb")`. The runner feeds it to `dxexec -R -B` in a
fresh empty folder (the "rgb" format appends frames to an existing file), reads
the raw pixels back, and compares them with tests/reference/images/<name>.png.

--dxroot     an installed DX tree (the folder holding bin_<arch>/ and fonts/);
             DXROOT from the environment is used when it is not given
--dxexec     the executive to run; default: <dxroot>/bin_*/dxexec
--update     write the rendered images as the new references
--only       run only the named scripts (file name without .script)
--out        where failing renders and diff images go
             (default: tests/reference/out, which git ignores)
--tolerance  largest per-channel difference counted as equal (default 0)
--max-fraction
             fraction of pixels allowed to differ beyond --tolerance
             (default 0.0)

Raw "rgb" output is used rather than TIFF because DX applies a gamma of 2.0
to TIFF; raw RGB is the renderer's own pixels. PNG reading and writing use only
the standard library (zlib), so nothing needs installing. Needs Python 3.11+.
Exits 0 when every script matches, 1 otherwise.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
REF = HERE / "reference"
SCRIPTS = REF / "scripts"
IMAGES = REF / "images"
DATA = REF / "data"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


@dataclass
class Image:
    width: int
    height: int
    rgb: bytes  # top row first, 3 bytes per pixel


# --- PNG (8-bit RGB, non-interlaced) ---------------------------------------

def _chunk(kind: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(kind + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def write_png(path: Path, img: Image) -> None:
    stride = img.width * 3
    raw = b"".join(b"\x00" + img.rgb[y * stride:(y + 1) * stride]
                   for y in range(img.height))
    header = struct.pack(">IIBBBBB", img.width, img.height, 8, 2, 0, 0, 0)
    path.write_bytes(PNG_SIGNATURE + _chunk(b"IHDR", header)
                     + _chunk(b"IDAT", zlib.compress(raw, 9))
                     + _chunk(b"IEND", b""))


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path: Path) -> Image:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(f"{path}: not a PNG file")
    pos, idat, header = len(PNG_SIGNATURE), b"", None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ValueError(f"{path}: no IHDR chunk")
    width, height, depth, colour, _, _, interlace = header
    if (depth, colour, interlace) != (8, 2, 0):
        raise ValueError(f"{path}: only 8-bit RGB, non-interlaced PNG is supported")
    raw, stride, bpp = zlib.decompress(idat), width * 3, 3
    out, prev, pos = bytearray(), bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up_left = prev[i - bpp] if i >= bpp else 0
            pred = (0, left, prev[i], (left + prev[i]) // 2,
                    _paeth(left, prev[i], up_left))[kind]
            line[i] = (line[i] + pred) & 0xFF
        out += line
        prev = line
    return Image(width, height, bytes(out))


# --- Rendering ---------------------------------------------------------------

def find_dxexec(dxroot: Path) -> Path:
    names = ("dxexec.exe", "dxexec") if os.name == "nt" else ("dxexec",)
    for name in names:
        found = sorted(dxroot.glob(f"bin_*/{name}"))
        if found:
            return found[0]
    raise SystemExit(f"no bin_*/dxexec under {dxroot}; pass --dxexec")


def render(script: Path, dxexec: Path, dxroot: Path) -> tuple[Image | None, str]:
    """Run one script; return the image (or None) and the executive's output."""
    env = dict(os.environ, DXROOT=str(dxroot), DXDATA=str(DATA))
    with tempfile.TemporaryDirectory(prefix="dxref-") as work:
        with script.open("rb") as stdin:
            try:
                proc = subprocess.run([str(dxexec), "-R", "-B"], stdin=stdin,
                                      cwd=work, env=env, capture_output=True,
                                      timeout=300)
            except subprocess.TimeoutExpired:
                return None, "timed out after 300 s"
        log = (proc.stdout + proc.stderr).decode(errors="replace")
        if proc.returncode != 0:
            return None, f"{log}\nexit status {proc.returncode}"
        if "ERROR" in log:
            return None, log
        size_file, rgb_file = Path(work, "out.size"), Path(work, "out.rgb")
        if not (size_file.exists() and rgb_file.exists()):
            return None, f"{log}\nno out.rgb / out.size written"
        width, height, frames = (int(n) for n in
                                 size_file.read_text().strip().split("x"))
        rgb = rgb_file.read_bytes()
        if frames != 1 or len(rgb) != width * height * 3:
            return None, f"{log}\nunexpected output: {width}x{height}x{frames}"
        return Image(width, height, rgb), log


# --- Comparison --------------------------------------------------------------

def compare(actual: Image, expected: Image, tolerance: int) -> tuple[int, int, Image]:
    """Return (pixels over tolerance, largest channel difference, diff image)."""
    a, e = actual.rgb, expected.rgb
    bad, worst, diff = 0, 0, bytearray(len(a))
    for p in range(0, len(a), 3):
        d = max(abs(a[p] - e[p]), abs(a[p + 1] - e[p + 1]), abs(a[p + 2] - e[p + 2]))
        worst = max(worst, d)
        if d > tolerance:
            bad += 1
            # Protan-safe highlight: orange on a dimmed blue-grey copy.
            diff[p:p + 3] = (255, 140, 0)
        else:
            grey = a[p] // 4
            diff[p:p + 3] = (grey, grey, min(255, grey + 24))
    return bad, worst, Image(actual.width, actual.height, bytes(diff))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dxroot", type=Path, default=os.environ.get("DXROOT"))
    parser.add_argument("--dxexec", type=Path)
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--only", nargs="+", metavar="NAME")
    parser.add_argument("--out", type=Path, default=REF / "out")
    parser.add_argument("--tolerance", type=int, default=0)
    parser.add_argument("--max-fraction", type=float, default=0.0)
    args = parser.parse_args()

    if sys.version_info < (3, 11):
        parser.error("needs Python 3.11 or newer")
    if args.dxroot is None:
        parser.error("pass --dxroot or set DXROOT")
    dxroot = args.dxroot.resolve()
    dxexec = (args.dxexec or find_dxexec(dxroot)).resolve()

    scripts = sorted(SCRIPTS.glob("*.script"))
    if args.only:
        wanted = set(args.only)
        scripts = [s for s in scripts if s.stem in wanted]
        missing = wanted - {s.stem for s in scripts}
        if missing:
            parser.error(f"no such script: {', '.join(sorted(missing))}")

    failures = 0
    for script in scripts:
        name = script.stem
        image, log = render(script, dxexec, dxroot)
        if image is None:
            failures += 1
            print(f"FAIL   {name}: render failed")
            print("       " + "\n       ".join(log.strip().splitlines()[-8:]))
            continue
        ref_path = IMAGES / f"{name}.png"
        if args.update:
            IMAGES.mkdir(parents=True, exist_ok=True)
            write_png(ref_path, image)
            print(f"UPDATE {name}: {image.width}x{image.height}")
            continue
        if not ref_path.exists():
            failures += 1
            print(f"FAIL   {name}: no reference image (run with --update)")
            continue
        expected = read_png(ref_path)
        if (expected.width, expected.height) != (image.width, image.height):
            failures += 1
            print(f"FAIL   {name}: size {image.width}x{image.height}, "
                  f"expected {expected.width}x{expected.height}")
            continue
        bad, worst, diff = compare(image, expected, args.tolerance)
        fraction = bad / (image.width * image.height)
        if fraction > args.max_fraction:
            failures += 1
            args.out.mkdir(parents=True, exist_ok=True)
            write_png(args.out / f"{name}.actual.png", image)
            write_png(args.out / f"{name}.diff.png", diff)
            print(f"FAIL   {name}: {bad} pixels differ ({fraction:.3%}), "
                  f"largest channel difference {worst}; see {args.out}")
        else:
            print(f"ok     {name}" + (f" ({bad} pixels within allowance)" if bad else ""))

    total = len(scripts)
    print(f"\n{total - failures} of {total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

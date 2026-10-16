# 2. CMake build for the executive, with Python code generators

- Status: Accepted
- Date: 2026-10-15
- Deciders: Nicholas Yue
- Discussion: [diary 2026-10-15](../diary/2026-10-15.md), *Phase 1*

## Context

OpenDX builds with autotools (Autoconf 2.59 output committed) and relies on
shell, `sed`, `tr`, `awk` and the C preprocessor to generate code. None of that
runs natively on Windows, and the committed generated files hid whether the
generators still worked. Phase 0 established a known-good baseline (`8b6f39f`)
and a reference suite (`tests/reference/`) that renders byte-for-byte
repeatable images.

## Decision

1. **CMake (3.20 or newer) builds the executive** (`src/exec` → `dxexec`). The
   autotools build stays for the UI (`src/uipp`) until the UI is replaced
   (ADR 0001); both builds work from the same tree.
2. **Code generators are Python 3.11 scripts** in `tools/codegen/`, standard
   library only, each a port of the original shell/awk rule and checked
   byte-for-byte against its output:
   - `dx_class.py`: `libdx/*.X` → class headers and sources (`class`, `class.awk`)
   - `zexpand.py`: `libdx/*.Z` macro templates → C (`local.mk.in` rules)
   - `mdf_preprocess.py`: `dxmods/dxmdf.src` → `dx.mdf`
   - `mdf2c.py`: `dx.mdf` → `user.c`, the module table (`lib/mdf2c.awk`)
   The C preprocessor is the compiler CMake found, passed in by the build.
3. **Generated files are not committed.** They are produced in the build tree.
   (Autotools regenerates its own copies, so its build is unaffected.)
4. **Each library is a CMake object library** (`dx_libdx`, `dx_dpexec`,
   `dx_dxmods`, `dx_hwrender`) linked straight into `dxexec`. The libraries call
   each other in a cycle, which static libraries handle badly; object libraries
   also make phase 2 (the executive as a library) a small step.
5. **C dialect pinned to gnu17**, which GCC 11 used by default. Newer compilers
   default to C23 (`bool` keyword, implicit declarations are errors).
6. **Install layout matches autotools** (`<prefix>/dx/bin_<arch>/dxexec`,
   `dx/lib`, `dx/fonts`), so `DXROOT=<prefix>/dx` works for both builds.
7. **`dxconfig.h` is generated from `cmake/dxconfig.h.cmake`**, covering the 81
   settings the executive reads, with the autotools names.

## Consequences

- Python 3.11 is a build-time dependency on every platform.
- Linux only so far: the CMake files stop with an error elsewhere. macOS and
  Windows need the X11/GLX code (`hwrender`, `libdx` display code) made
  optional first.
- X11 and OpenGL are still required, as in the autotools build.
- The `.Z` and MDF preprocessing assume a GCC/Clang-style `-E`; MSVC needs the
  differences listed in `mdf_preprocess.py`.
- The two builds can drift. The reference suite is the check that they render
  the same.

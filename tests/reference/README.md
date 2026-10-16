# Reference renders

Headless DX scripts with known-good images, used to check that each
modernisation step still renders the same pictures. Each script makes its own
data (or reads a small file from `data/`, found through `DXDATA`) and ends with
`WriteImage(img, "out", "rgb")`.

| Folder | Contents |
|---|---|
| `scripts/` | one `.script` per feature area, run by `dxexec -R -B` |
| `images/` | the reference PNGs, 320×240, one per script |
| `data/` | input files the scripts import |
| `out/` | failing renders and diff images (not committed) |

## Running

```sh
python3.11 tests/run_reference.py --dxroot <install>/dx
```

`--dxroot` is the installed DX tree (it holds `bin_<arch>/dxexec` and
`fonts/`). Add `--only NAME ...` for a subset and `--update` to rewrite the
references. A failure writes `<name>.actual.png` and `<name>.diff.png` (differing
pixels in orange) to `out/`. `--tolerance` and `--max-fraction` loosen the
comparison; both default to an exact match.

The DX script language does not allow a module call inside another call's
arguments: give each step its own variable.

## How the references were made

Built from the 4.4.4 baseline (`8b6f39f` plus the `linux/sys.h` fix) on
Rocky Linux 9.7, GCC 11.5, with the committed autotools files:

```sh
./configure --prefix=<install> CXXFLAGS="-g -O2 -std=gnu++14 -Wno-narrowing -fpermissive"
make -j && make install
```

Rendering uses DX's software renderer and is byte-for-byte repeatable, so any
difference is a change in behaviour. Expect that to loosen once the compiler or
platform changes (phase 1); raise `--tolerance` then rather than regenerating
the references, so a real change still shows.

The CMake build of `dxexec` (phase 1) reproduces these references byte for
byte:

```sh
cmake -S . -B <build> -G Ninja -DCMAKE_INSTALL_PREFIX=<install>
cmake --build <build> && cmake --install <build>
python3.11 tests/run_reference.py --dxroot <install>/dx
```

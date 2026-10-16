# 3. GPU rendering: the executive exports scenes, the UI draws them

- Status: Accepted
- Date: 2026-10-16
- Deciders: Nicholas Yue
- Discussion: [diary 2026-10-16](../diary/2026-10-16.md), *Phase 3: the new renderer*

## Context

The new UI (ADR 0001) shows pictures that the executive renders in software and
writes to files; mouse interaction re-runs `main()` for every camera step. That
is fast enough for small programs (a few milliseconds per step) but grows with
the scene, leaves the GPU unused, and advances stateful modules (GetGlobal,
SetGlobal) on every step. The legacy fast path, the executive's OpenGL renderer
drawing into its own X window, does not fit the UI, containers or WASM.

Three ways to get the scene to a GPU renderer in the UI were weighed:

1. the executive flattens the object an Image node renders into drawable meshes
   and writes them; the UI draws them;
2. the UI reads DX's own object format (`Export` to `.dx`) and flattens it
   itself;
3. the UI links the executive as a library and walks DX objects in memory.

## Decision

**Option 1.** A new executive module, `DxuiScene`, flattens the object (groups,
transforms, fields; connections split into triangles, segments or points;
colours, normals and opacities resolved per vertex; shading values; the lights
DX would use, defaults included) into a small binary scene file. The format is
defined in `include/dxuiscene.h`, shared by the module (C) and the UI (C++).
The UI's rewrite of the `macro Image` wrappers calls it next to the camera
report, so every run produces `<id>.scene` beside `<id>.rgb`.

The UI draws scenes with OpenGL 3.3 core-level shaders (the GLES 3.0 / WebGL2
subset), and moves the camera itself: rotating and zooming re-render on the GPU
without running the network. The executive is involved only when the network
runs.

## Consequences

- Smooth interaction on the GPU, and no re-execution for camera moves.
- The flattening reuses the semantics of the legacy hardware renderer
  (`hwGather.c`, `hwXfield.c`) and the software renderer's defaults, in C next
  to the data, rather than duplicating them in the UI.
- First slice: opaque surfaces, lines and points with DX lighting. Not drawn
  yet, and counted in the scene file so the UI can say so: screen-space
  annotation (captions, colour bars), volume rendering, textures, clipping.
  For those the software-rendered picture remains, with a switch between the
  two views.
- The scene file is a new interface between the executive and the UI; it is
  versioned by its magic (`DXSCENE1`).
- The same file is what a WASM build would load, so the renderer carries over.

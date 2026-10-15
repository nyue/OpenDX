# 1. UI toolkit: Dear ImGui and noodles, not Qt or Motif

- Status: Accepted
- Date: 2026-10-15
- Deciders: Nicholas Yue
- Discussion: [diary 2026-10-15](../diary/2026-10-15.md), *Modernisation: UI toolkit*

## Context

OpenDX's user interface (`src/uipp`, about 1000 C++ files) is built on X11 and
Motif: 284 files include `Xm/`, `uipp/base` is a home-grown component framework
wrapping Motif, and `uipp/widgets` holds custom Xt widgets, including the visual
program canvas (`WorkspaceW.c`) and the colormap editor. Motif ties the UI to X11
and rules out macOS, Windows, containers without X, and the browser.

The targets are Linux, macOS and Windows desktops, a container image, and a
WebAssembly build. Qt 6 was the first candidate. It was set aside to avoid its
licensing questions: OpenDX is under the IBM Public License, and Qt is LGPL, and
a WASM build links Qt statically, which brings LGPL relinking obligations.

The author has already proven a permissively licensed stack on all three targets
in other projects:

- **Remex** (`gitlab.com/nyue/remex-polygonal-modeller`): GLFW + OpenGL 3.3 core
  + Dear ImGui on the desktop, a WebGL2 build under Emscripten, and an
  application container that draws on the host display (Wayland first, X11
  fallback; Windows 11 through Podman and WSLg). See its `container/` folder and
  diary 2026-10-11.
- **Meta-Noodles research** (`gitlab.com/meta-noodles/research` at `969f8b0`):
  `noodles`, a C++17 OpenGL node-graph renderer from Meta, ported to WebGL2
  (`docs/wasm-port-assessment.md`). **shader-noodles** builds a browser-based
  graph editor on it and ships as a container.

noodles draws only the graph canvas: nodes, links, labels, hit-testing, undo. It
has no windows, menus, dialogs or text input, so it cannot replace a widget
toolkit on its own.

## Decision

1. **No Qt and no Motif.** The new UI uses only permissively licensed libraries.
2. **Dear ImGui** (docking branch) for every part of the UI except the graph
   canvas: the menus, the module palette, configuration dialogs, control panels
   and interactors, image windows, the sequencer, and the message window.
   OpenDX's many top-level windows become docked panels.
3. **noodles for the visual program canvas**, replacing `WorkspaceW.c` and the
   widgets around it. Use the author's fork, `github.com/nyue/noodles`, branch
   `wasm-port` (pinned at `13e27ed`, 2026-08-05), not upstream
   `facebookexperimental/noodles`: upstream does not build for WebGL2.
4. **GLFW** for windows, input and the GL context, as in Remex.
5. **One GL context** shared by ImGui, noodles and DX's own image window.
6. **The UI–exec boundary stays the DX protocol** that `dxui` and `dxexec`
   already speak. A browser (HTML) UI must remain possible later without changes
   to the exec.

## Consequences

- Licences: Dear ImGui (MIT), noodles (MIT; bundled fonts under SIL OFL 1.1),
  GLFW (zlib). None of them restricts static linking, so the WASM build carries
  no licence question beyond shipping notices.
- The UI is a **rewrite of the view layer**, not a translation. ImGui is
  immediate-mode; `uipp/base` is retained-mode Motif with callbacks, and does not
  port. The model classes can be partly kept: 45 of the 164 `.C` files in
  `dxuilib` have no X or Motif references, but `Network.C` has 14, so they must
  be separated from the view first.
- ImGui is weaker than a native toolkit at text editing, IME and accessibility,
  and has no native look. This is accepted for a visualization tool.
- File dialogs need two implementations: native on the desktop, the browser
  picker and downloads on the web (see Remex's parked *Web/WASM file I/O*).
- The renderer must be OpenGL 3.3 core / OpenGL ES 3.0 level so that ImGui,
  noodles and DX's image window share one context on the desktop and in WebGL2.
  This makes rewriting the fixed-function code in `src/exec/hwrender` (GL 1.x,
  GLX) a required step.
- Depending on a fork brings drift risk: the fork will move away from upstream,
  and noodles' API is experimental. The research repo already plans a review of
  that drift in December 2026.
- The container image can reuse Remex's `container/` tooling (multi-stage
  `Containerfile`, `remex_run.py` launcher, `notices.py`, `release.py`).

// Turns a .net file into the script the UI sends to dxexec.
//
// In exec-only mode each Image node runs the `macro Image(...)` the .net file
// carries, which ends in Display() calls that open an X window of the
// executive's own. The UI rewrites those calls, inside the Image macros only,
// to DxuiDisplay(id, ...): a macro defined here that renders when given a
// camera and writes the picture to <imageDir>/<id>.rgb (+ .size), where the
// UI picks it up. Each file keeps its own camera and axes logic, whatever DX
// version wrote it.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dxui {

struct NetScript {
    std::string text;         // DxuiDisplay definition + the rewritten file
    int displayCallsRewritten = 0;
};

// netText is the .net file's contents; imageDir is where images go.
NetScript netScriptForUi(std::string_view netText, const std::filesystem::path& imageDir);

} // namespace dxui

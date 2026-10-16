// The Image wrapper macro for exec-only runs of a .net file; see NetImageMacro.cpp.

#pragma once

#include <string_view>

namespace dxui {

// `macro Image(id, object, ..., webOptions) -> (object, camera, where) {...}`,
// 49 parameters, ending in "}\n".
std::string_view legacyImageMacroText();

} // namespace dxui

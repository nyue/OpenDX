// Writes node positions back into a .net file.
//
// Only the `x = X, y = Y` numbers of each node's `// node Module[inst]:` comment
// line change; every other byte of the original text is kept, so the file stays
// loadable by DX and by the legacy UI. A node comment that is not where the
// parser recorded it means the text and the model disagree: nothing is written.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dxui {

struct NetMacro;

// Rewrites `original` with the positions of `macro`'s nodes (rounded to whole
// workspace pixels). Returns false with *error set if a node's comment line
// cannot be found or does not name that node.
bool writeNodePositions(std::string_view original, const NetMacro& macro, std::string& out,
                        std::string* error = nullptr);

// Writes `text` to `path` without risking a half-written file: a temporary file
// beside it, then a rename over it, keeping the original's permissions.
bool saveTextAtomically(const std::filesystem::path& path, std::string_view text,
                        std::string* error = nullptr);

} // namespace dxui

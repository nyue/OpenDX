// Writes .net files back out after the UI has changed the network.
//
// Two writers, both keeping every byte of the original text that the change
// does not touch, so the file stays loadable by DX and by the legacy UI:
//
// writeNodePositions -- only the `x = X, y = Y` numbers of each node's
//   `// node Module[inst]:` comment line change. Enough after moves.
//
// writeNet -- structural changes: nodes added or deleted, wires connected or
//   disconnected, input values set or cleared, nodes moved. The macro body is
//   a sequence of node blocks (the `// node` comment, its input/output/other
//   comment lines, the call); after the macro come the input value assignments
//   (`main_Mod_1_in_2 = value;`), any `macro Image(...)` wrappers and the
//   footer (`Executive(...)`, `$sync`, `main();`). On write:
//   - an unchanged node keeps its block and its value assignments verbatim; a
//     node that only moved keeps them with x/y patched;
//   - a changed node (wires into it, input values, input count, label) gets its
//     block regenerated in the legacy UI's format (src/uipp/dxui/Node.C
//     netPrintCommentHeader / netNodeString): comment lines that still hold are
//     reused verbatim, the call is rewritten with arguments naming the source
//     output (`main_Import_1_out_1`), its own input variable
//     (`main_Isosurface_2_in_3`) or -- in macros, where the legacy UI writes
//     values inline -- the value itself; its `_in_k = ...;` assignments are
//     regenerated in place (NULL for an input left to default);
//   - a new node gets a block in that format, its assignments are appended to
//     the values section; a deleted node vanishes with its assignments;
//   - blocks are re-ordered only as much as wiring requires (each node after
//     the nodes it reads, Receivers after their Transmitter);
//   - a new Image node gets its window id (input 1 = "Image_N", as the legacy
//     ImageNode::initialize sets it), its `CacheScene(...)` line, and -- when
//     the file has no `macro Image` wrapper, or only an older one with fewer
//     parameters than the node has inputs -- the current wrapper
//     (NetImageMacro.cpp) in the values section.
//
//   Refused (returns false, *error says which node and why), because the
//   legacy format for them needs state the model does not carry:
//   - adding interactors and other UI-state nodes: Scalar, Integer, Vector and
//     their List forms, Selector, SelectorList, Toggle, Value, ValueList,
//     String, StringList, FileSelector, Colormap, Sequencer, Reset, Probe,
//     ProbeList, Pick, DXLInput, DXLOutput;
//   - changing the wires into or input values of those nodes (this includes
//     deleting or disconnecting the node that drives a data-driven
//     interactor);
//   - a wire that closes a loop through a Transmitter/Receiver pair
//     (NetEdit::connect only checks wires);
//   - adding Transmitter or Receiver nodes (no label editing yet); adding or
//     deleting Input/Output nodes (that changes the macro's signature);
//   - changing a node whose block is not the legacy shape (comment lines, at
//     most one statement, nothing else), or changes to macro-level data
//     (name, parameters, comments, workspace, decorators);
//   - files whose network is not their only macro, or whose macro body cannot
//     be located in the text.
//   Not done (written files still run): no `include` lines for added macro
//   nodes (the executive finds them on DXMACROS), no page group for added
//   nodes, and a Compute's `// expression:` comment is not updated when its
//   input 1 value is set.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dxui {

struct NetMacro;
struct NetFile;
class ModuleLibrary;

// Rewrites `original` with the positions of `macro`'s nodes (rounded to whole
// workspace pixels). Returns false with *error set if a node's comment line
// cannot be found or does not name that node.
bool writeNodePositions(std::string_view original, const NetMacro& macro, std::string& out,
                        std::string* error = nullptr);

// Writes `edited` as .net text, reusing `originalText` (parsed as `original`)
// for everything the edits leave alone. `edited` is `original` after NetEdit
// operations and moves: a node is identified by NetNode::line (the line of its
// node comment in originalText; 0 for an added node) together with
// module/instance. `lib` gives added nodes' module definitions (input count of
// an Image for its wrapper, Compute's input names); may be null.
// With edited == original the result is originalText byte for byte.
// Returns false with *error set when the edit cannot be written (see above);
// `out` is then unspecified.
bool writeNet(std::string_view originalText, const NetFile& original, const NetFile& edited,
              const ModuleLibrary* lib, std::string& out, std::string* error = nullptr);

// Writes `text` to `path` without risking a half-written file: a temporary file
// beside it, then a rename over it, keeping the original's permissions.
bool saveTextAtomically(const std::filesystem::path& path, std::string_view text,
                        std::string* error = nullptr);

} // namespace dxui

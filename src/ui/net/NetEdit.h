// Edits on a network model (NetMacro): adding and deleting nodes, wiring,
// and setting input values. Each returns false with *why set when the edit
// is not allowed, and leaves the macro unchanged.
//
// DX rules enforced: one wire per input (connecting to a wired input replaces
// its wire), no wire from a node to itself or that would close a loop (a
// network is acyclic). A new node has line == 0: NetWriter generates its text.

#pragma once

#include <string>

namespace dxui {

struct NetMacro;
struct ModuleDef;

// Adds a node of `def` at workspace (x, y); its instance number is one more
// than the highest of that module in the macro. Returns its index.
int addNode(NetMacro& macro, const ModuleDef& def, float x, float y);

// Removes the node and every wire to or from it; later indices shift down.
bool deleteNode(NetMacro& macro, int node, std::string* why = nullptr);

// Wires output `output` of `from` to input `input` of `to` (ports 1-based).
bool connect(NetMacro& macro, int from, int output, int to, int input,
             std::string* why = nullptr);

// Removes the wire into input `input` of `to`.
bool disconnect(NetMacro& macro, int to, int input, std::string* why = nullptr);

// Sets an input's value as DX script text (e.g. `"rainwater"`, `[0 0 1]`, `5`);
// an empty value returns the input to its default.
bool setInputValue(NetMacro& macro, int node, int input, const std::string& value,
                   std::string* why = nullptr);

// Whether input `input` of `node` is shown on the node, as DX decides: always
// when wired; else the file's `// input[i]: visible =` if it lists the input;
// else the definition's (visible:1); with no definition, every input of a node
// with at most 10 inputs. Internal inputs (visible:2) are never shown.
bool inputShown(const NetMacro& macro, int node, int input, const ModuleDef* def);

// Shows or hides an input on the node (the legacy UI's Expand/Collapse per
// input); written as `// input[i]: visible = 0|1`. A wired input cannot be
// hidden; an internal input (visible:2 in its definition) cannot be shown.
// `def` may be null (no definition known).
bool setInputVisible(NetMacro& macro, int node, int input, bool visible,
                     const ModuleDef* def, std::string* why = nullptr);

} // namespace dxui

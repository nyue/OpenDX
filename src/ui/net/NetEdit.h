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

} // namespace dxui

// Reads OpenDX visual programs (.net) into a plain model for the new UI.
//
// A .net file is DX script with the editor's state in comments: each node is a
// `// node Module[inst]: x = .., y = .., inputs = .., label = ..` block followed
// by the call that runs it. The network is the macro carrying those comments
// and the `// network: end of macro body` marker -- in every sample, the first
// and only such macro, main or the macro the file defines. What follows it is
// for the executive: generated `macro Image(...)` wrappers (dropped), input
// assignments (kept, see NetNode::assignedValues) and `main();`.
//
// Wires come from the call arguments: an argument naming another node's output
// variable (`main_Import_1_out_1`) is a link. The legacy reader in
// src/uipp/dxuilib/Network.C is the reference for these rules.
//
// Standard library only; the parser never throws and never trusts its input.

#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dxui {

// From a `// input[i]:` line (1-based index). A line giving only `visible`
// leaves the input defaulting, as the legacy reader does.
struct NetInput {
    int index = 0;
    bool visible = true;
    bool defaulting = true;
    int type = 0;           // DX type bit mask, 0 when absent
    std::string value;      // text after "value = ", trailing blanks removed
    bool hasValue = false;  // the line had a "value =" part
};

// From a `// output[i]:` line -- interactors record their current value here.
struct NetOutput {
    int index = 0;
    bool visible = true;
    int type = 0;
    std::string value;
    bool hasValue = false;
};

struct NetNode {
    std::string module;     // e.g. "Import"
    int instance = 0;       // the [inst] number
    std::string label;      // label = ...
    float x = 0, y = 0;     // workspace position
    // numInputs is the node comment's count when given, else the argument
    // count. numOutputs is `outputs =` when given (rare: only modules with
    // repeatable outputs write it); otherwise the highest output index seen in
    // the file -- lvalues, output[] lines, links. Without the module
    // definitions (MDF) that is a lower bound, e.g. an unused third output of
    // Image is invisible here unless its variable is written.
    int numInputs = 0, numOutputs = 0;
    bool inputsFromComment = false, outputsFromComment = false;
    std::vector<NetInput> inputs;            // only the ones listed in comments
    std::vector<NetOutput> outputs;          // only the ones listed in comments
    std::vector<std::string> extraComments;  // other per-node comment lines, verbatim
    std::string pageGroup;                   // from `// page group:`, also in extraComments
    // Values the file assigns to this node's input variables, keyed by
    // 1-based input (`main_Import_1_in_1 = "rainwater";` -> {1, "\"rainwater\""}).
    std::map<int, std::string> assignedValues;
    int line = 0;  // of the node comment
};

// Node indices into NetMacro::nodes; ports 1-based.
struct NetLink {
    std::size_t fromNode;
    int fromOutput;
    std::size_t toNode;
    int toInput;
};

// Transmitter/Receiver pairs connect by label, not by wire; they are not in
// NetMacro::links. Derived on request from the labels.
struct NetWireless {
    std::size_t transmitter;
    std::size_t receiver;
};

// A macro parameter: `name` or `name = default` in the macro signature.
struct NetFormal {
    std::string name;
    std::string defaultValue;  // source text, empty when absent
};

// A `// decorator` block and its `annotation` / `resource` lines, verbatim.
// Not interpreted yet.
struct NetDecorator {
    int line = 0;
    std::vector<std::string> comments;
};

struct NetMacro {
    std::string name;
    std::vector<NetNode> nodes;
    std::vector<NetLink> links;
    std::vector<std::string> comments;   // `// comment:` lines of this macro
    int workspaceWidth = 0, workspaceHeight = 0;

    std::vector<NetFormal> formalInputs, formalOutputs;
    std::string category, description;   // `// CATEGORY`, `// DESCRIPTION`
    std::vector<NetDecorator> decorators;
    // Header comments not modelled above (page assignment, layout, macro
    // reference, the MDF block, ...), verbatim.
    std::vector<std::string> headerComments;
    int line = 0;  // of the `macro` keyword

    std::vector<NetWireless> wirelessPairs() const;
    const NetNode* findNode(std::string_view module, int instance) const;
};

struct NetFile {
    std::string formatVersion, dxVersion;  // from the version line, may be empty
    std::vector<NetMacro> macros;          // in file order
    std::vector<std::string> includes;     // `include "file"` targets, unquoted, escapes kept
    // Things the reader tolerated: unresolved output references, unparseable
    // input lines, damage after the network. Each starts with "line N: ".
    std::vector<std::string> warnings;

    const NetMacro* findMacro(std::string_view name) const;
};

struct NetParseError {
    int line = 0;
    std::string message;
};

// Returns false and fills err on failure. Never throws on malformed input; never crashes.
bool parseNet(std::string_view text, NetFile& out, NetParseError* err = nullptr);
bool parseNetFile(const std::filesystem::path& path, NetFile& out, NetParseError* err = nullptr);

}  // namespace dxui

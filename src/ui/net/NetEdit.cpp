#include "NetEdit.h"
#include "ModuleDefs.h"
#include "NetFile.h"

#include <algorithm>
#include <vector>

namespace dxui {

namespace {

bool refuse(std::string* why, const char* message)
{
    if (why)
        *why = message;
    return false;
}

bool validNode(const NetMacro& m, int node)
{
    return node >= 0 && static_cast<std::size_t>(node) < m.nodes.size();
}

// True if `target` can be reached from `start` along wires, counting each
// Transmitter -> Receiver pair (matched by label) as a wire too.
bool reaches(const NetMacro& m, std::size_t start, std::size_t target)
{
    const std::vector<NetWireless> wireless = m.wirelessPairs();
    std::vector<bool> seen(m.nodes.size(), false);
    std::vector<std::size_t> todo = {start};
    while (!todo.empty()) {
        std::size_t n = todo.back();
        todo.pop_back();
        if (n == target)
            return true;
        if (n >= seen.size() || seen[n])
            continue;
        seen[n] = true;
        for (const NetLink& l : m.links)
            if (l.fromNode == n)
                todo.push_back(l.toNode);
        for (const NetWireless& w : wireless)
            if (w.transmitter == n)
                todo.push_back(w.receiver);
    }
    return false;
}

} // namespace

int addNode(NetMacro& macro, const ModuleDef& def, float x, float y)
{
    int instance = 0;
    for (const NetNode& n : macro.nodes)
        if (n.module == def.name)
            instance = std::max(instance, n.instance);
    NetNode node;
    node.module = def.name;
    node.instance = instance + 1;
    node.label = def.name;
    node.x = x;
    node.y = y;
    node.numInputs = static_cast<int>(def.inputs.size());
    node.numOutputs = static_cast<int>(def.outputs.size());
    node.line = 0;
    macro.nodes.push_back(std::move(node));
    return static_cast<int>(macro.nodes.size()) - 1;
}

bool deleteNode(NetMacro& macro, int node, std::string* why)
{
    if (!validNode(macro, node))
        return refuse(why, "no such node");
    const auto n = static_cast<std::size_t>(node);
    // A Receiver whose Transmitter is gone reads nothing, silently.
    for (const NetWireless& w : macro.wirelessPairs())
        if (w.transmitter == n)
            return refuse(why, "Receivers still use this Transmitter; delete them first");
    std::vector<NetLink> kept;
    for (NetLink l : macro.links) {
        if (l.fromNode == n || l.toNode == n)
            continue;
        if (l.fromNode > n) --l.fromNode;
        if (l.toNode > n) --l.toNode;
        kept.push_back(l);
    }
    macro.links = std::move(kept);
    macro.nodes.erase(macro.nodes.begin() + node);
    return true;
}

bool connect(NetMacro& macro, int from, int output, int to, int input, std::string* why)
{
    if (!validNode(macro, from) || !validNode(macro, to))
        return refuse(why, "no such node");
    if (output < 1 || input < 1)
        return refuse(why, "ports are numbered from 1");
    if (from == to)
        return refuse(why, "a node cannot feed itself");
    const auto f = static_cast<std::size_t>(from), t = static_cast<std::size_t>(to);
    if (reaches(macro, t, f))
        return refuse(why, "that wire would make a loop");
    // One wire per input: replace any existing one.
    macro.links.erase(std::remove_if(macro.links.begin(), macro.links.end(),
                                     [&](const NetLink& l) { return l.toNode == t && l.toInput == input; }),
                      macro.links.end());
    macro.links.push_back({f, output, t, input});
    NetNode& dst = macro.nodes[t];
    dst.numInputs = std::max(dst.numInputs, input);
    NetNode& src = macro.nodes[f];
    src.numOutputs = std::max(src.numOutputs, output);
    return true;
}

bool disconnect(NetMacro& macro, int to, int input, std::string* why)
{
    if (!validNode(macro, to))
        return refuse(why, "no such node");
    const auto t = static_cast<std::size_t>(to);
    auto before = macro.links.size();
    macro.links.erase(std::remove_if(macro.links.begin(), macro.links.end(),
                                     [&](const NetLink& l) { return l.toNode == t && l.toInput == input; }),
                      macro.links.end());
    if (macro.links.size() == before)
        return refuse(why, "that input has no wire");
    return true;
}

bool setInputValue(NetMacro& macro, int node, int input, const std::string& value,
                   std::string* why)
{
    if (!validNode(macro, node))
        return refuse(why, "no such node");
    if (input < 1)
        return refuse(why, "inputs are numbered from 1");
    NetNode& n = macro.nodes[static_cast<std::size_t>(node)];
    auto it = std::find_if(n.inputs.begin(), n.inputs.end(),
                           [&](const NetInput& in) { return in.index == input; });
    if (value.empty()) {
        if (it != n.inputs.end()) {
            it->value.clear();
            it->hasValue = false;
            it->defaulting = true;
        }
        return true;
    }
    if (it == n.inputs.end()) {
        NetInput in{};
        in.index = input;
        in.visible = true;
        in.type = 0;
        n.inputs.push_back(in);
        it = n.inputs.end() - 1;
        std::sort(n.inputs.begin(), n.inputs.end(),
                  [](const NetInput& a, const NetInput& b) { return a.index < b.index; });
        it = std::find_if(n.inputs.begin(), n.inputs.end(),
                          [&](const NetInput& in2) { return in2.index == input; });
    }
    it->value = value;
    it->hasValue = true;
    it->defaulting = false;
    n.numInputs = std::max(n.numInputs, input);
    return true;
}

} // namespace dxui

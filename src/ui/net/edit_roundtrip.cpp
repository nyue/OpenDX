// The structural writer's round trip, per file:
//  1. identity: writeNet(text, f, f) gives text back byte for byte;
//  2. `trials` times: apply `edits` random NetEdit operations (fixed seed) --
//     add a node of a common module and wire it, connect/disconnect ports whose
//     types fit, delete a node, set or clear an input value, move a node --
//     write, re-parse, and check that the re-parsed network is the edited one
//     (nodes, positions, labels, input counts, input comments, wires, assigned
//     values of rewritten nodes) and that every line of the nodes the edits did
//     not touch is still in the file, unchanged.
// Edits the writer documents as unsupported (interactors, Transmitter/
// Receiver, Input/Output) are not generated; any refusal counts as a failure.

#include "edit_roundtrip.h"

#include "ModuleDefs.h"
#include "NetEdit.h"
#include "NetFile.h"
#include "NetWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dxui;

namespace {

const std::set<std::string>& special()
{
    static const std::set<std::string> s = {
        "Integer", "IntegerList", "Scalar", "ScalarList", "Vector", "VectorList", "Selector",
        "SelectorList", "Toggle", "Value", "ValueList", "String", "StringList", "FileSelector",
        "Colormap", "Sequencer", "Reset", "Probe", "ProbeList", "Pick", "DXLInput", "DXLOutput",
        "Input", "Output", "Transmitter", "Receiver"};
    return s;
}
bool isSpecial(const NetNode& n) { return special().count(n.module) != 0; }

std::string name(const NetNode& n) { return n.module + "[" + std::to_string(n.instance) + "]"; }

std::string readFile(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::vector<std::string> words(const std::string& types)
{
    std::vector<std::string> w;
    std::string cur;
    for (char c : types) {
        if (std::isalpha(static_cast<unsigned char>(c))) cur += c;
        else { if (!cur.empty() && cur != "or") w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty() && cur != "or") w.push_back(cur);
    return w;
}

// Loose MDF type match: either side "object", or a shared word.
bool typesFit(const ModuleLibrary& lib, const NetNode& from, int out, const NetNode& to, int in)
{
    const ModuleDef* fd = lib.find(from.module);
    const ModuleDef* td = lib.find(to.module);
    if (!fd || !td) return false;
    const ModuleParam* o = fd->output(out);
    const ModuleParam* i = td->input(in);
    if (!o || !i) return false;
    auto ow = words(o->types), iw = words(i->types);
    for (const std::string& a : iw) {
        if (a == "object") return true;
        for (const std::string& b : ow)
            if (a == b || b == "object") return true;
    }
    return false;
}

std::string valueFor(const ModuleParam* p, std::mt19937& rng)
{
    if (!p) return "";
    const std::string& t = p->types;
    auto has = [&](const char* w) { return t.find(w) != std::string::npos; };
    if (has("string")) {
        if (!p->defaultValue.empty() && p->defaultValue.front() == '"') return p->defaultValue;
        return rng() % 2 ? "\"x\"" : "";
    }
    if (has("flag")) return std::to_string(rng() % 2);
    if (has("integer")) return std::to_string(1 + rng() % 5);
    if (has("scalar")) return std::to_string(1 + rng() % 9) + ".5";
    if (has("vector")) return "[0 1 0]";
    return "";
}

// True if `target` is downstream of `start`, through wires and Transmitter ->
// Receiver pairs (NetEdit's loop check sees wires only).
bool reaches(const NetMacro& m, std::size_t start, std::size_t target)
{
    std::vector<std::vector<std::size_t>> next(m.nodes.size());
    for (const NetLink& l : m.links) next[l.fromNode].push_back(l.toNode);
    for (const NetWireless& w : m.wirelessPairs()) next[w.transmitter].push_back(w.receiver);
    std::vector<bool> seen(m.nodes.size(), false);
    std::vector<std::size_t> todo = {start};
    while (!todo.empty()) {
        std::size_t n = todo.back();
        todo.pop_back();
        if (n == target) return true;
        if (seen[n]) continue;
        seen[n] = true;
        for (std::size_t t : next[n]) todo.push_back(t);
    }
    return false;
}

struct Trial {
    NetFile edited;
    std::vector<std::string> log;
};

// Edits after which the program should compute what it did before.
void benignEdits(const NetFile& f, const ModuleLibrary& lib, std::mt19937& rng, int count, Trial& t)
{
    t.edited = f;
    NetMacro& m = t.edited.macros[0];
    auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
    bool imageAdded = false;
    for (int done = 0, guard = 0; done < count && guard < 200; ++guard) {
        unsigned r = rng() % 100;
        std::vector<std::size_t> wires;
        for (std::size_t i = 0; i < m.links.size(); ++i)
            if (!isSpecial(m.nodes[m.links[i].toNode]) && !isSpecial(m.nodes[m.links[i].fromNode]))
                wires.push_back(i);
        if (r < 30 && !wires.empty()) {   // splice Options into a wire
            const ModuleDef* def = lib.find("Options");
            if (!def) continue;
            NetLink l = m.links[wires[pick(wires.size())]];
            const std::string what = name(m.nodes[l.fromNode]) + "." + std::to_string(l.fromOutput) + " -> " +
                                     name(m.nodes[l.toNode]) + "." + std::to_string(l.toInput);
            const float x = m.nodes[l.toNode].x + 30, y = m.nodes[l.toNode].y - 40;
            int n = addNode(m, *def, x, y);   // may move m.nodes
            t.log.push_back("splice " + name(m.nodes[static_cast<std::size_t>(n)]) + " into " + what);
            if (!connect(m, static_cast<int>(l.fromNode), l.fromOutput, n, 1) ||
                !connect(m, n, 1, static_cast<int>(l.toNode), l.toInput)) {
                t.log.push_back("  (splice failed)");
            }
            ++done;
        } else if (r < 40 && !wires.empty()) {   // re-make a wire (no change to the model)
            NetLink l = m.links[wires[pick(wires.size())]];
            disconnect(m, static_cast<int>(l.toNode), l.toInput);
            connect(m, static_cast<int>(l.fromNode), l.fromOutput, static_cast<int>(l.toNode), l.toInput);
            t.log.push_back("rewire " + name(m.nodes[l.toNode]) + "." + std::to_string(l.toInput));
            ++done;
        } else if (r < 70) {   // make a defaulting input explicit, with the default it shows
            std::size_t n = pick(std::max<std::size_t>(1, m.nodes.size()));
            if (m.nodes.empty() || isSpecial(m.nodes[n]) || m.nodes[n].module == "Image") continue;
            NetNode& node = m.nodes[n];
            std::vector<NetInput> shown;
            for (const NetInput& in : node.inputs)
                if (in.hasValue && in.defaulting && !in.value.empty() && in.value.front() != '(') shown.push_back(in);
            if (shown.empty()) continue;
            NetInput in = shown[pick(shown.size())];
            bool wiredIn = false;
            for (const NetLink& l : m.links) wiredIn = wiredIn || (l.toNode == n && l.toInput == in.index);
            if (wiredIn) continue;
            setInputValue(m, static_cast<int>(n), in.index, in.value);
            t.log.push_back("explicit " + name(node) + "." + std::to_string(in.index) + " = " + in.value);
            ++done;
        } else if (r < 85) {   // move
            if (m.nodes.empty()) continue;
            NetNode& node = m.nodes[pick(m.nodes.size())];
            node.x += 25;
            node.y -= 10;
            t.log.push_back("move " + name(node));
            ++done;
        } else if (!imageAdded) {   // a second view of something already shown
            const ModuleDef* def = lib.find("Image");
            if (!def) continue;
            std::vector<std::size_t> shown;
            for (const NetLink& l : m.links)
                if (m.nodes[l.toNode].module == "Image" && l.toInput == 2) shown.push_back(l.toNode);
            if (shown.empty()) continue;
            const NetLink* src = nullptr;
            std::size_t img = shown[pick(shown.size())];
            for (const NetLink& l : m.links)
                if (l.toNode == img && l.toInput == 2) src = &l;
            NetLink s = *src;
            int n = addNode(m, *def, m.nodes[img].x + 60, m.nodes[img].y);
            connect(m, static_cast<int>(s.fromNode), s.fromOutput, n, 2);
            t.log.push_back("add " + name(m.nodes[static_cast<std::size_t>(n)]) + " fed by " +
                            name(m.nodes[s.fromNode]) + "." + std::to_string(s.fromOutput));
            imageAdded = true;
            ++done;
        }
    }
}

// One random sequence of edits on f.macros[0].
void randomEdits(const NetFile& f, const ModuleLibrary& lib, std::mt19937& rng, int count, Trial& t)
{
    t.edited = f;
    NetMacro& m = t.edited.macros[0];
    static const char* adds[] = {"Isosurface", "Color", "Collect", "Compute", "Shade", "AutoColor",
                                 "ShowBox", "Scale", "Options", "Mark", "Normals", "Image"};
    auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
    auto anyNode = [&](auto pred) -> int {
        std::vector<int> c;
        for (std::size_t i = 0; i < m.nodes.size(); ++i)
            if (pred(m.nodes[i])) c.push_back(static_cast<int>(i));
        return c.empty() ? -1 : c[pick(c.size())];
    };
    auto plain = [](const NetNode& n) { return !isSpecial(n); };
    auto wired = [&](int node, int input) {
        for (const NetLink& l : m.links)
            if (static_cast<int>(l.toNode) == node && l.toInput == input) return true;
        return false;
    };
    // Wires output `out` of a random fitting node into (to, in).
    auto feed = [&](int to, int in) {
        for (int tries = 0; tries < 30; ++tries) {
            int from = static_cast<int>(pick(m.nodes.size()));
            const NetNode& src = m.nodes[static_cast<std::size_t>(from)];
            if (src.module == "Transmitter" || src.module == "Output") continue;
            int outs = std::max(1, src.numOutputs);
            int out = 1 + static_cast<int>(pick(static_cast<std::size_t>(outs)));
            if (!typesFit(lib, src, out, m.nodes[static_cast<std::size_t>(to)], in)) continue;
            if (reaches(m, static_cast<std::size_t>(to), static_cast<std::size_t>(from))) continue;
            std::string why;
            if (connect(m, from, out, to, in, &why)) {
                t.log.push_back("connect " + name(src) + "." + std::to_string(out) + " -> " +
                                name(m.nodes[static_cast<std::size_t>(to)]) + "." + std::to_string(in));
                return true;
            }
        }
        return false;
    };

    for (int done = 0; done < count;) {
        unsigned r = rng() % 100;
        if (r < 14) {   // add
            const char* mod = adds[pick(sizeof adds / sizeof *adds)];
            if (std::string(mod) == "Image" && rng() % 3) continue;
            const ModuleDef* def = lib.find(mod);
            if (!def) continue;
            int n = addNode(m, *def, static_cast<float>(rng() % 600), static_cast<float>(rng() % 600));
            t.log.push_back(std::string("add ") + name(m.nodes[static_cast<std::size_t>(n)]));
            if (rng() % 10 < 7) feed(n, std::string(mod) == "Image" ? 2 : 1);
            if (std::string(mod) != "Image" && rng() % 2) {
                for (int tries = 0; tries < 30; ++tries) {
                    int to = anyNode(plain);
                    if (to < 0 || to == n) break;
                    const NetNode& dst = m.nodes[static_cast<std::size_t>(to)];
                    if (dst.numInputs < 1) continue;
                    int in = 1 + static_cast<int>(pick(static_cast<std::size_t>(dst.numInputs)));
                    if (!typesFit(lib, m.nodes[static_cast<std::size_t>(n)], 1, dst, in)) continue;
                    if (reaches(m, static_cast<std::size_t>(to), static_cast<std::size_t>(n))) continue;
                    if (connect(m, n, 1, to, in)) {
                        t.log.push_back("connect " + name(m.nodes[static_cast<std::size_t>(n)]) + ".1 -> " +
                                        name(dst) + "." + std::to_string(in));
                        break;
                    }
                }
            }
            ++done;
        } else if (r < 38) {   // connect
            int to = anyNode(plain);
            if (to < 0) continue;
            const NetNode& dst = m.nodes[static_cast<std::size_t>(to)];
            if (dst.numInputs < 1) continue;
            int in = 1 + static_cast<int>(pick(static_cast<std::size_t>(dst.numInputs)));
            if (feed(to, in)) ++done;
        } else if (r < 52) {   // disconnect
            std::vector<std::size_t> c;
            for (std::size_t i = 0; i < m.links.size(); ++i)
                if (!isSpecial(m.nodes[m.links[i].toNode])) c.push_back(i);
            if (c.empty()) continue;
            NetLink l = m.links[c[pick(c.size())]];
            if (disconnect(m, static_cast<int>(l.toNode), l.toInput)) {
                t.log.push_back("disconnect " + name(m.nodes[l.toNode]) + "." + std::to_string(l.toInput));
                ++done;
            }
        } else if (r < 62) {   // delete
            // Not a node feeding an interactor: that would change the
            // interactor, which the writer refuses.
            int n = anyNode([&](const NetNode& x) {
                if (x.module == "Input" || x.module == "Output" || x.module == "Transmitter") return false;
                std::size_t idx = static_cast<std::size_t>(&x - m.nodes.data());
                for (const NetLink& l : m.links)
                    if (l.fromNode == idx && isSpecial(m.nodes[l.toNode])) return false;
                return true;
            });
            if (n < 0) continue;
            std::string what = name(m.nodes[static_cast<std::size_t>(n)]);
            if (deleteNode(m, n)) {
                t.log.push_back("delete " + what);
                ++done;
            }
        } else if (r < 70) {   // show or hide an unwired input
            int n = anyNode(plain);
            if (n < 0) continue;
            NetNode& node = m.nodes[static_cast<std::size_t>(n)];
            if (node.numInputs < 1) continue;
            int in = 1 + static_cast<int>(pick(static_cast<std::size_t>(node.numInputs)));
            if (wired(n, in)) continue;
            const ModuleDef* def = lib.find(node.module);
            bool show = rng() % 2 == 0;
            std::string what = name(node);
            if (setInputVisible(m, n, in, show, def)) {
                t.log.push_back(std::string(show ? "show " : "hide ") + what + "." + std::to_string(in));
                ++done;
            }
        } else if (r < 84) {   // set or clear a value
            int n = anyNode(plain);
            if (n < 0) continue;
            NetNode& node = m.nodes[static_cast<std::size_t>(n)];
            if (node.numInputs < 1) continue;
            int in = 1 + static_cast<int>(pick(static_cast<std::size_t>(node.numInputs)));
            if (wired(n, in)) continue;
            const ModuleDef* def = lib.find(node.module);
            std::string v = rng() % 4 == 0 ? std::string() : valueFor(def ? def->input(in) : nullptr, rng);
            if (node.module == "Image" || node.module == "Display" || node.module == "Import") v.clear();
            std::string what = name(node);
            if (setInputValue(m, n, in, v)) {
                t.log.push_back("set " + what + "." + std::to_string(in) + " = " + (v.empty() ? "(default)" : v));
                ++done;
            }
        } else {   // move
            if (m.nodes.empty()) continue;
            NetNode& node = m.nodes[pick(m.nodes.size())];
            node.x += static_cast<float>(static_cast<int>(rng() % 101) - 50);
            node.y += static_cast<float>(static_cast<int>(rng() % 101) - 50);
            t.log.push_back("move " + name(node));
            ++done;
        }
    }
}

using LinkKey = std::tuple<std::string, int, int, std::string, int, int>;
std::set<LinkKey> linkSet(const NetMacro& m)
{
    std::set<LinkKey> s;
    for (const NetLink& l : m.links) {
        const NetNode &a = m.nodes[l.fromNode], &b = m.nodes[l.toNode];
        s.insert({a.module, a.instance, l.fromOutput, b.module, b.instance, l.toInput});
    }
    return s;
}

const NetInput* inputOf(const NetNode& n, int k)
{
    for (const NetInput& in : n.inputs)
        if (in.index == k) return &in;
    return nullptr;
}

// Nodes the edits changed beyond moving (or added): their text was regenerated.
std::set<std::pair<std::string, int>> touched(const NetMacro& o, const NetMacro& e)
{
    std::set<std::pair<std::string, int>> t;
    auto incoming = [](const NetMacro& m, std::size_t node) {
        std::map<int, std::tuple<std::string, int, int>> w;
        for (const NetLink& l : m.links)
            if (l.toNode == node)
                w[l.toInput] = {m.nodes[l.fromNode].module, m.nodes[l.fromNode].instance, l.fromOutput};
        return w;
    };
    for (std::size_t i = 0; i < e.nodes.size(); ++i) {
        const NetNode& n = e.nodes[i];
        const NetNode* m = nullptr;
        std::size_t mi = 0;
        for (std::size_t j = 0; j < o.nodes.size(); ++j)
            if (n.line != 0 && o.nodes[j].line == n.line) { m = &o.nodes[j]; mi = j; }
        bool same = m && m->numInputs == n.numInputs && incoming(o, mi) == incoming(e, i) &&
                    m->inputs.size() == n.inputs.size();
        for (std::size_t k = 0; same && k < n.inputs.size(); ++k) {
            const NetInput &a = n.inputs[k], &b = m->inputs[k];
            same = a.index == b.index && a.value == b.value && a.hasValue == b.hasValue &&
                   a.defaulting == b.defaulting && a.visible == b.visible && a.type == b.type;
        }
        // Wiring an output its text does not assign rewrites the source.
        for (const NetLink& l : e.links)
            if (l.fromNode == i && m && l.fromOutput > m->numOutputs) same = false;
        if (!same) t.insert({n.module, n.instance});
    }
    return t;
}

// Empty when the re-parse matches the edited model.
std::string compare(const NetFile& orig, const NetFile& edited, const NetFile& again, const std::string& text,
                    const std::string& written)
{
    const NetMacro &e = edited.macros[0], &a = again.macros[0], &o = orig.macros[0];
    if (e.nodes.size() != a.nodes.size())
        return "node count " + std::to_string(a.nodes.size()) + ", expected " + std::to_string(e.nodes.size());
    std::map<std::pair<std::string, int>, const NetNode*> byName;
    for (const NetNode& n : a.nodes) byName[{n.module, n.instance}] = &n;
    const auto changed = touched(o, e);
    const bool varStyle = written.find("_in_1 = ") != std::string::npos || o.name == "main";
    for (std::size_t i = 0; i < e.nodes.size(); ++i) {
        const NetNode& n = e.nodes[i];
        auto it = byName.find({n.module, n.instance});
        if (it == byName.end()) return name(n) + " missing after re-parse";
        const NetNode& b = *it->second;
        if (std::lround(n.x) != std::lround(b.x) || std::lround(n.y) != std::lround(b.y))
            return name(n) + " at the wrong place";
        if (n.label != b.label) return name(n) + " label";
        if (n.numInputs != b.numInputs)
            return name(n) + " inputs " + std::to_string(b.numInputs) + ", expected " + std::to_string(n.numInputs);
        // Input comments; the writer may add an Image's id and infer types.
        std::vector<NetInput> want = n.inputs;
        if (n.line == 0 && n.module == "Image")   // id, useVector, buttonState, renderMode
            for (int k : {1, 4, 14, 19})
                if (!inputOf(n, k))
                    if (const NetInput* in = inputOf(b, k)) want.push_back(*in);
        std::sort(want.begin(), want.end(), [](const NetInput& x, const NetInput& y) { return x.index < y.index; });
        std::vector<NetInput> got = b.inputs;
        std::sort(got.begin(), got.end(), [](const NetInput& x, const NetInput& y) { return x.index < y.index; });
        if (n.module == "Image") {   // RECENABLE (24) is never written for a rewritten Image
            auto drop = [](std::vector<NetInput>& v) {
                v.erase(std::remove_if(v.begin(), v.end(), [](const NetInput& x) { return x.index == 24; }), v.end());
            };
            if (changed.count({n.module, n.instance})) { drop(want); drop(got); }
        }
        if (want.size() != got.size()) return name(n) + " input comment count";
        for (std::size_t k = 0; k < want.size(); ++k) {
            const NetInput &x = want[k], &y = got[k];
            if (x.index != y.index || x.visible != y.visible || x.hasValue != y.hasValue || x.value != y.value ||
                (x.hasValue && x.type != 0 && x.type != y.type))
                return name(n) + " input[" + std::to_string(x.index) + "] comment";
            bool wiredIn = false;
            for (const NetLink& l : e.links)
                if (l.toNode == i && l.toInput == x.index) wiredIn = true;
            if (x.defaulting != y.defaulting && !(wiredIn && y.defaulting))
                return name(n) + " input[" + std::to_string(x.index) + "] defaulting";
        }
        // Rewritten nodes: unwired inputs carry their value (or NULL).
        if (varStyle && changed.count({n.module, n.instance}) && !special().count(n.module)) {
            for (int k = 1; k <= n.numInputs; ++k) {
                bool wiredIn = false;
                for (const NetLink& l : e.links)
                    if (l.toNode == i && l.toInput == k) wiredIn = true;
                if (wiredIn) {
                    if (b.assignedValues.count(k)) return name(n) + " wired input " + std::to_string(k) + " also assigned";
                    continue;
                }
                const NetInput* in = inputOf(n, k);
                if (!in || !in->hasValue || in->defaulting) continue;
                auto av = b.assignedValues.find(k);
                if (av == b.assignedValues.end() || av->second != in->value) {
                    // Unchanged inputs keep their original assignment text.
                    const NetNode* on = nullptr;
                    for (const NetNode& x : o.nodes) if (n.line && x.line == n.line) on = &x;
                    const NetInput* oi = on ? inputOf(*on, k) : nullptr;
                    bool sameAsOrig = oi && oi->value == in->value && oi->hasValue && !oi->defaulting;
                    if (!sameAsOrig)
                        return name(n) + " input " + std::to_string(k) + " assigned '" +
                               (av == b.assignedValues.end() ? std::string("(none)") : av->second) + "'";
                }
            }
        }
    }
    if (linkSet(e) != linkSet(a)) return "wires differ";

    // Order: each node after the nodes it reads, unless the original already
    // had that pair the other way round.
    {
        std::set<std::pair<std::string, std::string>> tolerated;
        for (const NetLink& l : o.links)
            if (l.fromNode > l.toNode) tolerated.insert({name(o.nodes[l.fromNode]), name(o.nodes[l.toNode])});
        for (const NetWireless& w : o.wirelessPairs())
            if (w.transmitter > w.receiver) tolerated.insert({name(o.nodes[w.transmitter]), name(o.nodes[w.receiver])});
        for (const NetLink& l : a.links)
            if (l.fromNode > l.toNode && !tolerated.count({name(a.nodes[l.fromNode]), name(a.nodes[l.toNode])}))
                return name(a.nodes[l.toNode]) + " written before its source " + name(a.nodes[l.fromNode]);
        for (const NetWireless& w : a.wirelessPairs())
            if (w.transmitter > w.receiver &&
                !tolerated.count({name(a.nodes[w.transmitter]), name(a.nodes[w.receiver])}))
                return name(a.nodes[w.receiver]) + " written before its transmitter";
    }
    // Assignments before the run.
    {
        std::size_t run = written.rfind("\n" + o.name + "();");
        std::size_t lastAssign = written.rfind("\n" + o.name + "_");
        if (run != std::string::npos && lastAssign != std::string::npos && lastAssign > run &&
            text.rfind("\n" + o.name + "_") < text.rfind("\n" + o.name + "();"))
            return "an assignment was written after " + o.name + "();";
    }

    // Untouched nodes: every line of their block is in the output, unchanged.
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
    }
    std::multiset<std::string> outLines;
    {
        std::istringstream in(written);
        for (std::string l; std::getline(in, l);) outLines.insert(l);
    }
    std::vector<int> starts;
    for (const NetNode& n : o.nodes) starts.push_back(n.line);
    for (const NetDecorator& d : o.decorators) starts.push_back(d.line);
    int endMarker = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find("// network: end of macro body") != std::string::npos) { endMarker = static_cast<int>(i) + 1; break; }
    if (endMarker) starts.push_back(endMarker);
    std::sort(starts.begin(), starts.end());
    for (const NetNode& n : e.nodes) {
        if (n.line == 0 || changed.count({n.module, n.instance})) continue;
        auto nx = std::upper_bound(starts.begin(), starts.end(), n.line);
        int last = nx == starts.end() ? n.line : *nx - 1;
        std::string block;
        for (int l = n.line + 1; l <= last && l <= static_cast<int>(lines.size()); ++l) {
            const std::string& s = lines[static_cast<std::size_t>(l) - 1];
            if (l == last && nx != starts.end() && s.find_first_not_of(" \t\r/") == std::string::npos) continue;
            block += s + "\n";
        }
        if (!block.empty() && written.find(block) == std::string::npos)
            return "lines of untouched " + name(n) + " changed";
        const NetNode* on = nullptr;
        for (const NetNode& x : o.nodes) if (x.line == n.line) on = &x;
        bool moved = on && (std::lround(on->x) != std::lround(n.x) || std::lround(on->y) != std::lround(n.y));
        if (!moved && !outLines.count(lines[static_cast<std::size_t>(n.line) - 1]))
            return "node line of unmoved " + name(n) + " changed";
        // Its value assignments.
        const std::string pfx = o.name + "_" + n.module + "_" + std::to_string(n.instance) + "_";
        for (std::size_t l = endMarker ? static_cast<std::size_t>(endMarker) : 0; l < lines.size(); ++l)
            if (lines[l].compare(0, pfx.size(), pfx) == 0 && !outLines.count(lines[l]))
                return "value line '" + lines[l] + "' of untouched " + name(n) + " lost";
    }
    return "";
}

} // namespace

int runEditRoundtrip(const std::vector<const char*>& files, const ModuleLibrary& lib,
                     const EditRoundtripOptions& opt)
{
    int failures = 0;
    long trialsOk = 0, trialsRun = 0, editsDone = 0;
    std::set<std::string> outNames;
    for (const char* file : files) {
        const std::string text = readFile(file);
        NetFile f;
        NetParseError perr;
        if (!parseNet(text, f, &perr)) {
            std::printf("FAIL %s: parse: %s\n", file, perr.message.c_str());
            ++failures;
            continue;
        }
        std::string out, err;
        if (!writeNet(text, f, f, &lib, out, &err)) {
            std::printf("FAIL %s: identity write: %s\n", file, err.c_str());
            ++failures;
            continue;
        }
        if (out != text) {
            std::printf("FAIL %s: identity write changed the text\n", file);
            if (!opt.outDir.empty()) {
                fs::create_directories(opt.outDir);
                std::ofstream(fs::path(opt.outDir) / ("identity_" + fs::path(file).filename().string()),
                              std::ios::binary) << out;
            }
            ++failures;
            continue;
        }
        std::string problem;
        std::string lastText;
        Trial lastTrial;
        unsigned fileSeed = opt.seed;
        for (const char* c = file; *c; ++c) fileSeed = fileSeed * 131u + static_cast<unsigned char>(*c);
        for (int t = 0; t < opt.trials && problem.empty(); ++t) {
            std::mt19937 rng(fileSeed + static_cast<unsigned>(t) * 7919u);
            Trial trial;
            if (opt.benign)
                benignEdits(f, lib, rng, opt.edits, trial);
            else
                randomEdits(f, lib, rng, opt.edits, trial);
            ++trialsRun;
            editsDone += static_cast<long>(trial.log.size());
            std::string written;
            if (!writeNet(text, f, trial.edited, &lib, written, &err)) {
                problem = "trial " + std::to_string(t) + ": writer refused: " + err;
            } else {
                NetFile again;
                if (!parseNet(written, again, &perr)) {
                    problem = "trial " + std::to_string(t) + ": re-parse line " + std::to_string(perr.line) + ": " +
                              perr.message;
                } else {
                    std::string c = compare(f, trial.edited, again, text, written);
                    if (!c.empty()) problem = "trial " + std::to_string(t) + ": " + c;
                    else {
                        ++trialsOk;
                        lastText = written;
                        lastTrial = trial;
                    }
                }
            }
            if (!problem.empty() && opt.verbose)
                for (const std::string& l : trial.log) std::printf("    %s\n", l.c_str());
            if (!problem.empty() && !opt.outDir.empty()) {
                fs::create_directories(opt.outDir);
                std::ofstream(fs::path(opt.outDir) / "failed.net", std::ios::binary) << written;
            }
        }
        if (problem.empty()) {
            std::printf("ok %s\n", file);
            if (!opt.outDir.empty() && !lastText.empty()) {
                fs::path p(file);
                std::string base = p.parent_path().filename().string() + "_" + p.filename().string();
                while (!outNames.insert(base).second) base = "x" + base;
                fs::create_directories(opt.outDir);
                std::ofstream(fs::path(opt.outDir) / base, std::ios::binary) << lastText;
                std::ofstream log(fs::path(opt.outDir) / (base + ".edits"));
                log << file << "\n";
                for (const std::string& l : lastTrial.log) log << l << "\n";
            }
        } else {
            std::printf("FAIL %s: %s\n", file, problem.c_str());
            ++failures;
        }
    }
    std::printf("%zu files, %d failed; %ld/%ld edit sequences ok, %ld edits\n", files.size(), failures,
                trialsOk, trialsRun, editsDone);
    return failures;
}

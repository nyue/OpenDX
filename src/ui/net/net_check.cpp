// net_check FILE...         parse each .net and print one line per file
// net_check --dump FILE     print the nodes and links for eyeballing
// net_check -w FILE...      also print each file's warnings
// net_check --roundtrip FILE...
//                           check the position writer: unchanged positions give
//                           the file back byte for byte; moved ones re-parse at
//                           the new place with every other line unchanged
//
// Exit status 1 if any file fails to parse.

#include "NetFile.h"
#include "NetWriter.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string nodeName(const dxui::NetNode& n) {
    return n.module + "[" + std::to_string(n.instance) + "]";
}

void dump(const dxui::NetFile& f) {
    std::printf("version: format %s, dx %s\n", f.formatVersion.c_str(), f.dxVersion.c_str());
    for (const dxui::NetMacro& m : f.macros) {
        std::printf("macro %s(", m.name.c_str());
        for (std::size_t i = 0; i < m.formalInputs.size(); ++i)
            std::printf("%s%s", i ? ", " : "", m.formalInputs[i].name.c_str());
        std::printf(") -> (");
        for (std::size_t i = 0; i < m.formalOutputs.size(); ++i)
            std::printf("%s%s", i ? ", " : "", m.formalOutputs[i].name.c_str());
        std::printf(")  workspace %dx%d, %zu comment lines, %zu decorators\n", m.workspaceWidth,
                    m.workspaceHeight, m.comments.size(), m.decorators.size());
        std::printf("nodes (%zu):\n", m.nodes.size());
        for (std::size_t i = 0; i < m.nodes.size(); ++i) {
            const dxui::NetNode& n = m.nodes[i];
            std::printf("  %3zu %s \"%s\" %g %g  in=%d out=%d\n", i, nodeName(n).c_str(), n.label.c_str(),
                        n.x, n.y, n.numInputs, n.numOutputs);
        }
        std::printf("links (%zu):\n", m.links.size());
        for (const dxui::NetLink& l : m.links)
            std::printf("  %s.%d -> %s.%d\n", nodeName(m.nodes[l.fromNode]).c_str(), l.fromOutput,
                        nodeName(m.nodes[l.toNode]).c_str(), l.toInput);
        for (const dxui::NetWireless& w : m.wirelessPairs())
            std::printf("  %s ~> %s  (\"%s\")\n", nodeName(m.nodes[w.transmitter]).c_str(),
                        nodeName(m.nodes[w.receiver]).c_str(), m.nodes[w.receiver].label.c_str());
    }
    for (const std::string& w : f.warnings) std::printf("warning: %s\n", w.c_str());
}

// Writes the positions of every macro in turn.
bool writeAll(std::string_view text, const dxui::NetFile& f, std::string& out, std::string& error) {
    std::string current(text);
    for (const dxui::NetMacro& m : f.macros) {
        if (!dxui::writeNodePositions(current, m, out, &error)) return false;
        current = out;
    }
    out = current;
    return true;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return lines;
}

// Empty string on success, else what went wrong.
std::string roundtrip(const char* file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    dxui::NetFile f;
    dxui::NetParseError err;
    if (!dxui::parseNet(text, f, &err)) return "parse: " + err.message;

    std::string same, error;
    if (!writeAll(text, f, same, error)) return "write: " + error;
    if (same != text) return "unchanged positions did not give the file back";

    // Move every node, write, re-parse: new positions, other lines untouched.
    dxui::NetFile moved = f;
    for (dxui::NetMacro& m : moved.macros)
        for (dxui::NetNode& n : m.nodes) { n.x += 17; n.y -= 5; }
    std::string shifted;
    if (!writeAll(text, moved, shifted, error)) return "write moved: " + error;
    dxui::NetFile again;
    if (!dxui::parseNet(shifted, again, &err)) return "re-parse: " + err.message;
    for (std::size_t k = 0; k < moved.macros.size(); ++k)
        for (std::size_t i = 0; i < moved.macros[k].nodes.size(); ++i) {
            const auto& a = moved.macros[k].nodes[i];
            const auto& b = again.macros[k].nodes[i];
            if (std::lround(a.x) != std::lround(b.x) || std::lround(a.y) != std::lround(b.y))
                return "moved " + nodeName(a) + " re-parsed at the wrong place";
        }
    const auto before = splitLines(text), after = splitLines(shifted);
    if (before.size() != after.size()) return "line count changed";
    for (std::size_t i = 0; i < before.size(); ++i)
        if (before[i] != after[i] && before[i].find("// node ") == std::string::npos)
            return "line " + std::to_string(i + 1) + " changed but is not a node comment";
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    bool dumpMode = false, warnings = false, roundtripMode = false;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--dump") {
            dumpMode = true;
        } else if (a == "--roundtrip") {
            roundtripMode = true;
        } else if (a == "-w") {
            warnings = true;
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: net_check [-w] FILE...\n       net_check --dump FILE\n"
                             "       net_check --roundtrip FILE...\n");
        return 2;
    }

    int failures = 0;
    if (roundtripMode) {
        for (const char* file : files) {
            std::string problem = roundtrip(file);
            if (problem.empty()) std::printf("ok %s\n", file);
            else { std::printf("FAIL %s: %s\n", file, problem.c_str()); ++failures; }
        }
        return failures ? 1 : 0;
    }
    for (const char* file : files) {
        dxui::NetFile f;
        dxui::NetParseError err;
        if (!dxui::parseNetFile(file, f, &err)) {
            std::printf("FAIL %s:%d: %s\n", file, err.line, err.message.c_str());
            ++failures;
            continue;
        }
        if (dumpMode) {
            std::printf("== %s\n", file);
            dump(f);
            continue;
        }
        std::size_t nodes = 0, links = 0;
        for (const dxui::NetMacro& m : f.macros) {
            nodes += m.nodes.size();
            links += m.links.size();
        }
        std::printf("ok %s macros=%zu nodes=%zu links=%zu\n", file, f.macros.size(), nodes, links);
        if (warnings)
            for (const std::string& w : f.warnings) std::printf("  warning: %s\n", w.c_str());
    }
    return failures ? 1 : 0;
}

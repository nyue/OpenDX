// net_check FILE...         parse each .net and print one line per file
// net_check --dump FILE     print the nodes and links for eyeballing
// net_check -w FILE...      also print each file's warnings
//
// Exit status 1 if any file fails to parse.

#include "NetFile.h"

#include <cstdio>
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

}  // namespace

int main(int argc, char** argv) {
    bool dumpMode = false, warnings = false;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--dump") {
            dumpMode = true;
        } else if (a == "-w") {
            warnings = true;
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: net_check [-w] FILE...\n       net_check --dump FILE\n");
        return 2;
    }

    int failures = 0;
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

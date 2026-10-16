// net_check --edit-roundtrip: random NetEdit sequences through writeNet and
// back through the parser. See edit_roundtrip.cpp.

#pragma once

#include <string>
#include <vector>

namespace dxui {
class ModuleLibrary;
}

struct EditRoundtripOptions {
    unsigned seed = 1;
    int trials = 20;            // edit sequences per file
    int edits = 6;              // edits per sequence
    std::string outDir;         // write one edited file per input here (plus .edits log)
    bool verbose = false;
    // Edits that keep what the program computes: splice an Options node into
    // a wire, re-make a wire, re-set a value, move, add an Image fed from an
    // existing output.
    bool benign = false;
};

// Prints one line per file; returns the number of failing files.
int runEditRoundtrip(const std::vector<const char*>& files, const dxui::ModuleLibrary& lib,
                     const EditRoundtripOptions& options);

// Module definitions: what each node type's inputs and outputs are.
//
// Read from .mdf files (the executive's dx.mdf, the UI's ui.mdf, user module
// files) and from macro definitions in .net files. A later definition of the
// same name replaces an earlier one, as in DX.
//
//   MODULE name / CATEGORY / DESCRIPTION / FLAGS
//   INPUT name [visible:0,...]; types; default; description
//   OPTIONS a; b; ...          menu values for the INPUT before it
//   OUTPUT name [...]; types; description
//   REPEAT n                   the last n inputs repeat (e.g. Collect)

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dxui {

struct NetMacro;

struct ModuleParam {
    std::string name;
    std::string types;          // e.g. "scalar field", "vector or string"
    std::string defaultValue;   // e.g. "1", "(none)", "(data mean)"
    std::string description;
    // 1 shown, 0 hidden until exposed, 2 internal (never shown).
    int visible = 1;
    std::vector<std::string> options;
};

struct ModuleDef {
    std::string name, category, description;
    std::vector<ModuleParam> inputs, outputs;
    int repeat = 0;             // REPEAT n
    std::vector<std::string> flags;
    bool isMacro = false;
    std::string source;         // file it came from

    // Input number (1-based) to its definition, following REPEAT for inputs
    // past the declared ones; nullptr when out of range.
    const ModuleParam* input(int index) const;
    const ModuleParam* output(int index) const;
};

class ModuleLibrary {
public:
    // .mdf text; `source` names it in errors and in ModuleDef::source.
    // Returns the number of modules read.
    int loadMdfText(std::string_view text, const std::string& source);
    // Returns false (and sets *error) if the file cannot be read.
    bool loadMdfFile(const std::filesystem::path& path, std::string* error = nullptr);
    // A macro defined in a .net file, as a module: its formal inputs and
    // outputs, category and description.
    void addMacro(const NetMacro& macro, const std::string& source);
    // Every .net in the folders of a DXMACROS-style path (':'-separated) that
    // defines a macro other than main. Returns the number of macros added.
    int loadMacroPath(const std::string& path);

    const ModuleDef* find(std::string_view name) const;
    std::size_t size() const { return modules_.size(); }
    // Names by category, each list sorted, for a palette.
    std::map<std::string, std::vector<std::string>> byCategory() const;

private:
    std::map<std::string, ModuleDef, std::less<>> modules_;
};

} // namespace dxui

#include "ModuleDefs.h"
#include "NetFile.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace dxui {

namespace {

std::string trim(std::string_view s)
{
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos)
        return {};
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

// Splits on ';' into at most `max` fields; the last keeps any further ';'.
std::vector<std::string> fields(std::string_view s, std::size_t max)
{
    std::vector<std::string> out;
    while (out.size() + 1 < max) {
        std::size_t semi = s.find(';');
        if (semi == std::string_view::npos)
            break;
        out.push_back(trim(s.substr(0, semi)));
        s.remove_prefix(semi + 1);
    }
    out.push_back(trim(s));
    return out;
}

// "name [visible:0, cache:2]" -> name and its visible attribute.
void nameAndAttributes(const std::string& field, ModuleParam& p)
{
    std::size_t open = field.find('[');
    p.name = trim(std::string_view(field).substr(0, open));
    if (open == std::string::npos)
        return;
    std::size_t close = field.find(']', open);
    std::string attrs = field.substr(open + 1, close == std::string::npos ? std::string::npos
                                                                          : close - open - 1);
    std::size_t v = attrs.find("visible");
    if (v != std::string::npos) {
        std::size_t colon = attrs.find(':', v);
        if (colon != std::string::npos)
            p.visible = std::atoi(attrs.c_str() + colon + 1);
    }
}

} // namespace

const ModuleParam* ModuleDef::input(int index) const
{
    if (index < 1 || inputs.empty())
        return nullptr;
    auto n = static_cast<std::size_t>(index);
    if (n <= inputs.size())
        return &inputs[n - 1];
    if (repeat <= 0 || static_cast<std::size_t>(repeat) > inputs.size())
        return nullptr;
    // Inputs past the declared ones cycle through the last `repeat`.
    std::size_t first = inputs.size() - static_cast<std::size_t>(repeat);
    return &inputs[first + (n - 1 - first) % static_cast<std::size_t>(repeat)];
}

const ModuleParam* ModuleDef::output(int index) const
{
    if (index < 1 || static_cast<std::size_t>(index) > outputs.size())
        return nullptr;
    return &outputs[static_cast<std::size_t>(index) - 1];
}

int ModuleLibrary::loadMdfText(std::string_view text, const std::string& source)
{
    int count = 0;
    ModuleDef current;
    bool open = false;
    auto finish = [&] {
        if (open && !current.name.empty()) {
            modules_[current.name] = std::move(current);
            ++count;
        }
        current = ModuleDef();
        open = false;
    };

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view raw = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                                            : nl - pos);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#')
            continue;
        std::size_t sp = line.find_first_of(" \t");
        std::string key = line.substr(0, sp);
        std::string rest = sp == std::string::npos ? "" : trim(std::string_view(line).substr(sp));

        if (key == "MODULE") {
            finish();
            current.name = rest;
            current.source = source;
            open = true;
        } else if (!open) {
            continue;
        } else if (key == "CATEGORY") {
            current.category = rest;
        } else if (key == "DESCRIPTION") {
            current.description = rest;
        } else if (key == "FLAGS") {
            std::istringstream words(rest);
            for (std::string w; words >> w;)
                current.flags.push_back(w);
        } else if (key == "REPEAT") {
            current.repeat = std::atoi(rest.c_str());
        } else if (key == "INPUT") {
            auto f = fields(rest, 4);
            ModuleParam p;
            nameAndAttributes(f[0], p);
            if (f.size() > 1) p.types = f[1];
            if (f.size() > 2) p.defaultValue = f[2];
            if (f.size() > 3) p.description = f[3];
            current.inputs.push_back(std::move(p));
        } else if (key == "OUTPUT") {
            auto f = fields(rest, 3);
            ModuleParam p;
            nameAndAttributes(f[0], p);
            if (f.size() > 1) p.types = f[1];
            if (f.size() > 2) p.description = f[2];
            current.outputs.push_back(std::move(p));
        } else if (key == "OPTIONS" && !current.inputs.empty()) {
            for (std::string& o : fields(rest, 1000))
                if (!o.empty())
                    current.inputs.back().options.push_back(std::move(o));
        }
        // OUTBOARD, LOADABLE and the like do not change the interface.
    }
    finish();
    return count;
}

bool ModuleLibrary::loadMdfFile(const fs::path& path, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot read " + path.string();
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    loadMdfText(text.str(), path.string());
    return true;
}

void ModuleLibrary::addMacro(const NetMacro& macro, const std::string& source)
{
    ModuleDef def;
    def.name = macro.name;
    def.category = macro.category.empty() ? "Macros" : macro.category;
    def.description = macro.description;
    def.isMacro = true;
    def.source = source;
    for (const auto& in : macro.formalInputs) {
        ModuleParam p;
        p.name = in.name;
        p.defaultValue = in.defaultValue;
        def.inputs.push_back(std::move(p));
    }
    for (const auto& out : macro.formalOutputs) {
        ModuleParam p;
        p.name = out.name;
        def.outputs.push_back(std::move(p));
    }
    modules_[def.name] = std::move(def);
}

int ModuleLibrary::loadMacroPath(const std::string& path)
{
    int added = 0;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos
                                                                       : colon - start);
        start = colon == std::string::npos ? path.size() + 1 : colon + 1;
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".net")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            NetFile net;
            if (!parseNetFile(f, net))
                continue;
            for (const NetMacro& m : net.macros)
                if (m.name != "main") {
                    addMacro(m, f.string());
                    ++added;
                }
        }
    }
    return added;
}

const ModuleDef* ModuleLibrary::find(std::string_view name) const
{
    auto it = modules_.find(name);
    return it == modules_.end() ? nullptr : &it->second;
}

std::map<std::string, std::vector<std::string>> ModuleLibrary::byCategory() const
{
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& [name, def] : modules_)
        out[def.category.empty() ? "Other" : def.category].push_back(name);
    return out;   // names arrive sorted from the map
}

} // namespace dxui

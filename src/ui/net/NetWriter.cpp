#include "NetWriter.h"
#include "NetFile.h"

#include <cmath>
#include <fstream>
#include <regex>
#include <vector>

namespace fs = std::filesystem;

namespace dxui {

namespace {

void fail(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
}

} // namespace

bool writeNodePositions(std::string_view original, const NetMacro& macro, std::string& out,
                        std::string* error)
{
    // Lines with their endings, so joining them restores the text exactly.
    std::vector<std::string> lines;
    for (std::size_t pos = 0; pos < original.size();) {
        std::size_t nl = original.find('\n', pos);
        std::size_t end = nl == std::string_view::npos ? original.size() : nl + 1;
        lines.emplace_back(original.substr(pos, end - pos));
        pos = end;
    }

    // `// node Module[inst]: x = 12, y = 34, ...`; groups: prefix, x, middle, y.
    static const std::regex nodeLine(
        R"(^(\s*//\s*node\s+(\w+)\[(\d+)\]:\s*x\s*=\s*)(-?\d+)(\s*,\s*y\s*=\s*)(-?\d+))");
    for (const NetNode& node : macro.nodes) {
        const std::string what = node.module + "[" + std::to_string(node.instance) + "]";
        if (node.line < 1 || static_cast<std::size_t>(node.line) > lines.size()) {
            fail(error, what + ": no node comment at line " + std::to_string(node.line));
            return false;
        }
        std::string& line = lines[static_cast<std::size_t>(node.line) - 1];
        std::smatch m;
        if (!std::regex_search(line, m, nodeLine) || m[2] != node.module ||
            std::stoi(m[3]) != node.instance) {
            fail(error, what + ": line " + std::to_string(node.line) +
                            " is not its node comment");
            return false;
        }
        const long x = std::lround(node.x), y = std::lround(node.y);
        line = m[1].str() + std::to_string(x) + m[5].str() + std::to_string(y) +
               m.suffix().str();
    }

    out.clear();
    out.reserve(original.size() + 64);
    for (const std::string& l : lines)
        out += l;
    return true;
}

bool saveTextAtomically(const fs::path& path, std::string_view text, std::string* error)
{
    fs::path tmp = path;
    tmp += ".dxui2-save";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            fail(error, "cannot write " + tmp.string());
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        if (!f) {
            fail(error, "error writing " + tmp.string());
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    if (fs::exists(path, ec))
        fs::permissions(tmp, fs::status(path, ec).permissions(), ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        fail(error, "cannot replace " + path.string() + ": " + ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace dxui

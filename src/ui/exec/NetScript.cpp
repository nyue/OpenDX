#include "NetScript.h"

#include <cctype>
#include <regex>

namespace dxui {

namespace {

// DX script string literal for a path.
std::string quoted(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

// Display's inputs (object, camera, where, throttle, and four more in newer
// DX versions, unused here) after the picture's id; a macro takes fewer
// arguments than it declares, so both forms fit. "is null + 1" is 1 for a camera (render) and 2 for none (the
// object is already an image). frame 0 overwrites, where "rgb" would append.
// DxuiShow replaces calls whose result is unused (a macro output nobody reads
// draws a warning); DxuiDisplay replaces the ones assigned to a variable.
std::string displayMacros(const std::filesystem::path& imageDir)
{
    const std::string body =
        "{\n"
        "    dxuiNoCamera = Inquire(camera, \"is null + 1\");\n"
        "    dxuiRendered = Render(object, camera);\n"
        "    dxuiImage = Switch(dxuiNoCamera, dxuiRendered, object);\n"
        "    dxuiName = Format(" + quoted(imageDir.string() + "/%s") + ", id);\n"
        "    WriteImage(dxuiImage, dxuiName, \"rgb\", 0);\n";
    // DxuiCamera reports an Image's camera to the UI (DXLOutput), so mouse
    // interaction can start from it: labels "dxuicam:<id>:<what>". Width is
    // only defined for orthographic cameras and angle for perspective ones;
    // Switch evaluates only the input it selects.
    const std::string camera =
        "macro DxuiCamera(id, camera) -> ()\n"
        "{\n"
        "    dxuiCamTo = Inquire(camera, \"camera to\");\n"
        "    dxuiCamFrom = Inquire(camera, \"camera from\");\n"
        "    dxuiCamUp = Inquire(camera, \"camera up\");\n"
        "    dxuiCamPersp = Inquire(camera, \"camera perspective\");\n"
        "    dxuiCamWhich = Compute(\"$0+1\", dxuiCamPersp);\n"
        "    dxuiCamWidth = Inquire(camera, \"camera width\");\n"
        "    dxuiCamAngle = Inquire(camera, \"camera angle\");\n"
        "    dxuiCamSize = Switch(dxuiCamWhich, dxuiCamWidth, dxuiCamAngle);\n"
        "    dxuiLabelTo = Format(\"dxuicam:%s:to\", id);\n"
        "    dxuiLabelFrom = Format(\"dxuicam:%s:from\", id);\n"
        "    dxuiLabelUp = Format(\"dxuicam:%s:up\", id);\n"
        "    dxuiLabelPersp = Format(\"dxuicam:%s:perspective\", id);\n"
        "    dxuiLabelSize = Format(\"dxuicam:%s:size\", id);\n"
        "    DXLOutput(dxuiLabelTo, dxuiCamTo);\n"
        "    DXLOutput(dxuiLabelFrom, dxuiCamFrom);\n"
        "    DXLOutput(dxuiLabelUp, dxuiCamUp);\n"
        "    DXLOutput(dxuiLabelPersp, dxuiCamPersp);\n"
        "    DXLOutput(dxuiLabelSize, dxuiCamSize);\n"
        "}\n";
    return camera +
           "macro DxuiShow(id, object, camera, where, throttle, x5, x6, x7, x8) -> ()\n" + body + "}\n" +
           "macro DxuiDisplay(id, object, camera, where, throttle, x5, x6, x7, x8)"
           " -> (whereOut)\n" + body +
           "    whereOut = where;\n}\n";
}

} // namespace

NetScript netScriptForUi(std::string_view netText, const std::filesystem::path& imageDir)
{
    NetScript out;
    out.text = displayMacros(imageDir);

    // Copy the file line by line, rewriting Display( calls in two places:
    //  - inside `macro Image(...)` wrappers: DxuiShow/DxuiDisplay(id, ...), the
    //    Image node's id input naming the picture;
    //  - in a network, the call of a Display node (after its `// node
    //    Display[N]:` comment): the same with "Display_N" as the id.
    // A call whose result is assigned (`x =` before it, on this line or the
    // previous one) becomes DxuiDisplay, otherwise DxuiShow.
    static const std::regex displayCall(R"(\bDisplay\s*\()");
    static const std::regex nodeComment(R"(^\s*//\s*node\s+(\w+)\[(\d+)\])");
    static const std::regex callStart(R"(^\s*Display\s*\()");
    bool inImage = false;
    std::string node;          // "Display_3" while inside a Display node's block
    std::string previous;      // previous non-blank line
    std::size_t pos = 0;
    while (pos < netText.size()) {
        std::size_t nl = netText.find('\n', pos);
        std::size_t end = nl == std::string_view::npos ? netText.size() : nl + 1;
        std::string line(netText.substr(pos, end - pos));
        pos = end;

        std::smatch m;
        if (!inImage && line.rfind("macro Image(", 0) == 0) {
            inImage = true;
        } else if (inImage && line.rfind("}", 0) == 0) {
            // Report the camera before the wrapper ends (`camera` is one of
            // the Image macro's outputs in every DX version's wrapper).
            out.text += "    DxuiCamera(id, camera);\n";
            // The rendered object as a scene for the UI's GPU view (ADR 0003);
            // `object` is the Image macro's renderable output.
            out.text += "    dxuiSceneName = Format(" + quoted(imageDir.string() + "/%s.scene") +
                        ", id);\n    DxuiScene(object, dxuiSceneName);\n";
            inImage = false;
        } else if (std::regex_search(line, m, nodeComment)) {
            node = m[1] == "Display" ? "Display_" + m[2].str() : "";
        } else if (inImage || !node.empty()) {
            auto begin = std::sregex_iterator(line.begin(), line.end(), displayCall);
            int n = static_cast<int>(std::distance(begin, std::sregex_iterator()));
            if (n) {
                std::string trimmed = previous;
                while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
                    trimmed.pop_back();
                bool assigned = !std::regex_search(line, callStart) ||
                                (!trimmed.empty() && trimmed.back() == '=');
                std::string id = inImage ? "id" : "\"" + node + "\"";
                std::string repl = (assigned ? "DxuiDisplay(" : "DxuiShow(") + id + ", ";
                line = std::regex_replace(line, displayCall, repl);
                out.displayCallsRewritten += n;
                if (!inImage)
                    node.clear();   // one call per node
            }
        }
        if (line.find_first_not_of(" \t\r\n") != std::string::npos)
            previous = line;
        out.text += line;
    }
    if (!out.text.empty() && out.text.back() != '\n')
        out.text += '\n';
    return out;
}

} // namespace dxui

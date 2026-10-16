// Draws one macro of a .net file with noodles into an offscreen texture.
//
// The app shows the texture in an ImGui panel and forwards that panel's mouse
// input, so the graph lives inside the docking layout like any other panel.
// Requires a current OpenGL 3.3 core context; call init() once after creating
// it (it initialises GLEW, which noodles uses).

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dxui {

struct NetMacro;
class ModuleLibrary;

// Mouse state for the graph panel, in pixels relative to its top-left corner.
struct GraphInput {
    float x = 0, y = 0;
    bool hovered = false;      // pointer is over the panel
    bool leftDown = false, leftClicked = false, leftDoubleClicked = false;
    bool middleDown = false, rightDown = false;
    float dragDx = 0, dragDy = 0;  // pointer movement this frame while a button is held
    float wheel = 0;           // scroll steps this frame (positive = zoom in)
    bool altDown = false;
};

// A finished drag of one node, in DX workspace pixels (as the .net stores them).
struct NodeMove {
    int node;                  // index into NetMacro::nodes
    int fromX, fromY, toX, toY;
};

// A wiring gesture the view asks the app to apply (ports 1-based).
struct GraphEdit {
    enum class Kind { Connect, Disconnect };
    Kind kind;
    int fromNode = -1, fromPort = -1;   // Connect only
    int toNode = -1, toPort = -1;
};

class GraphView {
public:
    GraphView();
    ~GraphView();
    GraphView(const GraphView&) = delete;
    GraphView& operator=(const GraphView&) = delete;

    // assetsDir holds noodles' shaders/ and fonts/. Returns false with error() set.
    bool init(const std::filesystem::path& assetsDir);

    // The macro to draw; nullptr clears. The pointer must stay valid until the
    // next setMacro. Frames the whole graph.
    void setMacro(const NetMacro* macro);
    // Module definitions, for pin names and which inputs show; may be null.
    // Must outlive the view or be replaced. Rebuilds the graph.
    void setLibrary(const ModuleLibrary* library);

    // Pan and zoom from the panel's input. Left press on a node selects it and,
    // once the pointer has moved a few pixels, drags it; on empty space it pans.
    void handleInput(const GraphInput& input);
    // Drags finished since the last call. The caller updates the model.
    std::vector<NodeMove> takeMoves();
    // Places a node at DX workspace coordinates, e.g. for undo or redo.
    void setNodePosition(int node, int x, int y);
    // Wiring gestures since the last call: drag from an output to an input to
    // connect; drag from a wired input to empty space to disconnect.
    std::vector<GraphEdit> takeEdits();
    // While a wire is being dragged: its ends in panel pixels.
    bool wirePreview(float& x1, float& y1, float& x2, float& y2) const;
    // DX workspace coordinates of a point in the panel (for dropping nodes).
    void panelToWorkspace(float sx, float sy, int& x, int& y) const;
    // Rebuilds after the macro was edited, keeping the view and selection.
    void refresh();
    void select(int node);
    void frameAll();

    // Renders at width x height pixels and returns the GL texture name.
    unsigned int render(int width, int height);

    // Node index into NetMacro::nodes, or -1.
    int selectedNode() const;
    int hoveredNode() const;
    // Marks a node as failed (drawn distinctly), e.g. from an exec error; -1 clears.
    void setErrorNode(int node);

    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dxui

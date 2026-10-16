// GraphView: one macro of a .net file drawn by noodles into an offscreen
// texture (see GraphView.h).
//
// Mapping from the DX model to noodles:
//   - DX lays a program out top to bottom (inputs on a node's top edge, outputs
//     on the bottom); noodles draws inputs on the left and outputs on the right.
//     So the workspace is transposed -- DX y becomes the noodles x axis and DX x
//     becomes y -- which keeps the data flowing in the direction the wires leave
//     and enter nodes. Positions are scaled to noodles' world units.
//   - One noodles row per port pair: input i and output i share row i (noodles'
//     identity slots), pins named "in N" / "out N" (1-based, as in the file).
//   - Inputs the file marks `visible = 0` are hidden unless wired, as the
//     legacy editor does. Nodes with many inputs (Image has 48) show only the
//     wired and explicitly visible ones; the subtitle says how many are hidden.
//   - Page groups are separate canvases in DX with overlapping coordinates, so
//     each page is laid out in its own cell of a grid, on a labelled backdrop.
//
// Colours avoid red/green contrasts (the author is Protan): selection is blue,
// an execution error is orange, wireless (Transmitter/Receiver) nodes violet.

#include "GraphView.h"

#include "ModuleDefs.h"
#include "NetEdit.h"
#include "NetFile.h"

#include "core/NodeData.h"
#include "core/RenderConfig.h"
#include "render/FontAtlas.h"
#include "render/LinkRenderManager.h"
#include "render/NodeRenderManager.h"
#include "render/NodeTransformFrame.h"
#include "render/ShaderLibrary.h"
#include "render/StickerRenderer.h"
#include "render/TextRenderManager.h"

#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxui {

namespace {

// DX workspace pixels -> noodles world units, along the data flow (DX y) and
// across it (DX x). Noodles nodes are wider than DX's, hence the larger first.
constexpr double kScaleAlongFlow = 3.6, kScaleAcrossFlow = 3.0;
// Nodes with up to this many inputs show every input not hidden by the file;
// Spacing between page cells, world units.
constexpr double kPageGap = 260.0;
constexpr double kPagePadding = 70.0;
constexpr double kPageLabelSize = 64.0;
// Width / height the page grid aims for (the Network panel is landscape).
constexpr double kPanelAspect = 1.5;
constexpr double kMinZoom = 0.02, kMaxZoom = 4.0;

// Protan-safe palette.
constexpr std::array<float, 3> kTitleColour{0.28f, 0.34f, 0.45f};      // slate blue
constexpr std::array<float, 3> kWirelessColour{0.44f, 0.36f, 0.62f};   // violet
constexpr std::array<float, 3> kErrorColour{0.86f, 0.46f, 0.06f};      // orange
constexpr std::array<float, 4> kErrorHalo{1.00f, 0.58f, 0.12f, 1.0f};
constexpr std::array<float, 4> kHoverHalo{0.45f, 0.70f, 1.00f, 0.35f};
constexpr std::array<float, 4> kSelectStroke{0.35f, 0.66f, 1.00f, 1.0f};
constexpr float kLinkColour[3] = {0.62f, 0.68f, 0.78f};
constexpr float kLinkSelected[3] = {0.40f, 0.72f, 1.00f};
constexpr float kLinkHovered[3] = {0.75f, 0.85f, 1.00f};

std::string inputPin(int i) { return "in " + std::to_string(i); }
std::string outputPin(int i) { return "out " + std::to_string(i); }
std::string nodeId(std::size_t i) { return "n" + std::to_string(i); }

// Everything the ImGui OpenGL3 backend (and the app's own clear) relies on.
class GLStateGuard {
public:
    GLStateGuard()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo_);
        glGetIntegerv(GL_VIEWPORT, viewport_);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        for (int unit = 0; unit < 2; ++unit) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_[unit]);
        }
        glActiveTexture(static_cast<GLenum>(activeTexture_));
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha_);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb_);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha_);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc_);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask_);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColour_);
        blend_ = glIsEnabled(GL_BLEND);
        scissor_ = glIsEnabled(GL_SCISSOR_TEST);
        depth_ = glIsEnabled(GL_DEPTH_TEST);
        cull_ = glIsEnabled(GL_CULL_FACE);
        stencil_ = glIsEnabled(GL_STENCIL_TEST);
    }

    ~GLStateGuard()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo_));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo_));
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissorBox_[0], scissorBox_[1], scissorBox_[2], scissorBox_[3]);
        glUseProgram(static_cast<GLuint>(program_));
        glBindVertexArray(static_cast<GLuint>(vao_));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer_));
        for (int unit = 0; unit < 2; ++unit) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_[unit]));
        }
        glActiveTexture(static_cast<GLenum>(activeTexture_));
        glBlendEquationSeparate(static_cast<GLenum>(blendEqRgb_),
                                static_cast<GLenum>(blendEqAlpha_));
        glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb_), static_cast<GLenum>(blendDstRgb_),
                            static_cast<GLenum>(blendSrcAlpha_),
                            static_cast<GLenum>(blendDstAlpha_));
        glDepthFunc(static_cast<GLenum>(depthFunc_));
        glDepthMask(depthMask_);
        glClearColor(clearColour_[0], clearColour_[1], clearColour_[2], clearColour_[3]);
        set(GL_BLEND, blend_);
        set(GL_SCISSOR_TEST, scissor_);
        set(GL_DEPTH_TEST, depth_);
        set(GL_CULL_FACE, cull_);
        set(GL_STENCIL_TEST, stencil_);
    }

    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;

private:
    static void set(GLenum cap, GLboolean on)
    {
        if (on)
            glEnable(cap);
        else
            glDisable(cap);
    }

    GLint drawFbo_ = 0, readFbo_ = 0, viewport_[4] = {}, scissorBox_[4] = {};
    GLint program_ = 0, vao_ = 0, arrayBuffer_ = 0, activeTexture_ = GL_TEXTURE0;
    GLint texture_[2] = {};
    GLint blendSrcRgb_ = GL_ONE, blendDstRgb_ = GL_ZERO, blendSrcAlpha_ = GL_ONE,
          blendDstAlpha_ = GL_ZERO, blendEqRgb_ = GL_FUNC_ADD, blendEqAlpha_ = GL_FUNC_ADD;
    GLint depthFunc_ = GL_LESS;
    GLboolean depthMask_ = GL_TRUE;
    GLfloat clearColour_[4] = {};
    GLboolean blend_ = GL_FALSE, scissor_ = GL_FALSE, depth_ = GL_FALSE, cull_ = GL_FALSE,
              stencil_ = GL_FALSE;
};

struct Rect {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    bool empty() const { return x0 > x1; }
    void add(double x, double y)
    {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void add(const Rect& r)
    {
        if (!r.empty()) {
            add(r.x0, r.y0);
            add(r.x1, r.y1);
        }
    }
};

} // namespace

struct GraphView::Impl {
    std::string error;
    bool ready = false;

    noodles::ShaderLibrary shaders;
    noodles::FontAtlas font;
    noodles::NodeTransformFrame frame;
    noodles::NodeRenderManager nodeManager;
    noodles::LinkRenderManager linkManager;
    noodles::TextRenderManager textManager;
    noodles::StickerRenderer stickerRenderer;
    noodles::RenderConfig config;
    noodles::GraphModel graph;

    const NetMacro* macro = nullptr;
    const ModuleLibrary* library = nullptr;
    std::vector<std::string> ids;  // NetMacro node index -> noodles id
    std::unordered_map<std::string, int> indexOf;

    struct Page {
        std::string name;
        Rect box;  // backdrop, world units
    };
    std::vector<Page> pages;
    std::vector<float> pageLabelVerts;

    GLuint fbo = 0, colourTex = 0, depthRb = 0;
    int fbWidth = 0, fbHeight = 0;

    // Camera: world = pan + screen / zoom, y down.
    double panX = 0, panY = 0, zoom = 0.3;
    bool contentChanged = true;
    bool framePending = false;

    int selected = -1, hovered = -1, errorNode = -1;
    bool leftPanning = false, middlePanning = false, prevMiddle = false;
    float leftTravel = 0;  // pointer movement since the left press, pixels

    // Per node: the page cell offset added in arrangePages (world units) and
    // the page it sits on (-1 for a single-page network).
    std::vector<std::array<double, 2>> offset;
    std::vector<int> pageOf;

    // Dragging a node: armed by a press on it, moving once past a few pixels.
    int dragNode = -1;
    bool dragging = false;
    double pressWorldX = 0, pressWorldY = 0;
    noodles::Vec2d dragStart{0, 0};
    std::vector<NodeMove> moves;

    // Pin name -> DX port number, per node (pins are named by pinName()).
    std::vector<std::unordered_map<std::string, int>> inPortOf, outPortOf;

    // Dragging a wire from a port.
    struct Wire {
        bool active = false;
        int node = -1, port = -1;
        bool fromOutput = true;
        int wiredFrom = -1, wiredFromPort = -1;  // input already wired: its source
        double ax = 0, ay = 0;                    // anchor (world)
        double cx = 0, cy = 0;                    // pointer (world)
    } wire;
    std::vector<GraphEdit> edits;

    ~Impl()
    {
        if (fbo)
            glDeleteFramebuffers(1, &fbo);
        if (colourTex)
            glDeleteTextures(1, &colourTex);
        if (depthRb)
            glDeleteRenderbuffers(1, &depthRb);
    }

    void configure()
    {
        noodles::RenderConfig& c = config;
        c.nodeTitleFontSize = 30.0;
        c.nodePinFontSize = 22.0;
        c.nodeMarginH = 14.0;
        c.nodeMarginV = 8.0;
        c.nodePortSpacing = 4.0;
        c.nodePortWidth = 22.0;
        c.nodePortRadius = 6.0;
        c.nodePortRingThickness = 2.0;
        c.nodeCornerRadius = 10.0;
        c.selectedNodeStrokeWidth = 6.0;
        c.selectedNodeStrokeColor = kSelectStroke;
        c.showPinTypeLabels = false;
        c.showDisconnectedPorts = true;
        c.portConnectedFillColor = {0.78f, 0.83f, 0.92f, 1.0f};
        c.portConnectedRingColor = {1.0f, 1.0f, 1.0f, 0.0f};
        c.portDisconnectedFillColor = {0.20f, 0.22f, 0.26f, 1.0f};
        c.portDisconnectedRingColor = {0.55f, 0.62f, 0.74f, 1.0f};
        c.portHoverColor = {0.45f, 0.72f, 1.0f, 1.0f};
        c.backgroundClearColor = {0.12f, 0.12f, 0.14f, 1.0f};
        c.linkLineWidth = 6.0;
    }

    bool createFramebuffer(int w, int h)
    {
        if (fbo && w == fbWidth && h == fbHeight)
            return true;
        if (!fbo) {
            glGenFramebuffers(1, &fbo);
            glGenTextures(1, &colourTex);
            glGenRenderbuffers(1, &depthRb);
        }
        glBindTexture(GL_TEXTURE_2D, colourTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colourTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  depthRb);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            error = "graph framebuffer incomplete";
            return false;
        }
        fbWidth = w;
        fbHeight = h;
        return true;
    }

    // --- building the graph -------------------------------------------------

    const ModuleDef* defOf(std::size_t i) const
    {
        return library && macro ? library->find(macro->nodes[i].module) : nullptr;
    }

    // Pin names double as pin ids for links: the definition's name, numbered
    // where it repeats (Collect's "object 2"); "in k"/"out k" without one.
    static std::string pinName(const ModuleDef* def, int k, bool output)
    {
        const ModuleParam* param = def ? (output ? def->output(k) : def->input(k)) : nullptr;
        if (!param || param->name.empty())
            return output ? outputPin(k) : inputPin(k);
        const auto& list = output ? def->outputs : def->inputs;
        const auto declared = static_cast<int>(list.size());
        int same = 0;
        for (const ModuleParam& q : list)
            same += q.name == param->name;
        if (k > declared || same > 1)
            return param->name + " " + std::to_string(k);
        return param->name;
    }

    void build()
    {
        graph.clear();
        ids.clear();
        indexOf.clear();
        pages.clear();
        pageLabelVerts.clear();
        if (!macro)
            return;
        const NetMacro& m = *macro;
        const std::size_t n = m.nodes.size();

        // Wired ports per node, and the highest port number each link needs.
        std::vector<std::set<int>> wiredIn(n), wiredOut(n);
        std::vector<int> needIn(n, 0), needOut(n, 0);
        for (const NetLink& l : m.links) {
            if (l.fromNode >= n || l.toNode >= n || l.fromOutput < 1 || l.toInput < 1)
                continue;
            wiredOut[l.fromNode].insert(l.fromOutput);
            wiredIn[l.toNode].insert(l.toInput);
            needOut[l.fromNode] = std::max(needOut[l.fromNode], l.fromOutput);
            needIn[l.toNode] = std::max(needIn[l.toNode], l.toInput);
        }

        ids.reserve(n);
        offset.assign(n, {0.0, 0.0});
        pageOf.assign(n, -1);
        inPortOf.assign(n, {});
        outPortOf.assign(n, {});
        for (std::size_t i = 0; i < n; ++i) {
            const NetNode& src = m.nodes[i];
            noodles::NodeData d;
            d.id = nodeId(i);
            d.name = src.label.empty() ? src.module : src.label;
            if (d.name.empty())
                d.name = "?";
            d.type = src.module;

            const bool wireless = src.module == "Transmitter" || src.module == "Receiver";
            const auto& tint = i == static_cast<std::size_t>(errorNode)
                                   ? kErrorColour
                                   : (wireless ? kWirelessColour : kTitleColour);
            d.displayColorR = tint[0];
            d.displayColorG = tint[1];
            d.displayColorB = tint[2];

            // Inputs, as DX shows them: a wired input always; otherwise the
            // file's `visible =` if it lists the input, else the module
            // definition's (visible:1). Without a definition, a node with many
            // inputs shows only wired or explicitly visible ones.
            const ModuleDef* def = defOf(i);
            const int defIn = def ? static_cast<int>(def->inputs.size()) : 0;
            const int defOut = def ? static_cast<int>(def->outputs.size()) : 0;
            const int numIn = std::clamp(std::max({src.numInputs, needIn[i], defIn}), 0, 4096);
            int hidden = 0;
            for (int k = 1; k <= numIn; ++k) {
                // The same rule Details uses (NetEdit inputShown); links that
                // need a port beyond the node's inputs still get one.
                bool show = wiredIn[i].count(k) > 0 ||
                            inputShown(m, static_cast<int>(i), k, def);
                if (show) {
                    d.originalInputPins.push_back(pinName(def, k, false));
                    inPortOf[i][d.originalInputPins.back()] = k;
                } else {
                    ++hidden;
                }
            }
            // Some modules (Route, Switch) take more outputs than they declare.
            const int numOut = std::clamp(std::max({src.numOutputs, needOut[i], defOut}), 0, 4096);
            for (int k = 1; k <= numOut; ++k) {
                d.originalOutputPins.push_back(pinName(def, k, true));
                outPortOf[i][d.originalOutputPins.back()] = k;
            }

            std::string subtitle;
            if (!src.label.empty() && src.label != src.module)
                subtitle = src.module;
            if (hidden > 0) {
                if (!subtitle.empty())
                    subtitle += ", ";
                subtitle += "+" + std::to_string(hidden) + " hidden input" + (hidden > 1 ? "s" : "");
            }
            d.schemaTypeName = subtitle;

            if (static_cast<int>(i) == selected) {
                d.selected = true;
                d.zOrder = 1;
            }

            // Transposed: DX flows down, noodles flows right.
            d.position = noodles::Vec2d(static_cast<double>(src.y) * kScaleAlongFlow,
                                        static_cast<double>(src.x) * kScaleAcrossFlow);
            ids.push_back(d.id);
            indexOf[d.id] = static_cast<int>(i);
            graph.nodes.emplace(d.id, std::move(d));
        }

        for (const NetLink& l : m.links) {
            if (l.fromNode >= n || l.toNode >= n || l.fromOutput < 1 || l.toInput < 1)
                continue;
            noodles::LinkData link;
            link.sourceNodeId = ids[l.fromNode];
            link.sourcePort = pinName(defOf(l.fromNode), l.fromOutput, true);
            link.targetNodeId = ids[l.toNode];
            link.targetPort = pinName(defOf(l.toNode), l.toInput, false);
            link.targetPropertyName = link.targetPort;
            graph.links.push_back(std::move(link));
        }

        const noodles::FontMetrics metrics{font.ascender(), font.descender(), font.lineHeight()};
        for (auto& entry : graph.nodes) {
            graph.layoutNode(
                entry.second,
                [this](const std::string& text, double size) {
                    return textManager.calculateTextWidth(text, size);
                },
                metrics, &config);
        }
        arrangePages();
        updateLinks();
        graph.markLinksChanged();
        graph.buildConnectionCache();

        frame.reset();
        nodeManager.resetNodeQuadCaches();
        textManager.resetPositionCaches();
        contentChanged = true;
    }

    // DX pages share one coordinate space; give each its own cell.
    void arrangePages()
    {
        if (!macro)
            return;
        const NetMacro& m = *macro;
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::size_t>> members;
        for (std::size_t i = 0; i < m.nodes.size(); ++i) {
            const std::string& g = m.nodes[i].pageGroup;
            if (!members.count(g))
                order.push_back(g);
            members[g].push_back(i);
        }
        if (order.size() < 2)
            return;

        auto boxOf = [&](const std::vector<std::size_t>& nodes) {
            Rect r;
            for (std::size_t i : nodes) {
                const noodles::NodeData& d = graph.nodes.at(ids[i]);
                r.add(d.position[0], d.position[1]);
                r.add(d.position[0] + d.size[0], d.position[1] + d.size[1]);
            }
            return r;
        };

        // Backdrop size per page: padding around its nodes plus a label strip.
        const double top = kPagePadding + kPageLabelSize * 1.4;
        std::vector<Rect> content;
        std::vector<double> pw, ph;
        for (const std::string& g : order) {
            content.push_back(boxOf(members[g]));
            pw.push_back(content.back().x1 - content.back().x0 + 2 * kPagePadding);
            ph.push_back(content.back().y1 - content.back().y0 + top + kPagePadding);
        }
        // Rows of `cols` pages; pick the column count whose overall extent
        // best fills a typical landscape panel.
        auto place = [&](std::size_t cols, std::vector<std::array<double, 2>>* at) {
            double x = 0, y = 0, rowH = 0, width = 0;
            for (std::size_t p = 0; p < order.size(); ++p) {
                if (p > 0 && p % cols == 0) {
                    x = 0;
                    y += rowH + kPageGap;
                    rowH = 0;
                }
                if (at)
                    at->push_back({x, y});
                width = std::max(width, x + pw[p]);
                x += pw[p] + kPageGap;
                rowH = std::max(rowH, ph[p]);
            }
            return std::max(width / kPanelAspect, y + rowH);
        };
        std::size_t bestCols = 1;
        double bestScore = place(1, nullptr);
        for (std::size_t c = 2; c <= order.size(); ++c) {
            const double score = place(c, nullptr);
            if (score < bestScore) {
                bestScore = score;
                bestCols = c;
            }
        }
        std::vector<std::array<double, 2>> at;
        place(bestCols, &at);

        for (std::size_t p = 0; p < order.size(); ++p) {
            const Rect& r = content[p];
            const double dx = at[p][0] + kPagePadding - r.x0;
            const double dy = at[p][1] + top - r.y0;
            for (std::size_t i : members[order[p]]) {
                noodles::NodeData& d = graph.nodes.at(ids[i]);
                d.position = noodles::Vec2d(d.position[0] + dx, d.position[1] + dy);
                offset[i] = {dx, dy};
                pageOf[i] = static_cast<int>(p);
            }
            Page page;
            page.name = order[p].empty() ? std::string("(no page)") : order[p];
            page.box.add(at[p][0], at[p][1]);
            page.box.add(at[p][0] + pw[p], at[p][1] + ph[p]);
            pages.push_back(page);
        }

        for (const Page& page : pages)
            textManager.generateTextVertices(page.name, page.box.x0 + kPagePadding * 0.5,
                                             page.box.y0 + kPagePadding * 0.5 + kPageLabelSize,
                                             -0.04f, kPageLabelSize, pageLabelVerts, -1.0f);
    }

    static double pinCentreY(const noodles::NodeData& node, const std::string& pin, bool output)
    {
        const auto& pins = output ? node.outputPins : node.inputPins;
        const auto& centres = output ? node.layoutOutputCenterY : node.layoutInputCenterY;
        for (std::size_t i = 0; i < pins.size(); ++i)
            if (pins[i] == pin)
                return node.position[1] + (i < centres.size() ? centres[i] : node.size[1] * 0.5);
        return node.position[1] + node.size[1] * 0.5;
    }

    void updateLinks()
    {
        const std::string sel = selected >= 0 ? ids[static_cast<std::size_t>(selected)] : "";
        for (noodles::LinkData& link : graph.links) {
            auto src = graph.nodes.find(link.sourceNodeId);
            auto dst = graph.nodes.find(link.targetNodeId);
            if (src == graph.nodes.end() || dst == graph.nodes.end()) {
                link.isDangling = true;
                continue;
            }
            link.start = noodles::Vec2d(src->second.position[0] + src->second.size[0],
                                        pinCentreY(src->second, link.sourcePort, true));
            link.end = noodles::Vec2d(dst->second.position[0],
                                      pinCentreY(dst->second, link.targetPort, false));
            link.selected = !sel.empty() && (link.sourceNodeId == sel || link.targetNodeId == sel);
        }
        graph.markLinksChanged();
    }

    // DX workspace <-> world, through the transpose, the scales and the page
    // cell offset (the inverse of build() and arrangePages()).
    noodles::Vec2d worldFromDx(std::size_t i, double x, double y) const
    {
        return noodles::Vec2d(y * kScaleAlongFlow + offset[i][0],
                              x * kScaleAcrossFlow + offset[i][1]);
    }
    void dxFromWorld(std::size_t i, const noodles::Vec2d& w, int& x, int& y) const
    {
        x = static_cast<int>(std::lround((w[1] - offset[i][1]) / kScaleAcrossFlow));
        y = static_cast<int>(std::lround((w[0] - offset[i][0]) / kScaleAlongFlow));
    }

    // After positions change: links follow, caches rebuild, and the node's
    // page backdrop grows to keep it inside.
    void moved(std::size_t i)
    {
        if (pageOf[i] >= 0 && static_cast<std::size_t>(pageOf[i]) < pages.size()) {
            const noodles::NodeData& d = graph.nodes.at(ids[i]);
            Rect& box = pages[static_cast<std::size_t>(pageOf[i])].box;
            box.add(d.position[0] - kPagePadding, d.position[1] - kPagePadding);
            box.add(d.position[0] + d.size[0] + kPagePadding,
                    d.position[1] + d.size[1] + kPagePadding);
        }
        frame.reset();
        nodeManager.resetNodeQuadCaches();
        textManager.resetPositionCaches();
        updateLinks();
        contentChanged = true;
    }

    // Selection, error and hover change appearance only; positions stay put.
    void restyle()
    {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            noodles::NodeData& d = graph.nodes.at(ids[i]);
            const bool isSel = static_cast<int>(i) == selected;
            d.selected = isSel;
            d.zOrder = isSel ? 1 : 0;
            const bool wireless = d.type == "Transmitter" || d.type == "Receiver";
            const auto& tint = static_cast<int>(i) == errorNode
                                   ? kErrorColour
                                   : (wireless ? kWirelessColour : kTitleColour);
            d.displayColorR = tint[0];
            d.displayColorG = tint[1];
            d.displayColorB = tint[2];
        }
        updateLinks();
        contentChanged = true;
    }

    // --- camera ---------------------------------------------------------------

    Rect worldBounds() const
    {
        Rect r;
        for (const auto& entry : graph.nodes) {
            const noodles::NodeData& d = entry.second;
            r.add(d.position[0], d.position[1]);
            r.add(d.position[0] + d.size[0], d.position[1] + d.size[1]);
        }
        for (const Page& p : pages)
            r.add(p.box);
        return r;
    }

    void doFrameAll(int w, int h)
    {
        const Rect r = worldBounds();
        if (r.empty() || w <= 0 || h <= 0) {
            zoom = 0.3;
            panX = panY = 0;
            return;
        }
        const double zx = w / std::max(1.0, (r.x1 - r.x0) * 1.08);
        const double zy = h / std::max(1.0, (r.y1 - r.y0) * 1.08);
        zoom = std::clamp(std::min(zx, zy), kMinZoom, 1.0);
        panX = (r.x0 + r.x1) * 0.5 - w / (2.0 * zoom);
        panY = (r.y0 + r.y1) * 0.5 - h / (2.0 * zoom);
    }

    void screenToWorld(double sx, double sy, double& wx, double& wy) const
    {
        wx = panX + sx / zoom;
        wy = panY + sy / zoom;
    }

    void zoomAt(double sx, double sy, double steps)
    {
        double bx, by, ax, ay;
        screenToWorld(sx, sy, bx, by);
        zoom = std::clamp(zoom * std::pow(1.15, steps), kMinZoom, kMaxZoom);
        screenToWorld(sx, sy, ax, ay);
        panX += bx - ax;
        panY += by - ay;
    }

    int nodeAt(double sx, double sy) const
    {
        double wx, wy;
        screenToWorld(sx, sy, wx, wy);
        int best = -1, bestZ = -1;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const noodles::NodeData& d = graph.nodes.at(ids[i]);
            if (wx >= d.position[0] && wx <= d.position[0] + d.size[0] && wy >= d.position[1] &&
                wy <= d.position[1] + d.size[1] && d.zOrder > bestZ) {
                bestZ = d.zOrder;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    // The port within a few pixels of a panel point: node, DX port number,
    // output or input, and the port's world position.
    struct PortHit {
        int node = -1, port = -1;
        bool output = false;
        double x = 0, y = 0;
    };
    PortHit portAt(double sx, double sy) const
    {
        double wx, wy;
        screenToWorld(sx, sy, wx, wy);
        const double tolerance = 9.0 / zoom;
        PortHit best;
        double bestD = tolerance * tolerance;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const noodles::NodeData& d = graph.nodes.at(ids[i]);
            for (bool output : {false, true}) {
                const auto& pins = output ? d.outputPins : d.inputPins;
                const auto& portOf = output ? outPortOf[i] : inPortOf[i];
                const double px = output ? d.position[0] + d.size[0] : d.position[0];
                for (const std::string& pin : pins) {
                    auto it = portOf.find(pin);
                    if (it == portOf.end())
                        continue;
                    const double py = pinCentreY(d, pin, output);
                    const double dd = (px - wx) * (px - wx) + (py - wy) * (py - wy);
                    if (dd < bestD) {
                        bestD = dd;
                        best = {static_cast<int>(i), it->second, output, px, py};
                    }
                }
            }
        }
        return best;
    }

    void projection(int w, int h, float* out) const
    {
        const double left = panX, right = panX + w / zoom;
        const double top = panY, bottom = panY + h / zoom;
        std::fill(out, out + 16, 0.0f);
        out[0] = static_cast<float>(2.0 / (right - left));
        out[5] = static_cast<float>(2.0 / (top - bottom));
        out[10] = -2.0f / 2000.0f;
        out[12] = static_cast<float>(-(right + left) / (right - left));
        out[13] = static_cast<float>(-(top + bottom) / (top - bottom));
        out[15] = 1.0f;
    }

    // A rounded rectangle around a node, drawn just behind it.
    void halo(int index, double grow, const std::array<float, 4>& colour, const float* proj)
    {
        if (index < 0 || index >= static_cast<int>(ids.size()))
            return;
        const noodles::NodeData& d = graph.nodes.at(ids[static_cast<std::size_t>(index)]);
        nodeManager.renderPortHighlight(
            static_cast<float>(d.position[0] - grow), static_cast<float>(d.position[1] - grow),
            static_cast<float>(d.size[0] + 2 * grow), static_cast<float>(d.size[1] + 2 * grow),
            static_cast<float>(noodles::RenderConfig::backgroundDepth(d.zOrder) -
                               noodles::RenderConfig::kDepthStep * 0.5),
            proj, static_cast<float>(config.nodeCornerRadius + grow), colour);
    }

    void draw(int w, int h)
    {
        const auto& bg = config.backgroundClearColor;
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(bg[0], bg[1], bg[2], bg[3]);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (graph.nodes.empty())
            return;
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        float proj[16];
        projection(w, h, proj);
        const auto z = static_cast<float>(zoom);
        const auto fw = static_cast<float>(w), fh = static_cast<float>(h);

        if (!pages.empty()) {
            std::vector<noodles::StickerRenderInput> backdrops;
            for (const Page& p : pages) {
                noodles::StickerRenderInput s;
                s.x = static_cast<float>(p.box.x0);
                s.y = static_cast<float>(p.box.y0);
                s.width = static_cast<float>(p.box.x1 - p.box.x0);
                s.height = static_cast<float>(p.box.y1 - p.box.y0);
                s.r = 52;
                s.g = 56;
                s.b = 66;
                s.a = 150;
                backdrops.push_back(s);
            }
            stickerRenderer.renderStickerData(backdrops, proj, 24.0f, {0, 0, 0, 0});
            textManager.renderText(pageLabelVerts, proj, {0.72f, 0.76f, 0.84f, 1.0f});
        }

        for (bool drawSelected : {false, true})
            linkManager.renderLinksFromGraph(graph, proj, config, z, static_cast<float>(panX),
                                             static_cast<float>(panY), fw, fh, 1.0f, drawSelected,
                                             false, kLinkColour, kLinkSelected, kLinkHovered,
                                             kLinkSelected, 0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        halo(errorNode, 14.0, kErrorHalo, proj);
        if (hovered != selected && hovered != errorNode)
            halo(hovered, 8.0, kHoverHalo, proj);

        nodeManager.renderNodesFromGraph(graph, config, font, proj,
                                         static_cast<float>(config.nodeCornerRadius),
                                         config.selectedNodeStrokeColor, contentChanged);
        nodeManager.renderPortHighlightsFromGraph(graph, config, noodles::PortHighlightState{},
                                                  proj,
                                                  static_cast<float>(config.nodePortWidth * 0.5),
                                                  contentChanged);
        textManager.renderNodeTextFull(graph.nodes, proj, z, contentChanged, config, panX, panY,
                                       static_cast<double>(w), static_cast<double>(h), true);
        contentChanged = false;
    }
};

GraphView::GraphView() : impl_(std::make_unique<Impl>()) {}
GraphView::~GraphView() = default;

bool GraphView::init(const std::filesystem::path& assetsDir)
{
    Impl& s = *impl_;
    s.ready = false;
    glewExperimental = GL_TRUE;
    const GLenum status = glewInit();
    bool glewOk = status == GLEW_OK;
#ifdef GLEW_ERROR_NO_GLX_DISPLAY
    glewOk = glewOk || status == GLEW_ERROR_NO_GLX_DISPLAY;
#endif
    if (!glewOk) {
        s.error = std::string("glewInit failed: ") +
                  reinterpret_cast<const char*>(glewGetErrorString(status));
        return false;
    }
    glGetError();  // GLEW's core-profile probe leaves a benign GL_INVALID_ENUM
    if (!glGenFramebuffers || !glGenVertexArrays) {
        s.error = "OpenGL 3.3 entry points are missing";
        return false;
    }
    if (assetsDir.empty()) {
        s.error = "noodles assets directory not configured";
        return false;
    }
    const std::string dir = assetsDir.string();
    std::error_code ec;
    if (!std::filesystem::is_directory(assetsDir / "shaders", ec) ||
        !std::filesystem::is_directory(assetsDir / "fonts", ec)) {
        s.error = "no noodles shaders/ and fonts/ under " + dir;
        return false;
    }

    // Graph rendering must not disturb what the app has bound.
    GLStateGuard guard;
    s.shaders.initialize(dir);
    for (const char* name : {"node", "link_poly", "text"}) {
        if (!s.shaders.get(name)) {
            s.error = std::string("noodles shader '") + name + "' failed to build from " + dir +
                      "/shaders";
            return false;
        }
    }
    s.font.initialize(dir + "/fonts/Poppins-Medium.png", dir + "/fonts/Poppins-Medium.json");
    if (s.font.textureId() == 0 || s.font.glyphs().empty()) {
        s.error = "no noodles font atlas under " + dir + "/fonts";
        return false;
    }
    s.nodeManager.initialize(&s.shaders);
    s.linkManager.initialize(&s.shaders);
    s.textManager.initialize(&s.shaders, &s.font);
    s.stickerRenderer.initialize(&s.shaders);
    s.nodeManager.setTransformFrame(&s.frame);
    s.textManager.setTransformFrame(&s.frame);
    s.configure();
    if (!s.createFramebuffer(16, 16))
        return false;
    s.error.clear();
    s.ready = true;
    s.build();
    return true;
}

void GraphView::setLibrary(const ModuleLibrary* library)
{
    Impl& s = *impl_;
    s.library = library;
    if (s.ready)
        s.build();
}

void GraphView::setMacro(const NetMacro* macro)
{
    Impl& s = *impl_;
    s.macro = macro;
    s.selected = s.hovered = s.errorNode = -1;
    s.leftPanning = s.middlePanning = false;
    s.dragNode = -1;
    s.dragging = false;
    s.moves.clear();
    if (s.ready)
        s.build();
    s.framePending = true;
}

void GraphView::handleInput(const GraphInput& in)
{
    Impl& s = *impl_;
    if (!s.ready)
        return;

    s.hovered = in.hovered ? s.nodeAt(in.x, in.y) : -1;

    if (in.hovered && in.wheel != 0.0f)
        s.zoomAt(in.x, in.y, in.wheel);

    // Middle drag pans; it must start over the panel but may leave it.
    if (in.middleDown && !s.prevMiddle && in.hovered)
        s.middlePanning = true;
    if (!in.middleDown)
        s.middlePanning = false;
    s.prevMiddle = in.middleDown;

    // A press on a node selects it and arms a drag. A press on empty space
    // drags the view, and clears the selection only if it is released without
    // moving (a click).
    // A press on a port starts a wire instead.
    if (in.leftClicked && in.hovered) {
        const Impl::PortHit port = s.portAt(in.x, in.y);
        if (port.node >= 0) {
            s.wire = Impl::Wire();
            s.wire.active = true;
            s.wire.node = port.node;
            s.wire.port = port.port;
            s.wire.fromOutput = port.output;
            s.wire.ax = s.wire.cx = port.x;
            s.wire.ay = s.wire.cy = port.y;
            if (!port.output && s.macro)
                for (const NetLink& l : s.macro->links)
                    if (static_cast<int>(l.toNode) == port.node && l.toInput == port.port) {
                        s.wire.wiredFrom = static_cast<int>(l.fromNode);
                        s.wire.wiredFromPort = l.fromOutput;
                    }
            return;
        }
    }
    if (s.wire.active) {
        s.screenToWorld(in.x, in.y, s.wire.cx, s.wire.cy);
        if (!in.leftDown) {
            const Impl::PortHit target = s.portAt(in.x, in.y);
            Impl::Wire w = s.wire;
            s.wire = Impl::Wire();
            if (target.node >= 0 && target.output != w.fromOutput && target.node != w.node) {
                GraphEdit e{GraphEdit::Kind::Connect};
                if (w.fromOutput) {
                    e.fromNode = w.node; e.fromPort = w.port;
                    e.toNode = target.node; e.toPort = target.port;
                } else {
                    e.fromNode = target.node; e.fromPort = target.port;
                    e.toNode = w.node; e.toPort = w.port;
                }
                s.edits.push_back(e);
            } else if (target.node < 0 && !w.fromOutput && w.wiredFrom >= 0) {
                GraphEdit e{GraphEdit::Kind::Disconnect};
                e.toNode = w.node;
                e.toPort = w.port;
                s.edits.push_back(e);
            }
        }
        return;
    }

    if (in.leftClicked) {
        const int hit = s.hovered;
        if (hit >= 0 && hit != s.selected) {
            s.selected = hit;
            s.restyle();
        }
        s.leftPanning = hit < 0;
        s.leftTravel = 0;
        s.dragNode = hit;
        s.dragging = false;
        if (hit >= 0) {
            s.screenToWorld(in.x, in.y, s.pressWorldX, s.pressWorldY);
            s.dragStart = s.graph.nodes.at(s.ids[static_cast<std::size_t>(hit)]).position;
        }
    }
    if (s.leftPanning || s.dragNode >= 0)
        s.leftTravel += std::abs(in.dragDx) + std::abs(in.dragDy);

    if (s.dragNode >= 0) {
        const auto i = static_cast<std::size_t>(s.dragNode);
        noodles::NodeData& d = s.graph.nodes.at(s.ids[i]);
        if (in.leftDown) {
            // Past a few pixels the press becomes a drag (a click only selects).
            if (!s.dragging && s.leftTravel >= 4.0f)
                s.dragging = true;
            if (s.dragging) {
                double wx, wy;
                s.screenToWorld(in.x, in.y, wx, wy);
                d.position = noodles::Vec2d(s.dragStart[0] + wx - s.pressWorldX,
                                            s.dragStart[1] + wy - s.pressWorldY);
                s.moved(i);
            }
        } else {
            if (s.dragging) {
                // Snap to whole workspace pixels, which is what gets saved.
                int fromX, fromY, toX, toY;
                s.dxFromWorld(i, s.dragStart, fromX, fromY);
                s.dxFromWorld(i, d.position, toX, toY);
                d.position = s.worldFromDx(i, toX, toY);
                s.moved(i);
                if (toX != fromX || toY != fromY)
                    s.moves.push_back({s.dragNode, fromX, fromY, toX, toY});
            }
            s.dragNode = -1;
            s.dragging = false;
        }
    }
    if (!in.leftDown && s.leftPanning) {
        s.leftPanning = false;
        if (s.leftTravel < 4.0f && s.selected >= 0) {
            s.selected = -1;
            s.restyle();
        }
    }

    if ((s.middlePanning || s.leftPanning) && (in.dragDx != 0.0f || in.dragDy != 0.0f)) {
        s.panX -= in.dragDx / s.zoom;
        s.panY -= in.dragDy / s.zoom;
    }
}

std::vector<GraphEdit> GraphView::takeEdits()
{
    std::vector<GraphEdit> out;
    out.swap(impl_->edits);
    return out;
}

bool GraphView::wirePreview(float& x1, float& y1, float& x2, float& y2) const
{
    const Impl& s = *impl_;
    if (!s.wire.active)
        return false;
    x1 = static_cast<float>((s.wire.ax - s.panX) * s.zoom);
    y1 = static_cast<float>((s.wire.ay - s.panY) * s.zoom);
    x2 = static_cast<float>((s.wire.cx - s.panX) * s.zoom);
    y2 = static_cast<float>((s.wire.cy - s.panY) * s.zoom);
    if (!s.wire.fromOutput) {   // always draw output -> input
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    return true;
}

void GraphView::panelToWorkspace(float sx, float sy, int& x, int& y) const
{
    const Impl& s = *impl_;
    double wx, wy;
    s.screenToWorld(sx, sy, wx, wy);
    // Page offsets differ per node; a new node joins the first page's frame.
    const double ox = s.offset.empty() ? 0.0 : s.offset[0][0];
    const double oy = s.offset.empty() ? 0.0 : s.offset[0][1];
    x = static_cast<int>(std::lround((wy - oy) / kScaleAcrossFlow));
    y = static_cast<int>(std::lround((wx - ox) / kScaleAlongFlow));
}

void GraphView::refresh()
{
    Impl& s = *impl_;
    if (!s.ready)
        return;
    const int count = s.macro ? static_cast<int>(s.macro->nodes.size()) : 0;
    if (s.selected >= count) s.selected = -1;
    if (s.hovered >= count) s.hovered = -1;
    if (s.errorNode >= count) s.errorNode = -1;
    s.dragNode = -1;
    s.dragging = false;
    s.wire = Impl::Wire();
    const double panX = s.panX, panY = s.panY, zoom = s.zoom;
    s.build();
    s.panX = panX;
    s.panY = panY;
    s.zoom = zoom;
}

void GraphView::select(int node)
{
    Impl& s = *impl_;
    s.selected = (node >= 0 && node < static_cast<int>(s.ids.size())) ? node : -1;
    if (s.ready)
        s.restyle();
}

std::vector<NodeMove> GraphView::takeMoves()
{
    std::vector<NodeMove> out;
    out.swap(impl_->moves);
    return out;
}

void GraphView::setNodePosition(int node, int x, int y)
{
    Impl& s = *impl_;
    if (!s.ready || node < 0 || node >= static_cast<int>(s.ids.size()))
        return;
    const auto i = static_cast<std::size_t>(node);
    s.graph.nodes.at(s.ids[i]).position = s.worldFromDx(i, x, y);
    s.moved(i);
}

void GraphView::frameAll()
{
    impl_->framePending = true;
}

unsigned int GraphView::render(int width, int height)
{
    Impl& s = *impl_;
    if (!s.ready || width <= 0 || height <= 0)
        return s.colourTex;
    width = std::min(width, 16384);
    height = std::min(height, 16384);

    GLStateGuard guard;
    if (!s.createFramebuffer(width, height)) {
        s.ready = false;
        return 0;
    }
    if (s.framePending) {
        s.doFrameAll(width, height);
        s.framePending = false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, width, height);
    s.draw(width, height);
    return s.colourTex;
}

int GraphView::selectedNode() const { return impl_->selected; }
int GraphView::hoveredNode() const { return impl_->hovered; }

void GraphView::setErrorNode(int node)
{
    Impl& s = *impl_;
    if (node < -1 || node >= static_cast<int>(s.ids.size()))
        node = -1;
    if (node == s.errorNode)
        return;
    s.errorNode = node;
    if (s.ready)
        s.restyle();
}

const std::string& GraphView::error() const { return impl_->error; }

} // namespace dxui

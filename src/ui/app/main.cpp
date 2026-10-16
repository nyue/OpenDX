// dxui2: the new OpenDX user interface (ADR 0001).
//
//   dxui2 [--dxroot DIR] [--dxexec PATH] [--data DIR] [--macros PATH]
//         [--mdf FILE]... [FILE.net]
//
// First slice: open a .net file, show its network, run it in dxexec and show
// the messages. DXROOT, DXDATA and DXMACROS from the environment are the
// defaults.

#include "ExecClient.h"
#include "GraphView.h"
#include "Camera.h"
#include "ImagePanel.h"
#include "SceneView.h"
#include "NetFile.h"
#include "NetWriter.h"
#include "ModuleDefs.h"
#include "NetEdit.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder, for the first-run layout
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef DXUI_NOODLES_ASSETS
#define DXUI_NOODLES_ASSETS ""
#endif
#ifndef DXUI_SOURCE_UI_MDF
#define DXUI_SOURCE_UI_MDF ""   // ui.mdf in the source tree, for uninstalled runs
#endif

namespace fs = std::filesystem;

namespace {

struct LogLine {
    dxui::ExecMessage::Kind kind;
    std::string text;
};

// Protan-safe message colours: errors orange, warnings yellow, info blue.
ImVec4 colourFor(dxui::ExecMessage::Kind k)
{
    switch (k) {
    case dxui::ExecMessage::Kind::Error: return {1.00f, 0.55f, 0.10f, 1};
    case dxui::ExecMessage::Kind::Warning: return {0.95f, 0.85f, 0.30f, 1};
    case dxui::ExecMessage::Kind::Info: return {0.45f, 0.70f, 1.00f, 1};
    case dxui::ExecMessage::Kind::Output: return {0.65f, 0.65f, 0.70f, 1};
    default: return {0.90f, 0.90f, 0.92f, 1};
    }
}

struct App {
    dxui::ExecConfig exec;
    fs::path netPath;
    dxui::NetFile net;
    int macroIndex = -1;
    std::string openPath;
    std::vector<LogLine> log;
    dxui::ExecClient client;
    dxui::GraphView graph;
    bool graphReady = false;
    dxui::ImagePanel images;
    bool imagesReady = false;
    bool scrollLog = false;

    // Editing. Every save writes from the file as it was loaded (its text
    // and parse) to the current model, so line numbers in the model always
    // refer to that text. Undo keeps the network before and after each edit;
    // savedAt is where the undo stack stood at the last save (-1 once that
    // state can no longer be reached).
    std::string netText;
    dxui::NetFile loaded;
    struct Edit {
        int macro;
        dxui::NetMacro before, after;
        bool structural;   // more than positions changed
    };
    std::vector<Edit> undoStack, redoStack;
    long savedAt = 0;
    std::string saveAsPath;

    // An action waiting on the unsaved-changes prompt.
    enum class Pending { None, Quit, Open };
    Pending pending = Pending::None;
    fs::path pendingOpen;
    bool askToSave = false;
    bool quitNow = false;

    bool dirty() const { return static_cast<long>(undoStack.size()) != savedAt; }

    // Module definitions: dx.mdf, ui.mdf, --mdf files, then macros from
    // DXMACROS and from the open file's own folder (as the legacy UI did).
    dxui::ModuleLibrary library;
    std::vector<fs::path> extraMdf;
    std::string moduleFilter;
    ImVec2 graphOrigin{0, 0}, graphSize{0, 0};   // the Network panel's graph, last frame

    void loadLibrary(const fs::path& netDir)
    {
        library = dxui::ModuleLibrary();
        std::vector<fs::path> mdfs = {exec.dxroot / "lib" / "dx.mdf"};
        fs::path ui = exec.dxroot / "ui" / "ui.mdf";
        std::error_code ec;
        if (!fs::exists(ui, ec) && *DXUI_SOURCE_UI_MDF)
            ui = DXUI_SOURCE_UI_MDF;
        mdfs.push_back(ui);
        mdfs.insert(mdfs.end(), extraMdf.begin(), extraMdf.end());
        for (const fs::path& f : mdfs) {
            std::string error;
            if (!library.loadMdfFile(f, &error))
                note(dxui::ExecMessage::Kind::Warning, "module definitions: " + error);
        }
        int macros = library.loadMacroPath(exec.macroPath);
        if (!netDir.empty())
            macros += library.loadMacroPath(netDir.string());
        note(dxui::ExecMessage::Kind::Info,
             std::to_string(library.size()) + " module definitions (" +
                 std::to_string(macros) + " macros)");
        if (graphReady)
            graph.setLibrary(&library);
    }

    void note(dxui::ExecMessage::Kind kind, std::string text)
    {
        log.push_back({kind, std::move(text)});
        scrollLog = true;
    }

    const dxui::NetMacro* macro() const
    {
        if (macroIndex < 0 || macroIndex >= static_cast<int>(net.macros.size()))
            return nullptr;
        return &net.macros[static_cast<std::size_t>(macroIndex)];
    }

    // Opens a file, asking first if the current one has unsaved changes.
    void open(const fs::path& path)
    {
        if (dirty()) {
            pending = Pending::Open;
            pendingOpen = path;
            askToSave = true;
            return;
        }
        doOpen(path);
    }

    void requestQuit()
    {
        if (dirty()) {
            pending = Pending::Quit;
            askToSave = true;
        } else {
            quitNow = true;
        }
    }

    void doOpen(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            note(dxui::ExecMessage::Kind::Error, "cannot read " + path.string());
            return;
        }
        std::ostringstream text;
        text << in.rdbuf();
        dxui::NetFile parsed;
        dxui::NetParseError err;
        if (!dxui::parseNet(text.str(), parsed, &err)) {
            note(dxui::ExecMessage::Kind::Error,
                 path.string() + ":" + std::to_string(err.line) + ": " + err.message);
            return;
        }
        loadLibrary(path.parent_path());
        loaded = parsed;
        valueEdits.clear();
        cameras.clear();
        gpuScenes.clear();
        viewing.clear();
        viewPending = viewToModel = false;
        programRan = false;
        net = std::move(parsed);
        netText = text.str();
        netPath = path;
        saveAsPath = path.string();
        undoStack.clear();
        redoStack.clear();
        savedAt = 0;
        images.clear();
        // main is the program; other macros are listed for browsing.
        macroIndex = static_cast<int>(net.macros.size()) - 1;
        for (std::size_t i = 0; i < net.macros.size(); ++i)
            if (net.macros[i].name == "main")
                macroIndex = static_cast<int>(i);
        if (graphReady)
            graph.setMacro(macro());
        note(dxui::ExecMessage::Kind::Info, "opened " + path.string());
    }

    void run()
    {
        if (netPath.empty())
            return;
        if (!client.running()) {
            if (exec.dxexec.empty())
                exec.dxexec = exec.dxroot / "bin_linux" / "dxexec";
            exec.workDir = netPath.parent_path();
            if (!client.start(exec)) {
                note(dxui::ExecMessage::Kind::Error, client.error());
                return;
            }
        }
        graph.setErrorNode(-1);
        if (imagesReady) {
            if (!client.runNetFileForUi(fs::absolute(netPath), images.directory()))
                note(dxui::ExecMessage::Kind::Error, client.error());
            else
                programRan = true;
        } else {
            client.runNetFile(fs::absolute(netPath));
        }
    }

    dxui::NetMacro* editable()
    {
        if (macroIndex < 0 || macroIndex >= static_cast<int>(net.macros.size()))
            return nullptr;
        return &net.macros[static_cast<std::size_t>(macroIndex)];
    }

    void record(dxui::NetMacro before, bool structural)
    {
        if (savedAt > static_cast<long>(undoStack.size()))
            savedAt = -1;   // the saved state was undone and is now gone
        undoStack.push_back({macroIndex, std::move(before), *editable(), structural});
        redoStack.clear();
    }

    // Runs one edit on the current macro: kept with undo if it succeeds, its
    // reason shown if it is refused.
    template <class Op>
    bool apply(Op op)
    {
        dxui::NetMacro* m = editable();
        if (!m)
            return false;
        dxui::NetMacro before = *m;
        std::string why;
        if (!op(*m, why)) {
            note(dxui::ExecMessage::Kind::Warning, why);
            return false;
        }
        // Keep the network savable: an edit the writer cannot write out is
        // taken back at once, with its reason.
        std::string text;
        if (!dxui::writeNet(netText, loaded, net, &library, text, &why)) {
            *m = std::move(before);
            note(dxui::ExecMessage::Kind::Warning, "not allowed: " + why);
            return false;
        }
        record(std::move(before), true);
        valueEdits.clear();
        if (graphReady)
            graph.refresh();
        return true;
    }

    // Drags the graph view finished: into the model and the undo stack.
    void takeMoves()
    {
        for (const dxui::NodeMove& mv : graph.takeMoves()) {
            dxui::NetMacro* m = editable();
            dxui::NetMacro before = *m;
            auto& n = m->nodes[static_cast<std::size_t>(mv.node)];
            n.x = static_cast<float>(mv.toX);
            n.y = static_cast<float>(mv.toY);
            record(std::move(before), false);
        }
    }

    // Wiring gestures from the graph view.
    void takeGraphEdits()
    {
        for (const dxui::GraphEdit& e : graph.takeEdits()) {
            if (e.kind == dxui::GraphEdit::Kind::Connect)
                apply([&](dxui::NetMacro& m, std::string& why) {
                    return dxui::connect(m, e.fromNode, e.fromPort, e.toNode, e.toPort, &why);
                });
            else
                apply([&](dxui::NetMacro& m, std::string& why) {
                    return dxui::disconnect(m, e.toNode, e.toPort, &why);
                });
        }
    }

    void addModule(const std::string& name, int x, int y)
    {
        const dxui::ModuleDef* def = library.find(name);
        if (!def)
            return;
        int added = -1;
        if (apply([&](dxui::NetMacro& m, std::string&) {
                added = dxui::addNode(m, *def, static_cast<float>(x), static_cast<float>(y));
                return true;
            }) && graphReady)
            graph.select(added);
    }

    void deleteSelected()
    {
        int sel = graphReady ? graph.selectedNode() : -1;
        if (sel < 0)
            return;
        apply([&](dxui::NetMacro& m, std::string& why) { return dxui::deleteNode(m, sel, &why); });
    }

    void restore(const Edit& e, bool after)
    {
        net.macros[static_cast<std::size_t>(e.macro)] = after ? e.after : e.before;
        valueEdits.clear();
        if (graphReady && e.macro == macroIndex)
            graph.refresh();
    }

    void undo()
    {
        if (undoStack.empty())
            return;
        Edit e = std::move(undoStack.back());
        undoStack.pop_back();
        restore(e, false);
        redoStack.push_back(std::move(e));
    }

    void redo()
    {
        if (redoStack.empty())
            return;
        Edit e = std::move(redoStack.back());
        redoStack.pop_back();
        restore(e, true);
        undoStack.push_back(std::move(e));
    }

    bool structuralEdits() const
    {
        for (const Edit& e : undoStack)
            if (e.structural)
                return true;
        return false;
    }

    // Writes the model into the loaded file's text and saves it to `path`.
    bool save(const fs::path& path)
    {
        if (netText.empty())
            return false;
        // Only moves: patch positions (works for any file). Otherwise the
        // structural writer, from the file as loaded to the current model.
        std::string text = netText, out, error;
        if (structuralEdits()) {
            if (!dxui::writeNet(netText, loaded, net, &library, text, &error)) {
                note(dxui::ExecMessage::Kind::Error, "not saved: " + error);
                return false;
            }
        } else {
            for (const dxui::NetMacro& m : net.macros) {
                if (!dxui::writeNodePositions(text, m, out, &error)) {
                    note(dxui::ExecMessage::Kind::Error, "not saved: " + error);
                    return false;
                }
                text = out;
            }
        }
        if (!dxui::saveTextAtomically(path, text, &error)) {
            note(dxui::ExecMessage::Kind::Error, "not saved: " + error);
            return false;
        }
        netPath = path;
        saveAsPath = path.string();
        savedAt = static_cast<long>(undoStack.size());
        note(dxui::ExecMessage::Kind::Info, "saved " + path.string());
        return true;
    }

    // The unsaved-changes prompt, then whatever was waiting on it.
    void drawSavePrompt()
    {
        if (askToSave) {
            ImGui::OpenPopup("Unsaved changes");
            askToSave = false;
        }
        if (!ImGui::BeginPopupModal("Unsaved changes", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("%s has unsaved changes.", netPath.filename().string().c_str());
        bool proceed = false;
        if (ImGui::Button("Save")) {
            proceed = save(netPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            proceed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pending = Pending::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        if (!proceed)
            return;
        if (pending == Pending::Quit) {
            quitNow = true;
        } else if (pending == Pending::Open) {
            undoStack.clear();   // discarded or saved: no longer dirty
            savedAt = 0;
            doOpen(pendingOpen);
        }
        pending = Pending::None;
    }

    std::string title() const
    {
        if (netPath.empty())
            return "OpenDX";
        return "OpenDX - " + netPath.filename().string() + (dirty() ? " *" : "");
    }

    // Exec errors name the failing node as /<macro>:<n>/<Module>:<instance>.
    int nodeInError(const std::string& text) const
    {
        const dxui::NetMacro* m = macro();
        if (!m)
            return -1;
        std::string prefix = "/" + m->name + ":";
        std::size_t at = text.find(prefix);
        if (at == std::string::npos)
            return -1;
        std::size_t slash = text.find('/', at + prefix.size());
        if (slash == std::string::npos)
            return -1;
        std::size_t colon = text.find(':', slash);
        if (colon == std::string::npos)
            return -1;
        std::string module = text.substr(slash + 1, colon - slash - 1);
        int instance = std::atoi(text.c_str() + colon + 1);
        for (std::size_t i = 0; i < m->nodes.size(); ++i)
            if (m->nodes[i].module == module && m->nodes[i].instance == instance)
                return static_cast<int>(i);
        return -1;
    }

    // Cameras the executive reported per image id (DxuiCamera), and the
    // interaction in progress: changed since last sent, when last sent.
    std::map<std::string, dxui::DxCamera> cameras;
    std::string viewing;          // image id being interacted with
    bool viewPending = false;
    double lastViewSend = 0;
    bool viewToModel = false;     // the model has not got the latest camera yet
    double lastViewMove = 0;
    // One re-render in flight at a time: sent, waiting for its image file.
    bool viewInFlight = false;
    std::filesystem::file_time_type viewStampBefore{};
    bool programRan = false;
    const bool timing = std::getenv("DXUI_TIMING") != nullptr;   // per-step latency to stderr
    int gpuDragFrames = 0;
    double gpuDragStart = 0, gpuDragLast = 0;

    // GPU views (ADR 0003): one per image id, reloaded when its scene changes.
    struct GpuScene {
        std::unique_ptr<dxui::SceneView> view;
        std::filesystem::file_time_type stamp{};
    };
    std::map<std::string, GpuScene> gpuScenes;

    void updateScenes()
    {
        for (const auto& [id, cam] : cameras) {
            (void)cam;
            auto st = images.sceneStamp(id);
            if (st == std::filesystem::file_time_type{})
                continue;
            GpuScene& g = gpuScenes[id];
            if (g.view && g.stamp == st)
                continue;
            if (!g.view)
                g.view = std::make_unique<dxui::SceneView>();
            std::string error;
            if (g.view->load(images.directory() / (id + ".scene"), &error))
                g.stamp = st;
            else if (error.find("truncated") == std::string::npos)   // half-written: retry
                note(dxui::ExecMessage::Kind::Warning, id + " scene: " + error);
        }
    }

    unsigned int gpuTexture(const std::string& id, int w, int h)
    {
        auto it = gpuScenes.find(id);
        auto cam = cameras.find(id);
        if (it == gpuScenes.end() || !it->second.view || !it->second.view->loaded() ||
            cam == cameras.end() || !cam->second.valid())
            return 0;
        int iw = 0, ih = 0;
        images.imageSize(id, iw, ih);
        return it->second.view->render(w, h, cam->second, iw, ih);
    }

    std::string gpuNote(const std::string& id)
    {
        auto it = gpuScenes.find(id);
        if (it == gpuScenes.end() || !it->second.view || !it->second.view->loaded())
            return {};
        const auto& sk = it->second.view->scene().skipped;
        static const char* names[] = {"screen objects", "volumes",
                                      "textures", "clipping", "other objects"};
        std::string out;
        for (std::size_t k = 0; k < sk.size(); ++k)
            if (sk[k])
                out += (out.empty() ? "GPU view leaves out: " : ", ") + std::to_string(sk[k]) +
                       " " + names[k];
        return out;
    }

    void takeValue(const std::string& text)
    {
        // "dxuicam:<id>:<what> <value>"
        static const std::string prefix = "dxuicam:";
        if (text.rfind(prefix, 0) != 0)
            return;
        std::size_t sp = text.find(' ');
        std::string label = text.substr(prefix.size(), sp - prefix.size());
        std::size_t colon = label.rfind(':');
        if (sp == std::string::npos || colon == std::string::npos)
            return;
        std::string id = label.substr(0, colon), what = label.substr(colon + 1);
        if (id == viewing && viewPending)
            return;   // our own newer camera is on its way
        cameras[id].set(what, text.substr(sp + 1));
    }

    // The Image node behind an image id ("Image_2" -> Image[2]).
    int imageNode(const std::string& id) const
    {
        const dxui::NetMacro* m = macro();
        static const std::string prefix = "Image_";
        if (!m || id.rfind(prefix, 0) != 0)
            return -1;
        int instance = std::atoi(id.c_str() + prefix.size());
        for (std::size_t i = 0; i < m->nodes.size(); ++i)
            if (m->nodes[i].module == "Image" && m->nodes[i].instance == instance)
                return static_cast<int>(i);
        return -1;
    }

    // Image inputs (ui.mdf): 4 useVector, 5 to, 6 from, 7 width, 10 up,
    // 11 angle, 12 perspective.
    std::vector<std::pair<int, std::string>> cameraInputs(const dxui::DxCamera& c) const
    {
        using C = dxui::DxCamera;
        return {{4, "1"},
                {5, C::format(c.to)},
                {6, C::format(c.from)},
                {10, C::format(c.up)},
                {c.perspective ? 11 : 7, C::format(c.size)},
                {12, c.perspective ? "1" : "0"}};
    }

    // Sends the camera of `id` to the executive and re-runs: only the Image's
    // part of the network recomputes (DX caches the rest).
    void sendView(const std::string& id)
    {
        int node = imageNode(id);
        const dxui::NetMacro* m = macro();
        if (node < 0 || !m || !client.running())
            return;
        const auto& n = m->nodes[static_cast<std::size_t>(node)];
        std::string var = m->name + "_Image_" + std::to_string(n.instance) + "_in_";
        std::string script;
        for (const auto& [k, value] : cameraInputs(cameras[id]))
            script += var + std::to_string(k) + " = " + value + ";\n";
        script += m->name + "();";
        viewStampBefore = images.stamp(id);
        client.send(script);
        lastViewSend = ImGui::GetTime();
        viewPending = false;
        viewInFlight = true;
    }

    void handleImageGesture(const dxui::ImageGesture& g)
    {
        // A zoom with the wheel has no release: keep it once the wheel rests.
        if (viewToModel && !g.rotate && !g.pan && ImGui::GetTime() - lastViewMove > 0.6) {
            if (viewPending)
                sendView(viewing);
            keepView(viewing);
            viewToModel = false;
        }
        if (g.id.empty() || !programRan)
            return;
        auto it = cameras.find(g.id);
        if (it == cameras.end() || !it->second.valid() || imageNode(g.id) < 0)
            return;
        dxui::DxCamera& cam = it->second;
        bool moved = false;
        if ((g.dx != 0 || g.dy != 0) && g.width > 0) {
            if (g.rotate)
                cam.orbit(g.dx, g.dy, g.width, g.height);
            else if (g.pan)
                cam.pan(g.dx, g.dy, g.width, g.height);
            moved = g.rotate || g.pan;
        }
        if (g.wheel != 0) {
            cam.zoom(g.wheel);
            moved = true;
        }
        if (moved) {
            viewing = g.id;
            viewPending = true;
            viewToModel = true;
            lastViewMove = ImGui::GetTime();
        }
        // GPU view: the camera only moves here; the executive gets it once, at
        // the end, so its own picture stays in step. Otherwise stepped: the
        // next camera goes out as soon as the previous image has arrived (one
        // render in flight); the last one when the drag ends.
        const bool gpu = images.gpuShown(g.id);
        // DXUI_TIMING: frames shown during a GPU drag and their spacing.
        if (timing && gpu && (g.rotate || g.pan)) {
            double now = ImGui::GetTime();
            if (gpuDragFrames == 0)
                gpuDragStart = now;
            ++gpuDragFrames;
            gpuDragLast = now;
        }
        if (timing && g.released && gpuDragFrames > 1) {
            std::fprintf(stderr, "dxui2: GPU drag %d frames, %.1f ms apart, no executive run\n",
                         gpuDragFrames, (gpuDragLast - gpuDragStart) * 1000.0 / (gpuDragFrames - 1));
            gpuDragFrames = 0;
        }
        if (viewPending && (gpu ? g.released : (!viewInFlight || g.released)))
            sendView(g.id);
        if (g.released && viewToModel) {
            keepView(g.id);
            viewToModel = false;
        }
    }

    // The camera into the Image node's inputs: undoable, and saved with the
    // program.
    void keepView(const std::string& id)
    {
        int node = imageNode(id);
        if (node < 0)
            return;
        auto inputs = cameraInputs(cameras[id]);
        apply([&](dxui::NetMacro& mm, std::string& why) {
            for (const auto& [k, value] : inputs)
                if (!dxui::setInputValue(mm, node, k, value, &why))
                    return false;
            return true;
        });
    }

    void pollExec()
    {
        for (auto& m : client.poll()) {
            if (m.kind == dxui::ExecMessage::Kind::Value) {
                takeValue(m.text);
                continue;
            }
            if (m.kind == dxui::ExecMessage::Kind::Error && graphReady) {
                int node = nodeInError(m.text);
                if (node >= 0)
                    graph.setErrorNode(node);
            }
            note(m.kind, std::move(m.text));
        }
    }

    void drawMenu()
    {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File")) {
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("##path", openPath.data(), openPath.capacity() + 1,
                             ImGuiInputTextFlags_CallbackResize,
                             [](ImGuiInputTextCallbackData* d) {
                                 auto* s = static_cast<std::string*>(d->UserData);
                                 if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                                     s->resize(static_cast<std::size_t>(d->BufTextLen));
                                     d->Buf = s->data();
                                 }
                                 return 0;
                             },
                             &openPath);
            ImGui::SameLine();
            if (ImGui::Button("Open") && !openPath.empty())
                open(openPath);
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, !netPath.empty()))
                save(netPath);
            ImGui::SetNextItemWidth(420);
            ImGui::InputText("##saveas", saveAsPath.data(), saveAsPath.capacity() + 1,
                             ImGuiInputTextFlags_CallbackResize,
                             [](ImGuiInputTextCallbackData* d) {
                                 auto* s = static_cast<std::string*>(d->UserData);
                                 if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                                     s->resize(static_cast<std::size_t>(d->BufTextLen));
                                     d->Buf = s->data();
                                 }
                                 return 0;
                             },
                             &saveAsPath);
            ImGui::SameLine();
            if (ImGui::Button("Save As") && !saveAsPath.empty() && !netText.empty())
                save(saveAsPath);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit"))
                requestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty()))
                undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redoStack.empty()))
                redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Node", "Delete", false,
                                graphReady && graph.selectedNode() >= 0))
                deleteSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Execute")) {
            if (ImGui::MenuItem("Execute Once", "Ctrl+E", false, !netPath.empty()))
                run();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void drawNetwork()
    {
        ImGui::Begin("Network");
        if (net.macros.size() > 1) {
            const char* current = macro() ? macro()->name.c_str() : "";
            if (ImGui::BeginCombo("Macro", current)) {
                for (std::size_t i = 0; i < net.macros.size(); ++i) {
                    bool sel = static_cast<int>(i) == macroIndex;
                    if (ImGui::Selectable(net.macros[i].name.c_str(), sel)) {
                        macroIndex = static_cast<int>(i);
                        graph.setMacro(macro());
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImVec2 size = ImGui::GetContentRegionAvail();
        int w = static_cast<int>(size.x), h = static_cast<int>(size.y);
        if (graphReady && w > 0 && h > 0) {
            ImVec2 origin = ImGui::GetCursorScreenPos();
            unsigned int tex = graph.render(w, h);
            // The texture is bottom-up (GL); flip it with the UVs.
            ImGui::Image(static_cast<ImTextureID>(tex), size, ImVec2(0, 1), ImVec2(1, 0));
            graphOrigin = origin;
            graphSize = size;
            ImGuiIO& io = ImGui::GetIO();
            // A module dragged from the Modules panel lands where it is dropped.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("DX_MODULE")) {
                    std::string name(static_cast<const char*>(pl->Data),
                                     static_cast<std::size_t>(pl->DataSize));
                    int x, y;
                    graph.panelToWorkspace(io.MousePos.x - origin.x, io.MousePos.y - origin.y, x, y);
                    addModule(name, x, y);
                }
                ImGui::EndDragDropTarget();
            }
            dxui::GraphInput in;
            in.hovered = ImGui::IsItemHovered();
            in.x = io.MousePos.x - origin.x;
            in.y = io.MousePos.y - origin.y;
            in.leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            in.middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
            in.rightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
            in.leftClicked = in.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            in.leftDoubleClicked = in.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            in.dragDx = io.MouseDelta.x;
            in.dragDy = io.MouseDelta.y;
            in.wheel = in.hovered ? io.MouseWheel : 0.0f;
            in.altDown = io.KeyAlt;
            graph.handleInput(in);
            takeMoves();
            takeGraphEdits();
            float x1, y1, x2, y2;
            if (graph.wirePreview(x1, y1, x2, y2)) {
                ImVec2 a(origin.x + x1, origin.y + y1), b(origin.x + x2, origin.y + y2);
                float bend = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
                ImGui::GetWindowDrawList()->AddBezierCubic(
                    a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b,
                    IM_COL32(115, 180, 255, 255), 2.5f);
            }
            if (in.hovered && ImGui::IsKeyPressed(ImGuiKey_F))
                graph.frameAll();
            if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput &&
                (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
                deleteSelected();
        } else if (!graphReady) {
            ImGui::TextWrapped("Graph view unavailable: %s", graph.error().c_str());
        }
        ImGui::End();
    }

    void drawMessages()
    {
        ImGui::Begin("Messages");
        if (ImGui::Button("Clear"))
            log.clear();
        ImGui::Separator();
        ImGui::BeginChild("log");
        for (const auto& l : log)
            ImGui::TextColored(colourFor(l.kind), "%s", l.text.c_str());
        if (scrollLog) {
            ImGui::SetScrollHereY(1.0f);
            scrollLog = false;
        }
        ImGui::EndChild();
        ImGui::End();
    }

    void drawDetails()
    {
        ImGui::Begin("Details");
        const dxui::NetMacro* m = macro();
        int sel = graphReady ? graph.selectedNode() : -1;
        if (m && sel >= 0 && sel < static_cast<int>(m->nodes.size())) {
            drawNodeDetails(*m, static_cast<std::size_t>(sel));
        } else if (m) {
            ImGui::Text("macro %s: %zu nodes, %zu links", m->name.c_str(),
                        m->nodes.size(), m->links.size());
            for (const auto& c : m->comments)
                ImGui::TextWrapped("%s", c.c_str());
        } else {
            ImGui::TextUnformatted("No program open.");
        }
        ImGui::End();
    }

    // The module palette: categories, a filter, and modules to add by
    // double-click (at the middle of the view) or by dragging onto the graph.
    void drawModules()
    {
        ImGui::Begin("Modules");
        if (!macro()) {
            ImGui::TextDisabled("Open a program to add modules.");
            ImGui::End();
            return;
        }
        ImGui::SetNextItemWidth(-1);
        inputText("##filter", moduleFilter);
        std::string filter = lowered(moduleFilter);
        for (const auto& [category, names] : library.byCategory()) {
            std::vector<const std::string*> shown;
            for (const std::string& n : names)
                if (filter.empty() || lowered(n).find(filter) != std::string::npos)
                    shown.push_back(&n);
            if (shown.empty())
                continue;
            ImGuiTreeNodeFlags flags = filter.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
            if (!ImGui::TreeNodeEx(category.c_str(), flags))
                continue;
            for (const std::string* n : shown) {
                ImGui::Selectable(n->c_str());
                if (ImGui::IsItemHovered()) {
                    if (const dxui::ModuleDef* d = library.find(*n); d && !d->description.empty())
                        ImGui::SetTooltip("%s", d->description.c_str());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        int x, y;
                        graph.panelToWorkspace(graphSize.x * 0.5f, graphSize.y * 0.5f, x, y);
                        addModule(*n, x, y);
                    }
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("DX_MODULE", n->data(), n->size());
                    ImGui::TextUnformatted(n->c_str());
                    ImGui::EndDragDropSource();
                }
            }
            ImGui::TreePop();
        }
        ImGui::End();
    }

    static std::string lowered(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool inputText(const char* id, std::string& s, ImGuiInputTextFlags flags = 0)
    {
        return ImGui::InputText(id, s.data(), s.capacity() + 1,
                                flags | ImGuiInputTextFlags_CallbackResize,
                                [](ImGuiInputTextCallbackData* d) {
                                    auto* str = static_cast<std::string*>(d->UserData);
                                    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                                        str->resize(static_cast<std::size_t>(d->BufTextLen));
                                        d->Buf = str->data();
                                    }
                                    return 0;
                                },
                                &s);
    }

    // Text being typed into Details' value fields, per input (node, input).
    std::map<std::pair<int, int>, std::string> valueEdits;

    // One row per input: its name, then where it is wired from, the value
    // the file sets, or the definition's default (dimmed).
    void drawNodeDetails(const dxui::NetMacro& m, std::size_t index)
    {
        const dxui::NetNode& n = m.nodes[index];
        const dxui::ModuleDef* def = library.find(n.module);
        ImGui::Text("%s[%d]", n.module.c_str(), n.instance);
        if (!n.label.empty() && n.label != n.module) {
            ImGui::SameLine();
            ImGui::TextDisabled("\"%s\"", n.label.c_str());
        }
        if (def && !def->description.empty())
            ImGui::TextWrapped("%s", def->description.c_str());
        else if (!def)
            ImGui::TextDisabled("no module definition found");
        ImGui::Separator();

        const int defIn = def ? static_cast<int>(def->inputs.size()) : 0;
        const int count = std::max(n.numInputs, defIn);
        const int node = static_cast<int>(index);

        // Expand / collapse: show every input, or hide the unwired ones.
        if (ImGui::SmallButton("Show all")) {
            apply([&](dxui::NetMacro& mm, std::string&) {
                for (int k = 1; k <= count; ++k)
                    if (!dxui::inputShown(mm, node, k, def))
                        dxui::setInputVisible(mm, node, k, true, def);
                return true;
            });
            return;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide unused")) {
            apply([&](dxui::NetMacro& mm, std::string&) {
                for (int k = 1; k <= count; ++k) {
                    bool wired = false;
                    for (const dxui::NetLink& l : mm.links)
                        wired |= l.toNode == index && l.toInput == k;
                    if (!wired && dxui::inputShown(mm, node, k, def))
                        dxui::setInputVisible(mm, node, k, false, def);
                }
                return true;
            });
            return;
        }
        if (!ImGui::BeginTable("inputs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("shown", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value");
        for (int k = 1; k <= count; ++k) {
            const dxui::ModuleParam* param = def ? def->input(k) : nullptr;
            if (param && param->visible == 2)
                continue;   // internal to the module
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(-k);
            {
                // Shown on the node; a wired input always is.
                bool wiredIn = false;
                for (const dxui::NetLink& l : m.links)
                    wiredIn |= l.toNode == index && l.toInput == k;
                bool shown = dxui::inputShown(m, node, k, def);
                ImGui::BeginDisabled(wiredIn);
                if (ImGui::Checkbox("##shown", &shown)) {
                    apply([&](dxui::NetMacro& mm, std::string& why) {
                        return dxui::setInputVisible(mm, node, k, shown, def, &why);
                    });
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    break;   // the macro changed under this loop
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(wiredIn ? "wired inputs are always shown"
                                              : "show this input on the node");
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            std::string name = param ? param->name : "input " + std::to_string(k);
            ImGui::TextUnformatted(name.c_str());
            if (param && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", param->types.c_str(), param->description.c_str());
            ImGui::TableNextColumn();

            const dxui::NetLink* wire = nullptr;
            for (const dxui::NetLink& l : m.links)
                if (l.toNode == index && l.toInput == k)
                    wire = &l;
            const dxui::NetInput* set = nullptr;
            for (const dxui::NetInput& in : n.inputs)
                if (in.index == k && in.hasValue && !in.defaulting)
                    set = &in;
            ImGui::PushID(k);
            if (wire && wire->fromNode < m.nodes.size()) {
                const dxui::NetNode& from = m.nodes[wire->fromNode];
                const dxui::ModuleDef* fromDef = library.find(from.module);
                const dxui::ModuleParam* out = fromDef ? fromDef->output(wire->fromOutput) : nullptr;
                if (ImGui::SmallButton("x")) {
                    apply([&](dxui::NetMacro& mm, std::string& why) {
                        return dxui::disconnect(mm, node, k, &why);
                    });
                    ImGui::PopID();
                    break;   // the macro changed under this loop
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("disconnect");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.45f, 0.70f, 1.0f, 1), "<- %s[%d] %s", from.module.c_str(),
                                   from.instance,
                                   out ? out->name.c_str()
                                       : ("output " + std::to_string(wire->fromOutput)).c_str());
            } else {
                // Editable: Enter sets the value (DX syntax, e.g. "name", 5,
                // [0 0 1]); an empty field goes back to the default.
                auto key = std::make_pair(static_cast<int>(index), k);
                auto it = valueEdits.find(key);
                if (it == valueEdits.end())
                    it = valueEdits.emplace(key, set ? set->value : std::string()).first;
                ImGui::SetNextItemWidth(-1);
                if (!set && it->second.empty())
                    ImGui::SetNextItemAllowOverlap();
                bool commit = inputText("##v", it->second, ImGuiInputTextFlags_EnterReturnsTrue);
                if (!set && it->second.empty() && !ImGui::IsItemActive()) {
                    ImVec2 at = ImGui::GetItemRectMin();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(at.x + 4, at.y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        param ? param->defaultValue.c_str() : "");
                }
                if (commit) {
                    std::string value = it->second;
                    apply([&](dxui::NetMacro& mm, std::string& why) {
                        return dxui::setInputValue(mm, node, k, value, &why);
                    });
                    valueEdits.clear();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void drawImage()
    {
        ImGui::Begin("Image");
        if (imagesReady) {
            images.update(viewInFlight);
            updateScenes();
            if (viewInFlight) {
                // The image arrived, or give up waiting after a second.
                double waited = ImGui::GetTime() - lastViewSend;
                if (images.stamp(viewing) != viewStampBefore || waited > 1.0) {
                    if (timing)
                        std::fprintf(stderr, "dxui2: camera to image %.0f ms%s\n", waited * 1000,
                                     waited > 1.0 ? " (gave up)" : "");
                    viewInFlight = false;
                }
            }
            handleImageGesture(images.draw());
        } else {
            ImGui::TextWrapped("No image folder; Image nodes open their own windows.");
        }
        ImGui::End();
    }
};

// First run only (no saved layout): network in the middle, details and
// image on the right, messages along the bottom.
void defaultLayout(ImGuiID dockspace)
{
    if (ImGui::DockBuilderGetNode(dockspace) &&
        ImGui::DockBuilderGetNode(dockspace)->IsSplitNode())
        return;
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID centre = dockspace;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.25f, nullptr, &centre);
    ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.16f, nullptr, &centre);
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.32f, nullptr, &centre);
    ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Network", centre);
    ImGui::DockBuilderDockWindow("Modules", left);
    ImGui::DockBuilderDockWindow("Image", rightTop);
    ImGui::DockBuilderDockWindow("Details", right);
    ImGui::DockBuilderDockWindow("Messages", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

// Layout file in the user's config folder, not the current directory.
std::string layoutFile()
{
    fs::path dir;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
        dir = x;
    else if (const char* h = std::getenv("HOME"))
        dir = fs::path(h) / ".config";
    else
        return "dxui2.ini";
    dir /= "opendx";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "dxui2.ini").string();
}

// SIGINT/SIGTERM ask the main loop to quit, so shutdown still stops dxexec
// and removes the image folder.
volatile std::sig_atomic_t quitRequested = 0;
extern "C" void onQuitSignal(int) { quitRequested = 1; }

void glfwError(int code, const char* text)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", code, text);
}

} // namespace

int main(int argc, char** argv)
{
    // On the heap so it (and its GL objects) can go before the context does.
    auto appPtr = std::make_unique<App>();
    App& app = *appPtr;
    if (const char* r = std::getenv("DXROOT"))
        app.exec.dxroot = r;
    if (const char* d = std::getenv("DXDATA"))
        app.exec.dataDir = d;
    if (const char* m = std::getenv("DXMACROS"))
        app.exec.macroPath = m;
    fs::path initial;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--mdf" && i + 1 < argc) {
            app.extraMdf.emplace_back(argv[++i]);
            continue;
        }
        if ((a == "--dxroot" || a == "--dxexec" || a == "--data" || a == "--macros") &&
            i + 1 < argc) {
            const char* v = argv[++i];
            if (a == "--dxroot") app.exec.dxroot = v;
            else if (a == "--dxexec") app.exec.dxexec = v;
            else if (a == "--macros") app.exec.macroPath = v;
            else app.exec.dataDir = v;
        } else {
            initial = a;
        }
    }

    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);
    glfwSetErrorCallback(glfwError);
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1400, 900, "OpenDX", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    static const std::string iniPath = layoutFile();
    io.IniFilename = iniPath.c_str();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // Closing the window goes through the unsaved-changes prompt.
    glfwSetWindowUserPointer(window, &app);
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        auto* a = static_cast<App*>(glfwGetWindowUserPointer(w));
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        a->requestQuit();
    });

    app.graphReady = app.graph.init(DXUI_NOODLES_ASSETS);
    app.imagesReady = app.images.init();
    app.images.gpuTexture = [&app](const std::string& id, int w, int h) {
        return app.gpuTexture(id, w, h);
    };
    app.images.gpuNote = [&app](const std::string& id) { return app.gpuNote(id); };
    app.images.movable = [&app](const std::string& id) {
        auto it = app.cameras.find(id);
        return it != app.cameras.end() && it->second.valid() && app.imageNode(id) >= 0;
    };
    if (!initial.empty())
        app.open(initial);

    std::string shownTitle;
    while (!app.quitNow && !quitRequested) {
        // Wake at least 10 times a second so exec messages show up while idle.
        // Short waits while a re-rendered image is due, so it shows at once.
        glfwWaitEventsTimeout(app.viewInFlight ? 0.002 : 0.1);
        app.pollExec();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiID dockspace = ImGui::DockSpaceOverViewport();
        defaultLayout(dockspace);
        app.drawMenu();
        // Shortcuts, unless a text field has the keyboard.
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E))
                app.run();
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && !app.netPath.empty())
                app.save(app.netPath);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
                app.undo();
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
                app.redo();
        }
        app.drawNetwork();
        app.drawDetails();
        app.drawModules();
        app.drawMessages();
        app.drawImage();
        app.drawSavePrompt();
        ImGui::Render();
        if (std::string t = app.title(); t != shownTitle) {
            glfwSetWindowTitle(window, t.c_str());
            shownTitle = t;
        }

        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    app.client.stop();
    appPtr.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

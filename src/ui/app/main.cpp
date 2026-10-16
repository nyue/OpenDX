// dxui2: the new OpenDX user interface (ADR 0001).
//
//   dxui2 [--dxroot DIR] [--dxexec PATH] [--data DIR] [--macros PATH] [FILE.net]
//
// First slice: open a .net file, show its network, run it in dxexec and show
// the messages. DXROOT, DXDATA and DXMACROS from the environment are the
// defaults.

#include "ExecClient.h"
#include "GraphView.h"
#include "ImagePanel.h"
#include "NetFile.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder, for the first-run layout
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef DXUI_NOODLES_ASSETS
#define DXUI_NOODLES_ASSETS ""
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

    void open(const fs::path& path)
    {
        dxui::NetFile parsed;
        dxui::NetParseError err;
        if (!dxui::parseNetFile(path, parsed, &err)) {
            note(dxui::ExecMessage::Kind::Error,
                 path.string() + ":" + std::to_string(err.line) + ": " + err.message);
            return;
        }
        net = std::move(parsed);
        netPath = path;
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
        } else {
            client.runNetFile(fs::absolute(netPath));
        }
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

    void pollExec()
    {
        for (auto& m : client.poll()) {
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
            if (ImGui::MenuItem("Quit"))
                glfwSetWindowShouldClose(glfwGetCurrentContext(), 1);
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
            ImGuiIO& io = ImGui::GetIO();
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
            if (in.hovered && ImGui::IsKeyPressed(ImGuiKey_F))
                graph.frameAll();
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
            const auto& n = m->nodes[static_cast<std::size_t>(sel)];
            ImGui::Text("%s[%d]", n.module.c_str(), n.instance);
            ImGui::Text("label: %s", n.label.c_str());
            ImGui::Text("inputs: %d  outputs: %d", n.numInputs, n.numOutputs);
            ImGui::Separator();
            for (const auto& in : n.inputs)
                ImGui::TextWrapped("input %d%s = %s", in.index,
                                   in.defaulting ? " (default)" : "", in.value.c_str());
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

    void drawImage()
    {
        ImGui::Begin("Image");
        if (imagesReady) {
            images.update();
            images.draw();
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
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.32f, nullptr, &centre);
    ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Network", centre);
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

    app.graphReady = app.graph.init(DXUI_NOODLES_ASSETS);
    app.imagesReady = app.images.init();
    if (!initial.empty())
        app.open(initial);

    while (!glfwWindowShouldClose(window) && !quitRequested) {
        // Wake at least 10 times a second so exec messages show up while idle.
        glfwWaitEventsTimeout(0.1);
        app.pollExec();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiID dockspace = ImGui::DockSpaceOverViewport();
        defaultLayout(dockspace);
        app.drawMenu();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E))
            app.run();
        app.drawNetwork();
        app.drawDetails();
        app.drawMessages();
        app.drawImage();
        ImGui::Render();

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

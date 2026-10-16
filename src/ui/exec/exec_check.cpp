// exec_check: run one .net file through dxexec via ExecClient, print messages.
//
//   exec_check --dxroot DIR [--dxexec PATH] [--data DIR] [--timeout SEC]
//              [--macros PATH] [--images DIR] FILE.net
//
// --macros  DXMACROS for the run (default: inherited)
//
// --images  run the file as the UI does: Image nodes write DIR/<id>.rgb
// --print-script  print what the UI would send (needs --images) and exit
//
// Exits 0 when the program ran and dxexec reported no ERROR, 1 otherwise.
// A test instrument for the UI's exec link; it opens no window.

#include "ExecClient.h"
#include "NetScript.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

const char* kindName(dxui::ExecMessage::Kind k)
{
    switch (k) {
    case dxui::ExecMessage::Kind::Info: return "info";
    case dxui::ExecMessage::Kind::Message: return "msg";
    case dxui::ExecMessage::Kind::Warning: return "warn";
    case dxui::ExecMessage::Kind::Error: return "ERROR";
    case dxui::ExecMessage::Kind::Output: return "out";
    case dxui::ExecMessage::Kind::Value: return "value";
    }
    return "?";
}

int usage()
{
    std::fprintf(stderr, "usage: exec_check --dxroot DIR [--dxexec PATH] "
                         "[--data DIR] [--timeout SEC] [--macros PATH] [--images DIR] "
                         "[--print-script] FILE.net\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    dxui::ExecConfig config;
    std::string file;
    std::filesystem::path images;
    int timeoutSec = 60;
    bool printScript = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--dxroot" || a == "--dxexec" || a == "--data" || a == "--timeout" ||
            a == "--images" || a == "--macros") {
            const char* v = next();
            if (!v)
                return usage();
            if (a == "--dxroot") config.dxroot = v;
            else if (a == "--dxexec") config.dxexec = v;
            else if (a == "--data") config.dataDir = v;
            else if (a == "--images") images = v;
            else if (a == "--macros") config.macroPath = v;
            else timeoutSec = std::atoi(v);
        } else if (a == "--print-script") {
            printScript = true;
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else {
            file = a;
        }
    }
    if (config.dxroot.empty() || file.empty())
        return usage();
    if (config.dxexec.empty())
        config.dxexec = config.dxroot / "bin_linux" / "dxexec";

    if (printScript) {
        if (images.empty())
            return usage();
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        std::fputs(dxui::netScriptForUi(text.str(), std::filesystem::absolute(images)).text.c_str(),
                   stdout);
        return 0;
    }

    dxui::ExecClient exec;
    if (!exec.start(config)) {
        std::fprintf(stderr, "start failed: %s\n", exec.error().c_str());
        return 1;
    }
    const auto path = std::filesystem::absolute(file);
    bool sent = images.empty() ? exec.runNetFile(path)
                               : exec.runNetFileForUi(path, std::filesystem::absolute(images));
    if (!sent) {
        std::fprintf(stderr, "send failed\n");
        return 1;
    }

    // Let the run begin, then poll until dxexec is idle or the deadline passes.
    bool errors = false;
    auto report = [&] {
        for (const auto& m : exec.poll()) {
            std::printf("[%s] %s\n", kindName(m.kind), m.text.c_str());
            errors |= m.kind == dxui::ExecMessage::Kind::Error;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool timedOut = true;
    while (std::chrono::steady_clock::now() < deadline) {
        report();
        if (!exec.busy()) {
            timedOut = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    report();
    exec.stop();
    report();
    if (timedOut)
        std::printf("timed out after %d s\n", timeoutSec);
    return (errors || timedOut) ? 1 : 0;
}

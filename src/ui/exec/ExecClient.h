// Starts dxexec in server mode and talks to it through DXLink.
//
// dxexec -r prints "port = N" and waits; DXLink connects to that port and
// carries scripts one way and messages back. Messages are queued and drained
// by poll(), so the caller decides when they are handled (once per UI frame).
// dxl.h stays out of this header: it defines ERROR and OK as macros.

#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DXLConnection;

namespace dxui {

struct ExecMessage {
    // Value: a DXLOutput value, `text` = "<label> <value>".
    enum class Kind { Info, Message, Warning, Error, Output, Value };
    Kind kind;
    std::string text;
};

struct ExecConfig {
    std::filesystem::path dxexec;   // the executable
    std::filesystem::path dxroot;   // DXROOT for it (fonts, lib/colors.txt, ...)
    std::filesystem::path dataDir;  // DXDATA: where Import looks; may be empty
    std::string macroPath;          // DXMACROS: where include finds macros; may be empty
    std::filesystem::path workDir;  // its current directory; empty = inherit
};

class ExecClient {
public:
    ExecClient();
    ~ExecClient();
    ExecClient(const ExecClient&) = delete;
    ExecClient& operator=(const ExecClient&) = delete;

    // Starts dxexec and connects. On failure returns false and sets error().
    bool start(const ExecConfig& config,
               std::chrono::milliseconds timeout = std::chrono::seconds(20));
    void stop();
    bool running() const { return conn_ != nullptr; }

    // Sends DX script text, e.g. `include "file.net";`.
    bool send(const std::string& script);
    // include "<path>" -- a .net file defines main() and ends by calling it.
    // Image nodes then open the executive's own windows.
    bool runNetFile(const std::filesystem::path& path);
    // Sends the file rewritten by netScriptForUi: Image nodes write their
    // pictures to imageDir/<id>.rgb instead. Returns false and sets error()
    // if the file cannot be read.
    bool runNetFileForUi(const std::filesystem::path& path,
                         const std::filesystem::path& imageDir);

    // Handles pending messages from dxexec and returns everything queued.
    std::vector<ExecMessage> poll();
    // True while dxexec reports a graph executing.
    bool busy();

    const std::string& error() const { return error_; }

private:
    void push(ExecMessage::Kind kind, std::string text);
    void drainOutput(int fd);

    DXLConnection* conn_ = nullptr;
    long pid_ = -1;
    int outFd_ = -1;
    std::thread reader_;
    std::mutex mutex_;
    std::deque<ExecMessage> queue_;
    std::string error_;

    friend struct ExecClientCallbacks;
};

} // namespace dxui

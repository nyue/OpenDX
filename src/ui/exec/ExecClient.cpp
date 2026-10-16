#include "ExecClient.h"
#include "NetScript.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <regex>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>

extern "C" {
#include <dxl.h>
// Exported by libDXL but declared only in its private dxlP.h.
DXLError DXLSendUnpacketized(DXLConnection* conn, const char* string);
}

extern char** environ;

namespace dxui {

// C callbacks from DXLink; `data` is the ExecClient.
struct ExecClientCallbacks {
    static void message(DXLConnection*, const char* text, void* data)
    {
        auto* self = static_cast<ExecClient*>(data);
        std::string s = text ? text : "";
        ExecMessage::Kind kind = ExecMessage::Kind::Message;
        if (s.find("ERROR") != std::string::npos)
            kind = ExecMessage::Kind::Error;
        else if (s.find("WARNING") != std::string::npos)
            kind = ExecMessage::Kind::Warning;
        self->push(kind, std::move(s));
    }
    static void info(DXLConnection*, const char* text, void* data)
    {
        auto* self = static_cast<ExecClient*>(data);
        self->push(ExecMessage::Kind::Info, text ? text : "");
    }
    static void error(DXLConnection*, const char* text, void* data)
    {
        // Warnings arrive on the error channel too.
        auto* self = static_cast<ExecClient*>(data);
        std::string s = text ? text : "";
        bool warning = s.find("WARNING") != std::string::npos &&
                       s.find("ERROR") == std::string::npos;
        self->push(warning ? ExecMessage::Kind::Warning : ExecMessage::Kind::Error,
                   std::move(s));
    }
};

ExecClient::ExecClient() = default;

ExecClient::~ExecClient() { stop(); }

void ExecClient::push(ExecMessage::Kind kind, std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({kind, std::move(text)});
}

// Copies dxexec's stdout into the message queue until it closes.
void ExecClient::drainOutput(int fd)
{
    std::string line;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                push(ExecMessage::Kind::Output, line);
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    if (!line.empty())
        push(ExecMessage::Kind::Output, line);
}

bool ExecClient::start(const ExecConfig& config, std::chrono::milliseconds timeout)
{
    stop();
    error_.clear();

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        error_ = std::string("pipe: ") + std::strerror(errno);
        return false;
    }

    // Child environment: ours, with DXROOT always set and DXDATA/DXMACROS
    // replaced only when the config gives them (otherwise inherited).
    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        bool replaced = !std::strncmp(*e, "DXROOT=", 7) ||
                        (!config.dataDir.empty() && !std::strncmp(*e, "DXDATA=", 7)) ||
                        (!config.macroPath.empty() && !std::strncmp(*e, "DXMACROS=", 9));
        if (!replaced)
            envStore.emplace_back(*e);
    }
    envStore.push_back("DXROOT=" + config.dxroot.string());
    if (!config.dataDir.empty())
        envStore.push_back("DXDATA=" + config.dataDir.string());
    if (!config.macroPath.empty())
        envStore.push_back("DXMACROS=" + config.macroPath);
    std::vector<char*> envp;
    for (auto& s : envStore)
        envp.push_back(s.data());
    envp.push_back(nullptr);

    std::string exe = config.dxexec.string();
    std::vector<std::string> argStore = {exe, "-r", "-B"};
    std::vector<char*> argv;
    for (auto& s : argStore)
        argv.push_back(s.data());
    argv.push_back(nullptr);

#if defined(__linux__)
    // fork + exec so the child can ask to be stopped if we die without
    // cleaning up (crash, kill -9): dxexec does not exit when the UI goes.
    // Between fork and exec only async-signal-safe calls.
    const pid_t parent = ::getpid();
    const std::string workDir = config.workDir.string();
    pid_t pid = ::fork();
    if (pid == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent)
            ::_exit(127);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0)
            ::dup2(devnull, 0);
        ::dup2(pipefd[1], 1);
        ::dup2(pipefd[1], 2);
        ::close(pipefd[0]);
        if (!workDir.empty() && ::chdir(workDir.c_str()) != 0)
            ::_exit(127);
        ::execve(exe.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    int rc = pid < 0 ? errno : 0;
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (!config.workDir.empty())
        posix_spawn_file_actions_addchdir_np(&actions, config.workDir.c_str());

    pid_t pid = -1;
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
#endif
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        error_ = "cannot start " + exe + ": " + std::strerror(rc);
        return false;
    }
    pid_ = pid;
    outFd_ = pipefd[0];

    // Read output until "port = N" or the deadline.
    const std::regex portLine(R"(port\s*=\s*(\d+))");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string pending;
    int port = -1;
    while (port < 0) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
            break;
        pollfd pfd{outFd_, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0)
            continue;
        char buf[1024];
        ssize_t n = ::read(outFd_, buf, sizeof buf);
        if (n <= 0)
            break;
        pending.append(buf, static_cast<size_t>(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            std::smatch m;
            if (std::regex_search(line, m, portLine))
                port = std::stoi(m[1]);
            else if (!line.empty())
                push(ExecMessage::Kind::Output, line);
        }
    }
    if (port < 0) {
        error_ = "dxexec did not report a port";
        stop();
        return false;
    }

    reader_ = std::thread([this, fd = outFd_] { drainOutput(fd); });

    conn_ = DXLConnectToRunningServer(port, "localhost");
    if (!conn_) {
        error_ = "cannot connect to dxexec on port " + std::to_string(port);
        stop();
        return false;
    }
    DXLSetMessageHandler(conn_, PACK_MESSAGE, nullptr,
                         &ExecClientCallbacks::message, this);
    DXLSetMessageHandler(conn_, PACK_INFO, nullptr, &ExecClientCallbacks::info, this);
    DXLSetErrorHandler(conn_, &ExecClientCallbacks::error, this);
    if (std::getenv("DXUI_TRACE_PROTOCOL"))
        DXLSetMessageDebugging(conn_, 1);   // packets to stderr
    return true;
}

void ExecClient::stop()
{
    if (conn_) {
        DXLExitDX(conn_);
        conn_ = nullptr;
    }
    if (pid_ > 0) {
        // Give it a moment to exit after DXLExitDX, then insist.
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            if (::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (i == 19) {
                ::kill(static_cast<pid_t>(pid_), SIGTERM);
                ::waitpid(static_cast<pid_t>(pid_), &status, 0);
            }
        }
        pid_ = -1;
    }
    if (reader_.joinable())
        reader_.join();
    if (outFd_ >= 0) {
        ::close(outFd_);
        outFd_ = -1;
    }
}

bool ExecClient::send(const std::string& script)
{
    if (!conn_)
        return false;
    // Plain script lines, as dxexec reads in script mode. The packetised
    // form DXLSend uses is rejected by a bare `dxexec -r` (syntax error at
    // the packet header); see docs/diary.
    return DXLSendUnpacketized(conn_, script.c_str()) == OK;
}

bool ExecClient::runNetFile(const std::filesystem::path& path)
{
    std::string p = path.string();
    std::string quoted;
    for (char c : p) {
        if (c == '"' || c == '\\')
            quoted += '\\';
        quoted += c;
    }
    return send("include \"" + quoted + "\";");
}

bool ExecClient::runNetFileForUi(const std::filesystem::path& path,
                                 const std::filesystem::path& imageDir)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "cannot read " + path.string();
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return send(netScriptForUi(text.str(), imageDir).text);
}

std::vector<ExecMessage> ExecClient::poll()
{
    if (conn_ && DXLIsMessagePending(conn_))
        DXLHandlePendingMessages(conn_);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ExecMessage> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

bool ExecClient::busy()
{
    if (!conn_)
        return false;
    int state = 0;
    if (DXLGetExecutionStatus(conn_, &state) != OK)
        return false;
    return state != 0;
}

} // namespace dxui

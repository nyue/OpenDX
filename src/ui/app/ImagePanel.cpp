#include "ImagePanel.h"

#include <GL/glew.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <csignal>
#include <cerrno>
#include <unistd.h>

namespace fs = std::filesystem;

namespace dxui {

ImagePanel::ImagePanel()
{
    // DX's 24-bit display gamma: DXGAMMA_24BIT, then DXGAMMA, else 2.0
    // (libdx/displayx.c, include/dx/arch.h).
    double gamma = 2.0;
    for (const char* name : {"DXGAMMA_24BIT", "DXGAMMA"}) {
        if (const char* s = std::getenv(name)) {
            double g = std::atof(s);
            if (g > 0) {
                gamma = g;
                break;
            }
        }
    }
    for (int i = 0; i < 256; ++i)
        gamma_[i] = static_cast<unsigned char>(
            std::lround(255.0 * std::pow(i / 255.0, 1.0 / gamma)));
}

ImagePanel::~ImagePanel()
{
    clear();
    if (!dir_.empty()) {
        std::error_code ec;
        fs::remove_all(dir_, ec);   // our own per-process folder only
    }
}

bool ImagePanel::init()
{
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec)
        return false;
    // Folders of earlier runs that were killed outright (no cleanup ran):
    // dxui2-images-<pid> whose process no longer exists.
    const std::string prefix = "dxui2-images-";
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || !entry.is_directory(ec))
            continue;
        std::string digits = name.substr(prefix.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
            continue;
        long pid = std::atol(digits.c_str());
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH)
            fs::remove_all(entry.path(), ec);
    }
    ec.clear();
    dir_ = base / (prefix + std::to_string(::getpid()));
    fs::create_directories(dir_, ec);
    return !ec;
}

void ImagePanel::clear()
{
    for (auto& [id, image] : images_)
        if (image.texture)
            glDeleteTextures(1, &image.texture);
    images_.clear();
    if (dir_.empty())
        return;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        auto ext = entry.path().extension();
        if (ext == ".rgb" || ext == ".size")
            fs::remove(entry.path(), ec);
    }
}

bool ImagePanel::load(const std::string& id, Image& image)
{
    int w = 0, h = 0, frames = 0;
    {
        std::ifstream size(dir_ / (id + ".size"));
        char x1 = 0, x2 = 0;
        if (!(size >> w >> x1 >> h >> x2 >> frames) || x1 != 'x' || x2 != 'x')
            return false;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
        return false;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
    std::ifstream data(dir_ / (id + ".rgb"), std::ios::binary);
    if (!data.read(reinterpret_cast<char*>(rgb.data()),
                   static_cast<std::streamsize>(rgb.size())))
        return false;   // still being written; try again next scan
    for (auto& v : rgb)
        v = gamma_[v];

    if (!image.texture)
        glGenTextures(1, &image.texture);
    GLint prevTex = 0, prevAlign = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glBindTexture(GL_TEXTURE_2D, image.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    image.width = w;
    image.height = h;
    return true;
}

void ImagePanel::update()
{
    if (dir_.empty())
        return;
    double now = ImGui::GetTime();
    if (lastScan_ >= 0 && now - lastScan_ < 0.25)
        return;
    lastScan_ = now;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".size")
            continue;
        std::string id = entry.path().stem().string();
        auto stamp = fs::last_write_time(entry.path(), ec);
        if (ec)
            continue;
        Image& image = images_[id];
        if (image.texture && stamp == image.stamp)
            continue;
        if (load(id, image))
            image.stamp = stamp;
    }
}

void ImagePanel::draw()
{
    if (images_.empty()) {
        ImGui::TextWrapped("Run the program (Ctrl+E) to see its images.");
        return;
    }
    if (!ImGui::BeginTabBar("images"))
        return;
    for (auto& [id, image] : images_) {
        if (!image.texture || !ImGui::BeginTabItem(id.c_str()))
            continue;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float scale = std::min(avail.x / static_cast<float>(image.width),
                               avail.y / static_cast<float>(image.height));
        if (scale > 0) {
            ImVec2 size(image.width * scale, image.height * scale);
            ImVec2 cur = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(cur.x + (avail.x - size.x) * 0.5f,
                                       cur.y + (avail.y - size.y) * 0.5f));
            // Rows arrive top first, as GL stores them: no flip.
            ImGui::Image(static_cast<ImTextureID>(image.texture), size);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s  %dx%d", id.c_str(), image.width, image.height);
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

} // namespace dxui

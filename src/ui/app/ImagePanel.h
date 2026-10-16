// The Image panel: shows the pictures dxexec's Image nodes write.
//
// Each Image node writes <dir>/<id>.rgb and <id>.size (see NetScript.h). The
// panel watches the folder, uploads changed images as textures and shows one
// tab per node, fitted to the panel. Pixels are corrected for display with
// DX's own gamma (DXGAMMA, else 2.0 on Linux), as the Display module does.
// Requires a current OpenGL context for update() and draw().

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace dxui {

class ImagePanel {
public:
    ImagePanel();
    ~ImagePanel();
    ImagePanel(const ImagePanel&) = delete;
    ImagePanel& operator=(const ImagePanel&) = delete;

    // Creates a private folder for this process; false if it cannot.
    bool init();
    const std::filesystem::path& directory() const { return dir_; }

    // Reloads images whose files changed. Cheap enough to call every frame;
    // it looks at the folder at most a few times a second.
    void update();
    // Drops the textures and the files (e.g. before running another program).
    void clear();
    // Draws inside the current ImGui window.
    void draw();

private:
    struct Image {
        int width = 0, height = 0;
        unsigned int texture = 0;
        std::filesystem::file_time_type stamp{};
    };
    bool load(const std::string& id, Image& image);

    std::filesystem::path dir_;
    std::map<std::string, Image> images_;
    unsigned char gamma_[256];
    double lastScan_ = -1.0;
};

} // namespace dxui

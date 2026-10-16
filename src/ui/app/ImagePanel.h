// The Image panel: shows the pictures dxexec's Image nodes write.
//
// Each Image node writes <dir>/<id>.rgb and <id>.size (see NetScript.h). The
// panel watches the folder, uploads changed images as textures and shows one
// tab per node, fitted to the panel. Pixels are corrected for display with
// DX's own gamma (DXGAMMA, else 2.0 on Linux), as the Display module does.
// Requires a current OpenGL context for update() and draw().

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace dxui {

// Mouse work over the shown image this frame, in pixels of the image as drawn.
struct ImageGesture {
    std::string id;            // the image under the pointer or being dragged
    float dx = 0, dy = 0;      // pointer movement while a button is held
    float wheel = 0;
    bool rotate = false;       // left button held
    bool pan = false;          // middle or right button held
    bool released = false;     // a drag ended this frame
    float width = 0, height = 0;
};

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
    // it looks at the folder at most a few times a second, or every frame
    // while `eager` (an image is expected soon).
    void update(bool eager = false);
    // Drops the textures and the files (e.g. before running another program).
    void clear();
    // Draws inside the current ImGui window; returns the mouse gesture made
    // over the visible image (left drag rotates, middle/right drag pans, the
    // wheel zooms -- the caller decides what that means).
    ImageGesture draw();
    // When the image `id` last changed on disk (zero if not seen).
    std::filesystem::file_time_type stamp(const std::string& id) const;
    // When <id>.scene last changed (zero if there is none).
    std::filesystem::file_time_type sceneStamp(const std::string& id) const;

    // The GPU view (ADR 0003): when set and it returns a texture for an image,
    // the tab shows that texture (rows bottom-up) instead of the executive's
    // picture, unless the tab's GPU box is unticked. `note` is shown under the
    // tab (e.g. what the GPU view does not draw).
    std::function<unsigned int(const std::string& id, int width, int height)> gpuTexture;
    std::function<std::string(const std::string& id)> gpuNote;
    // Whether the image's view can be moved (an Image node with a known
    // camera); the mouse hint shows only then.
    std::function<bool(const std::string& id)> movable;
    bool gpuShown(const std::string& id) const;
    // The executive's image size for `id`; false if not loaded.
    bool imageSize(const std::string& id, int& width, int& height) const;

private:
    struct Image {
        int width = 0, height = 0;
        unsigned int texture = 0;
        std::filesystem::file_time_type stamp{};
    };
    bool load(const std::string& id, Image& image);

    std::filesystem::path dir_;
    std::map<std::string, Image> images_;
    std::map<std::string, std::filesystem::file_time_type> scenes_;
    std::map<std::string, bool> gpuOff_;   // tabs switched to the executive's picture
    unsigned char gamma_[256];
    double lastScan_ = -1.0;
};

} // namespace dxui

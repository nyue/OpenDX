// Draws a scene file (ADR 0003) on the GPU into an offscreen texture, with the
// camera held by the UI: rotating and zooming need no executive run.
//
// OpenGL 3.3 core / GLSL 330 (the GLES 3.0 / WebGL2 subset). Requires a
// current context; GLEW must be initialised (GraphView::init does).

#pragma once

#include "SceneFile.h"

#include <filesystem>
#include <memory>
#include <string>

namespace dxui {

struct DxCamera;

class SceneView {
public:
    SceneView();
    ~SceneView();
    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    // Loads (or reloads) a scene file into GPU buffers.
    bool load(const std::filesystem::path& path, std::string* error = nullptr);
    bool loaded() const;
    const Scene& scene() const;

    // Renders at width x height pixels with `camera` and returns the GL
    // texture name (rows bottom-up, as GL stores them). 0 if nothing loaded.
    // imageWidth x imageHeight is the executive's image size, which screen
    // objects' pixel coordinates refer to (0: use width x height).
    unsigned int render(int width, int height, const DxCamera& camera, int imageWidth = 0,
                        int imageHeight = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dxui

// Reads the scene files the executive's DxuiScene module writes
// (include/dxuiscene.h; ADR 0003). No GL here, so it can be checked headless.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dxui {

struct SceneMesh {
    enum class Primitive { Points = 1, Lines = 2, Triangles = 3 };
    Primitive primitive = Primitive::Triangles;
    std::array<float, 16> matrix{};          // object to world, column-major
    float ambient = 1, diffuse = 0.7f, specular = 0.5f, shininess = 10;
    std::vector<float> positions;            // 3 per vertex
    std::vector<float> normals;              // 3 per vertex, or empty
    std::vector<float> colors;               // 3 per vertex, or empty
    std::vector<float> opacities;            // 1 per vertex, or empty
    std::vector<std::uint32_t> indices;      // empty: vertices in order
    std::size_t vertexCount() const { return positions.size() / 3; }
};

struct SceneLight {
    enum class Kind { Ambient = 1, Distant = 2 };
    Kind kind = Kind::Ambient;
    bool cameraRelative = false;
    std::array<float, 3> color{1, 1, 1};
    std::array<float, 3> direction{0, 0, 1};  // towards the light
};

struct Scene {
    std::vector<SceneMesh> meshes;
    std::vector<SceneLight> lights;
    std::array<std::uint32_t, 5> skipped{};   // DXS_SKIPPED_* counts
    std::array<float, 6> bounds{};            // world space, min then max
    bool hasBounds = false;

    std::size_t triangleCount() const;
    std::size_t skippedTotal() const;
};

// Returns false with *error set when the file is missing or malformed; never
// reads past the data it was given.
bool loadScene(const std::filesystem::path& path, Scene& out, std::string* error = nullptr);
bool parseScene(const std::vector<unsigned char>& bytes, Scene& out, std::string* error = nullptr);

} // namespace dxui

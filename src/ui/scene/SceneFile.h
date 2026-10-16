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

    // A screen object's mesh (captions, colour bars): positions are pixels
    // from `origin`, which DXS_SCREEN_* `screenPosition` interprets; screenZ
    // +1 in front, -1 behind, 0 in place.
    bool screen = false;
    std::uint32_t screenPosition = 0;
    int screenZ = 0;
    std::array<float, 3> origin{};
};

// A regular grid rendered as a volume (DXS_CHUNK_VOLUME): colours emitted and
// opacities absorbed, as DX holds them. The GPU view only approximates DX's
// compositing of them (docs/diary/2026-10-16.md).
struct SceneVolume {
    std::array<std::uint32_t, 3> counts{};   // grid points per axis
    bool perCell = false;                     // values per cube, else per point
    std::array<float, 16> matrix{};          // grid index to world, column-major
    float colorMultiplier = 1, opacityMultiplier = 1;
    std::vector<float> colors;               // 3 per value, last axis fastest
    std::vector<float> opacities;            // 1 per value
    std::array<std::uint32_t, 3> valueCounts() const
    {
        return perCell ? std::array<std::uint32_t, 3>{counts[0] - 1, counts[1] - 1, counts[2] - 1}
                       : counts;
    }
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
    std::vector<SceneVolume> volumes;
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

#include "SceneFile.h"

#include <dxuiscene.h>

#include <cmath>
#include <cstring>
#include <fstream>

namespace dxui {

namespace {

// Bounds-checked little-endian reader over a byte range.
class Reader {
public:
    Reader(const unsigned char* p, std::size_t n) : p_(p), n_(n) {}
    bool u32(std::uint32_t& v)
    {
        if (at_ > n_ || n_ - at_ < 4)
            return false;
        v = static_cast<std::uint32_t>(p_[at_]) | static_cast<std::uint32_t>(p_[at_ + 1]) << 8 |
            static_cast<std::uint32_t>(p_[at_ + 2]) << 16 |
            static_cast<std::uint32_t>(p_[at_ + 3]) << 24;
        at_ += 4;
        return true;
    }
    bool f32(float& v)
    {
        std::uint32_t bits;
        if (!u32(bits))
            return false;
        std::memcpy(&v, &bits, 4);
        return true;
    }
    bool floats(std::vector<float>& out, std::size_t count)
    {
        if (at_ > n_ || count > (n_ - at_) / 4)
            return false;
        out.resize(count);
        for (float& f : out)
            if (!f32(f))
                return false;
        return true;
    }
    bool bytes(char* out, std::size_t count)
    {
        if (count > left())
            return false;
        std::memcpy(out, p_ + at_, count);
        at_ += count;
        return true;
    }
    std::size_t left() const { return at_ > n_ ? 0 : n_ - at_; }
    std::size_t at() const { return at_; }
    void skip(std::size_t k) { at_ += k; }

private:
    const unsigned char* p_;
    std::size_t n_, at_ = 0;
};

bool fail(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
    return false;
}

bool readMesh(Reader& r, SceneMesh& m, std::string* error)
{
    std::uint32_t prim, vertices, indices, flags;
    if (!r.u32(prim) || !r.u32(vertices) || !r.u32(indices) || !r.u32(flags))
        return fail(error, "truncated mesh header");
    if (prim < DXS_POINTS || prim > DXS_TRIANGLES)
        return fail(error, "unknown primitive " + std::to_string(prim));
    m.primitive = static_cast<SceneMesh::Primitive>(prim);
    for (float& f : m.matrix)
        if (!r.f32(f))
            return fail(error, "truncated matrix");
    if (!r.f32(m.ambient) || !r.f32(m.diffuse) || !r.f32(m.specular) || !r.f32(m.shininess))
        return fail(error, "truncated shading values");
    const std::size_t v = vertices;
    if (!r.floats(m.positions, 3 * v))
        return fail(error, "truncated positions");
    if ((flags & DXS_HAS_NORMALS) && !r.floats(m.normals, 3 * v))
        return fail(error, "truncated normals");
    if ((flags & DXS_HAS_COLORS) && !r.floats(m.colors, 3 * v))
        return fail(error, "truncated colours");
    if ((flags & DXS_HAS_OPACITIES) && !r.floats(m.opacities, v))
        return fail(error, "truncated opacities");
    if (indices > r.left() / 4)
        return fail(error, "truncated indices");
    m.indices.resize(indices);
    for (std::uint32_t& i : m.indices) {
        if (!r.u32(i))
            return fail(error, "truncated indices");
        if (i >= vertices)
            return fail(error, "index out of range");
    }
    const std::size_t per = prim == DXS_TRIANGLES ? 3 : prim == DXS_LINES ? 2 : 1;
    const std::size_t used = indices ? indices : vertices;
    if (used % per)
        return fail(error, "element count is not a whole number of primitives");
    for (float f : m.positions)
        if (!std::isfinite(f))
            return fail(error, "non-finite position");
    return true;
}

} // namespace

std::size_t Scene::triangleCount() const
{
    std::size_t n = 0;
    for (const SceneMesh& m : meshes)
        if (m.primitive == SceneMesh::Primitive::Triangles)
            n += (m.indices.empty() ? m.vertexCount() : m.indices.size()) / 3;
    return n;
}

std::size_t Scene::skippedTotal() const
{
    std::size_t n = 0;
    for (auto s : skipped)
        n += s;
    return n;
}

bool parseScene(const std::vector<unsigned char>& bytes, Scene& out, std::string* error)
{
    out = Scene();
    Reader r(bytes.data(), bytes.size());
    char magic[8];
    if (!r.bytes(magic, 8) || std::memcmp(magic, DXS_MAGIC, 8) != 0)
        return fail(error, "not a DXSCENE1 file");
    std::uint32_t chunks;
    if (!r.u32(chunks))
        return fail(error, "truncated header");
    for (std::uint32_t c = 0; c < chunks; ++c) {
        std::uint32_t type, size;
        if (!r.u32(type) || !r.u32(size) || size > r.left() || size % 4)
            return fail(error, "bad chunk " + std::to_string(c));
        Reader chunk(bytes.data() + r.at(), size);
        r.skip(size);
        if (type == DXS_CHUNK_MESH) {
            SceneMesh m;
            if (!readMesh(chunk, m, error))
                return false;
            out.meshes.push_back(std::move(m));
        } else if (type == DXS_CHUNK_LIGHT) {
            SceneLight l;
            std::uint32_t kind, rel;
            if (!chunk.u32(kind) || !chunk.u32(rel) || !chunk.f32(l.color[0]) ||
                !chunk.f32(l.color[1]) || !chunk.f32(l.color[2]) || !chunk.f32(l.direction[0]) ||
                !chunk.f32(l.direction[1]) || !chunk.f32(l.direction[2]))
                return fail(error, "truncated light");
            if (kind != DXS_LIGHT_AMBIENT && kind != DXS_LIGHT_DISTANT)
                return fail(error, "unknown light kind " + std::to_string(kind));
            l.kind = static_cast<SceneLight::Kind>(kind);
            l.cameraRelative = rel != 0;
            out.lights.push_back(l);
        } else if (type == DXS_CHUNK_INFO) {
            for (auto& s : out.skipped)
                if (!chunk.u32(s))
                    return fail(error, "truncated info");
            for (float& b : out.bounds)
                if (!chunk.f32(b))
                    return fail(error, "truncated bounds");
            out.hasBounds = true;
        }
        // Unknown chunk types are skipped: newer writers may add some.
    }
    return true;
}

bool loadScene(const std::filesystem::path& path, Scene& out, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return fail(error, "cannot read " + path.string());
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    return parseScene(bytes, out, error);
}

} // namespace dxui

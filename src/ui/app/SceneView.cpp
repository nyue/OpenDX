#include "SceneView.h"
#include "Camera.h"

#include <GL/glew.h>
#include <dxuiscene.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dxui {

namespace {

// DX's shading model (the software renderer's, libdx): ambient plus, per
// distant light, diffuse N.L and specular (N.H)^shininess; both sides lit
// (the normal faces the viewer). Lines and points are not lit.
const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in float aOpacity;
uniform mat4 uModelView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;
out vec3 vNormal;
out vec3 vPosition;
out vec3 vColor;
out float vOpacity;
void main() {
    vec4 p = uModelView * vec4(aPosition, 1.0);
    vPosition = p.xyz;
    vNormal = uNormalMatrix * aNormal;
    vColor = aColor;
    vOpacity = aOpacity;
    gl_Position = uProjection * p;
}
)";

const char* kFragment = R"(#version 330 core
in vec3 vNormal;
in vec3 vPosition;
in vec3 vColor;
in float vOpacity;
uniform bool uLit;
uniform vec3 uAmbientLight;
uniform int uLightCount;
uniform vec3 uLightDir[8];     // view space, towards the light
uniform vec3 uLightColor[8];
uniform vec4 uShading;         // ambient, diffuse, specular, shininess
out vec4 fragColor;
void main() {
    vec3 c = vColor;
    if (uLit) {
        vec3 n = normalize(vNormal);
        vec3 toEye = normalize(-vPosition);
        if (dot(n, toEye) < 0.0) n = -n;
        vec3 sum = uShading.x * uAmbientLight * c;
        for (int i = 0; i < uLightCount; ++i) {
            vec3 l = normalize(uLightDir[i]);
            float d = max(dot(n, l), 0.0);
            vec3 h = normalize(l + toEye);
            float s = d > 0.0 ? pow(max(dot(n, h), 0.0), uShading.w) : 0.0;
            sum += uShading.y * d * c * uLightColor[i] + uShading.z * s * uLightColor[i];
        }
        c = sum;
    }
    fragColor = vec4(min(c, vec3(1.0)), vOpacity);   // linear; gamma in the last pass
}
)";

// Screen objects (captions, colour bars): pixels from an origin, as DX's
// _dxfScreen_Shade places them; not lit.
const char* kScreenVertex = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec3 aColor;
layout(location = 3) in float aOpacity;
uniform mat4 uModel;           // the screen object's own transforms, in pixels
uniform vec2 uOrigin;          // normalised device coordinates
uniform vec2 uHalfImage;       // half the executive's image size, pixels
out vec3 vColor;
out float vOpacity;
void main() {
    vec2 p = (uModel * vec4(aPosition, 1.0)).xy;
    gl_Position = vec4(uOrigin + p / uHalfImage, 0.0, 1.0);
    vColor = aColor;
    vOpacity = aOpacity;
}
)";

const char* kScreenFragment = R"(#version 330 core
in vec3 vColor;
in float vOpacity;
out vec4 fragColor;
void main() {
    fragColor = vec4(min(vColor, vec3(1.0)), vOpacity);
}
)";

// Volumes (libdx volume.c): an approximate preview. Emission c and absorption
// o are taken per unit of depth in the executive's image pixels; DX's own
// result measures differently (docs/diary/2026-10-16.md). A ray through the grid's box accumulates,
// front to back, C += T c dz, T *= 1 - o dz, stopping at opaque surfaces
// (the depth of the meshes drawn before). The box's back faces carry the
// rays, so this works from inside the volume too.
const char* kVolumeVertex = R"(#version 330 core
layout(location = 0) in vec3 aGrid;   // a corner of the unit cube
uniform mat4 uGridToWorld;
uniform mat4 uViewProj;
uniform vec3 uGridMax;        // grid points - 1, per axis
out vec3 vWorld;
void main() {
    vec4 w = uGridToWorld * vec4(aGrid * uGridMax, 1.0);
    vWorld = w.xyz;
    gl_Position = uViewProj * w;
}
)";

const char* kVolumeFragment = R"(#version 330 core
in vec3 vWorld;
uniform mat4 uWorldToGrid;
uniform mat4 uInvViewProj;
uniform vec3 uEye;            // perspective: the eye; orthographic: unused
uniform vec3 uViewDir;        // orthographic: the view direction
uniform bool uPerspective;
uniform bool uFlip;           // the grid matrix mirrors: faces swap
uniform vec3 uGridMax;        // grid points - 1, per axis (i, j, k)
uniform vec3 uTexSize;        // values per axis (i, j, k)
uniform bool uPerCell;
uniform sampler3D uValues;    // rgb colour, a opacity; texel (k, j, i)
uniform sampler2D uDepth;     // depth of the meshes already drawn
uniform vec2 uViewport;
uniform float uPixelsPerUnit, uStep, uColorMul, uOpacityMul;
out vec4 fragColor;
void main() {
    if (gl_FrontFacing != uFlip) discard;   // back faces only
    vec3 dir = uPerspective ? normalize(vWorld - uEye) : normalize(uViewDir);
    vec3 org = uPerspective ? uEye : vWorld - dir * 1.0e6;
    vec3 og = (uWorldToGrid * vec4(org, 1.0)).xyz;
    vec3 dg = (uWorldToGrid * vec4(dir, 0.0)).xyz;
    vec3 inv = 1.0 / mix(dg, vec3(1.0e-12), equal(dg, vec3(0.0)));
    vec3 ta = (vec3(0.0) - og) * inv, tb = (uGridMax - og) * inv;
    vec3 tlo = min(ta, tb), thi = max(ta, tb);
    float t0 = max(max(tlo.x, tlo.y), tlo.z);
    float t1 = min(min(thi.x, thi.y), thi.z);
    if (uPerspective) t0 = max(t0, 0.0);
    float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d < 1.0) {
        vec4 s = uInvViewProj * vec4(gl_FragCoord.xy / uViewport * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
        t1 = min(t1, dot(s.xyz / s.w - org, dir));
    }
    if (t1 <= t0) discard;
    int n = int(clamp(ceil((t1 - t0) / uStep), 1.0, 2048.0));
    float ds = (t1 - t0) / float(n);
    float dz = ds * uPixelsPerUnit;
    vec3 C = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < n; ++i) {
        vec3 g = og + dg * (t0 + (float(i) + 0.5) * ds);
        vec3 tc = uPerCell ? (floor(clamp(g, vec3(0.0), uGridMax - 1.0e-4)) + 0.5) / uTexSize
                           : (g + 0.5) / uTexSize;
        vec4 v = texture(uValues, tc.zyx);
        C += T * v.rgb * (dz * uColorMul);
        T *= max(0.0, 1.0 - v.a * dz * uOpacityMul);
        if (T < 0.002) break;
    }
    fragColor = vec4(C, T);      // blended as C + destination * T
}
)";

// Last pass: the linear picture through DX's display gamma.
const char* kGammaVertex = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kGammaFragment = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uLinear;
uniform float uInvGamma;
out vec4 fragColor;
void main() {
    vec3 c = texture(uLinear, vUv).rgb;
    fragColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(uInvGamma)), 1.0);
}
)";

using Mat4 = std::array<float, 16>;   // column-major

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    return r;
}

Mat4 lookAt(const Vec3& eye, const Vec3& at, const Vec3& upHint)
{
    Vec3 f = normalized(at - eye);
    Vec3 s = normalized(cross(f, upHint));
    Vec3 u = cross(s, f);
    Mat4 m{};
    m[0] = float(s[0]); m[4] = float(s[1]); m[8] = float(s[2]);
    m[1] = float(u[0]); m[5] = float(u[1]); m[9] = float(u[2]);
    m[2] = float(-f[0]); m[6] = float(-f[1]); m[10] = float(-f[2]);
    m[12] = float(-dot(s, eye));
    m[13] = float(-dot(u, eye));
    m[14] = float(dot(f, eye));
    m[15] = 1;
    return m;
}

// General 4x4 inverse (column-major); identity when singular.
Mat4 inverse(const Mat4& m)
{
    Mat4 inv{};
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = double(m[0]) * inv[0] + double(m[1]) * inv[4] + double(m[2]) * inv[8] + double(m[3]) * inv[12];
    if (std::fabs(det) < 1e-300)
        return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (float& v : inv)
        v = static_cast<float>(v / det);
    return inv;
}

GLuint compile(GLenum type, const char* source, std::string& log)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        log = buf;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

struct SceneView::Impl {
    Scene scene;
    bool loaded = false;

    struct GpuMesh {
        GLuint vao = 0, vbo[4] = {0, 0, 0, 0}, ebo = 0;
        GLsizei count = 0;
        GLenum mode = GL_TRIANGLES;
        bool indexed = false, lit = false, translucent = false;
        bool hasNormals = false, hasColors = false, hasOpacities = false;
        std::size_t source = 0;   // index into scene.meshes
    };
    std::vector<GpuMesh> meshes;

    struct GpuVolume {
        GLuint texture = 0;
        std::size_t source = 0;   // index into scene.volumes
    };
    std::vector<GpuVolume> volumes;

    GLuint program = 0, screenProgram = 0, volumeProgram = 0, gammaProgram = 0;
    // The picture is drawn linear (fbo: colour + depth texture), volumes read
    // a copy of the depth (depthFbo), and a last pass applies DX's display
    // gamma into out.
    GLuint fbo = 0, colour = 0, depth = 0;
    GLuint depthFbo = 0, depthCopy = 0;
    GLuint outFbo = 0, out = 0;
    GLuint cubeVao = 0, cubeVbo = 0, emptyVao = 0;
    int fbWidth = 0, fbHeight = 0;
    std::string error;

    ~Impl()
    {
        clearMeshes();
        clearVolumes();
        for (GLuint prog : {program, screenProgram, volumeProgram, gammaProgram})
            if (prog) glDeleteProgram(prog);
        for (GLuint f : {fbo, depthFbo, outFbo})
            if (f) glDeleteFramebuffers(1, &f);
        for (GLuint tex : {colour, depth, depthCopy, out})
            if (tex) glDeleteTextures(1, &tex);
        if (cubeVbo) glDeleteBuffers(1, &cubeVbo);
        for (GLuint v : {cubeVao, emptyVao})
            if (v) glDeleteVertexArrays(1, &v);
    }

    void clearVolumes()
    {
        for (GpuVolume& v : volumes)
            glDeleteTextures(1, &v.texture);
        volumes.clear();
    }

    void clearMeshes()
    {
        for (GpuMesh& g : meshes) {
            glDeleteVertexArrays(1, &g.vao);
            glDeleteBuffers(4, g.vbo);
            if (g.ebo) glDeleteBuffers(1, &g.ebo);
        }
        meshes.clear();
    }

    GLuint link(const char* vertex, const char* fragment)
    {
        GLuint vs = compile(GL_VERTEX_SHADER, vertex, error);
        GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, fragment, error) : 0;
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            return 0;
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            error = "scene shader did not link";
            glDeleteProgram(prog);
            return 0;
        }
        return prog;
    }

    bool buildProgram()
    {
        if (!program)
            program = link(kVertex, kFragment);
        if (!screenProgram)
            screenProgram = link(kScreenVertex, kScreenFragment);
        if (!volumeProgram)
            volumeProgram = link(kVolumeVertex, kVolumeFragment);
        if (!gammaProgram)
            gammaProgram = link(kGammaVertex, kGammaFragment);
        if (!cubeVao) {
            // The unit cube, outward-facing (counter-clockwise) triangles.
            static const float corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                               {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
            static const int faces[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                                             {0, 1, 5}, {0, 5, 4}, {3, 6, 2}, {3, 7, 6},
                                             {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
            std::vector<float> v;
            for (const auto& f : faces)
                for (int k : f)
                    v.insert(v.end(), corner[k], corner[k] + 3);
            GLint prevVao = 0, prevArray = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
            glGenVertexArrays(1, &cubeVao);
            glGenBuffers(1, &cubeVbo);
            glBindVertexArray(cubeVao);
            glBindBuffer(GL_ARRAY_BUFFER, cubeVbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(),
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
            glGenVertexArrays(1, &emptyVao);
            glBindVertexArray(static_cast<GLuint>(prevVao));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArray));
        }
        return program && screenProgram && volumeProgram && gammaProgram;
    }

    void uploadVolumes()
    {
        clearVolumes();
        GLint prevTex = 0, prevAlign = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_3D, &prevTex);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        for (std::size_t i = 0; i < scene.volumes.size(); ++i) {
            const SceneVolume& v = scene.volumes[i];
            const auto n = v.valueCounts();
            const std::size_t count = std::size_t(n[0]) * n[1] * n[2];
            std::vector<float> rgba(count * 4);
            for (std::size_t k = 0; k < count; ++k) {
                rgba[4 * k] = v.colors[3 * k];
                rgba[4 * k + 1] = v.colors[3 * k + 1];
                rgba[4 * k + 2] = v.colors[3 * k + 2];
                rgba[4 * k + 3] = v.opacities[k];
            }
            GpuVolume g;
            g.source = i;
            glGenTextures(1, &g.texture);
            glBindTexture(GL_TEXTURE_3D, g.texture);
            // The last axis varies fastest, so it is the texture's x.
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, GLsizei(n[2]), GLsizei(n[1]), GLsizei(n[0]), 0,
                         GL_RGBA, GL_FLOAT, rgba.data());
            const GLint filter = v.perCell ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filter);
            for (GLenum w : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
                glTexParameteri(GL_TEXTURE_3D, w, GL_CLAMP_TO_EDGE);
            volumes.push_back(g);
        }
        glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(prevTex));
        glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    }

    void upload()
    {
        clearMeshes();
        GLint prevVao = 0, prevArray = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
        for (std::size_t i = 0; i < scene.meshes.size(); ++i) {
            const SceneMesh& m = scene.meshes[i];
            const std::size_t n = m.vertexCount();
            if (n == 0)
                continue;
            GpuMesh g;
            g.source = i;
            g.mode = m.primitive == SceneMesh::Primitive::Triangles ? GL_TRIANGLES
                     : m.primitive == SceneMesh::Primitive::Lines   ? GL_LINES
                                                                    : GL_POINTS;
            g.lit = m.primitive == SceneMesh::Primitive::Triangles && !m.normals.empty();
            g.translucent = std::any_of(m.opacities.begin(), m.opacities.end(),
                                        [](float o) { return o < 0.999f; });
            glGenVertexArrays(1, &g.vao);
            glBindVertexArray(g.vao);
            glGenBuffers(4, g.vbo);
            // A missing attribute is disabled; its constant value is global
            // GL state, so render() sets it per draw.
            auto attribute = [&](int loc, const std::vector<float>& data, int size) {
                if (data.empty()) {
                    glDisableVertexAttribArray(static_cast<GLuint>(loc));
                    return;
                }
                glBindBuffer(GL_ARRAY_BUFFER, g.vbo[loc]);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                             data.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(static_cast<GLuint>(loc));
                glVertexAttribPointer(static_cast<GLuint>(loc), size, GL_FLOAT, GL_FALSE, 0, nullptr);
            };
            attribute(0, m.positions, 3);
            attribute(1, m.normals, 3);
            attribute(2, m.colors, 3);
            attribute(3, m.opacities, 1);
            g.hasNormals = !m.normals.empty();
            g.hasColors = !m.colors.empty();
            g.hasOpacities = !m.opacities.empty();
            if (!m.indices.empty()) {
                glGenBuffers(1, &g.ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(m.indices.size() * sizeof(std::uint32_t)),
                             m.indices.data(), GL_STATIC_DRAW);
                g.indexed = true;
                g.count = static_cast<GLsizei>(m.indices.size());
            } else {
                g.count = static_cast<GLsizei>(n);
            }
            meshes.push_back(g);
        }
        glBindVertexArray(static_cast<GLuint>(prevVao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArray));
    }

    static void texture2D(GLuint tex, GLint format, GLenum fmt, GLenum type, int w, int h, GLint filter)
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, fmt, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    bool framebuffer(int w, int h)
    {
        if (fbo && w == fbWidth && h == fbHeight)
            return true;
        if (!fbo) {
            glGenFramebuffers(1, &fbo);
            glGenFramebuffers(1, &depthFbo);
            glGenFramebuffers(1, &outFbo);
            glGenTextures(1, &colour);
            glGenTextures(1, &depth);
            glGenTextures(1, &depthCopy);
            glGenTextures(1, &out);
        }
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        texture2D(colour, GL_RGBA16F, GL_RGBA, GL_FLOAT, w, h, GL_NEAREST);
        texture2D(depth, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
        texture2D(depthCopy, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
        texture2D(out, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        auto complete = [&] {
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                return true;
            error = "scene framebuffer incomplete";
            return false;
        };
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        if (!complete())
            return false;
        glBindFramebuffer(GL_FRAMEBUFFER, depthFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthCopy, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (!complete())
            return false;
        glBindFramebuffer(GL_FRAMEBUFFER, outFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
        if (!complete())
            return false;
        fbWidth = w;
        fbHeight = h;
        return true;
    }
};

SceneView::SceneView() : impl_(std::make_unique<Impl>()) {}
SceneView::~SceneView() = default;

bool SceneView::load(const std::filesystem::path& path, std::string* error)
{
    Impl& s = *impl_;
    Scene scene;
    if (!loadScene(path, scene, error))
        return false;
    s.scene = std::move(scene);
    s.upload();
    s.uploadVolumes();
    s.loaded = true;
    return true;
}

bool SceneView::loaded() const { return impl_->loaded; }
const Scene& SceneView::scene() const { return impl_->scene; }

unsigned int SceneView::render(int width, int height, const DxCamera& camera, int imageWidth,
                               int imageHeight)
{
    Impl& s = *impl_;
    if (!s.loaded || width <= 0 || height <= 0 || !s.buildProgram())
        return 0;

    // What the ImGui backend relies on, restored at the end.
    GLint prevFbo = 0, prevProgram = 0, prevVao = 0, viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, viewport);
    const GLboolean depthOn = glIsEnabled(GL_DEPTH_TEST), blendOn = glIsEnabled(GL_BLEND),
                    cullOn = glIsEnabled(GL_CULL_FACE), scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLint prevActive = 0, prevTex2D[2] = {0, 0}, prevTex3D = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    for (int u = 0; u < 2; ++u) {
        glActiveTexture(GLenum(GL_TEXTURE0 + u));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D[u]);
    }
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &prevTex3D);

    if (!s.framebuffer(width, height)) {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);   // DX's default background (linear)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Camera: DX's width is the horizontal extent (orthographic); its angle
    // the full horizontal field of view (perspective).
    const Mat4 view = lookAt(camera.from, camera.to, camera.up);
    const double aspect = static_cast<double>(height) / width;
    const double dist = std::max(length(camera.from - camera.to), 1e-9);
    double radius = dist;
    if (s.scene.hasBounds) {
        const auto& b = s.scene.bounds;
        Vec3 lo{b[0], b[1], b[2]}, hi{b[3], b[4], b[5]};
        Vec3 centre = (lo + hi) * 0.5;
        radius = length(hi - lo) * 0.5 + length(centre - camera.to);
    }
    const double nearZ = std::max(dist - radius * 1.5, dist * 1e-3);
    const double farZ = dist + radius * 1.5;
    Mat4 proj{};
    if (camera.perspective) {
        const double halfW = nearZ * std::tan(camera.size * 3.14159265358979323846 / 360.0);
        const double halfH = halfW * aspect;
        proj[0] = float(nearZ / halfW);
        proj[5] = float(nearZ / halfH);
        proj[10] = float(-(farZ + nearZ) / (farZ - nearZ));
        proj[11] = -1;
        proj[14] = float(-2 * farZ * nearZ / (farZ - nearZ));
    } else {
        const double halfW = camera.size * 0.5, halfH = halfW * aspect;
        proj[0] = float(1 / halfW);
        proj[5] = float(1 / halfH);
        proj[10] = float(-2 / (farZ - nearZ));
        proj[14] = float(-(farZ + nearZ) / (farZ - nearZ));
        proj[15] = 1;
    }

    glUseProgram(s.program);
    auto loc = [&](const char* name) { return glGetUniformLocation(s.program, name); };
    glUniformMatrix4fv(loc("uProjection"), 1, GL_FALSE, proj.data());

    // Lights in view space; camera-relative ones already are.
    float ambient[3] = {0, 0, 0};
    std::vector<float> dirs, cols;
    for (const SceneLight& l : s.scene.lights) {
        if (l.kind == SceneLight::Kind::Ambient) {
            for (int k = 0; k < 3; ++k)
                ambient[k] += l.color[k];
            continue;
        }
        if (dirs.size() >= 8 * 3)
            continue;
        float d[3] = {l.direction[0], l.direction[1], l.direction[2]};
        if (!l.cameraRelative) {
            float v[3];
            for (int row = 0; row < 3; ++row)
                v[row] = view[0 * 4 + row] * d[0] + view[1 * 4 + row] * d[1] + view[2 * 4 + row] * d[2];
            std::copy(v, v + 3, d);
        }
        dirs.insert(dirs.end(), d, d + 3);
        cols.insert(cols.end(), l.color.begin(), l.color.end());
    }
    glUniform3fv(loc("uAmbientLight"), 1, ambient);
    glUniform1i(loc("uLightCount"), static_cast<GLint>(dirs.size() / 3));
    if (!dirs.empty()) {
        glUniform3fv(loc("uLightDir"), static_cast<GLsizei>(dirs.size() / 3), dirs.data());
        glUniform3fv(loc("uLightColor"), static_cast<GLsizei>(cols.size() / 3), cols.data());
    }

    const Mat4 viewProj = multiply(proj, view);
    const float imgW = static_cast<float>(imageWidth > 0 ? imageWidth : width);
    const float imgH = static_cast<float>(imageHeight > 0 ? imageHeight : height);
    // Screen objects: behind everything (z -1) before the scene, the rest after.
    auto drawScreen = [&](bool front) {
        glUseProgram(s.screenProgram);
        auto sloc = [&](const char* name) { return glGetUniformLocation(s.screenProgram, name); };
        glUniform2f(sloc("uHalfImage"), imgW * 0.5f, imgH * 0.5f);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (const Impl::GpuMesh& g : s.meshes) {
            const SceneMesh& m = s.scene.meshes[g.source];
            if (!m.screen || (m.screenZ < 0) == front)
                continue;
            // Where the origin lands, in normalised device coordinates.
            float ox = 0, oy = 0;
            if (m.screenPosition == DXS_SCREEN_VIEWPORT) {
                ox = 2 * m.origin[0] - 1;
                oy = 2 * m.origin[1] - 1;
            } else if (m.screenPosition == DXS_SCREEN_PIXEL) {
                ox = m.origin[0] / (imgW * 0.5f) - 1;
                oy = m.origin[1] / (imgH * 0.5f) - 1;
            } else if (m.screenPosition == DXS_SCREEN_WORLD) {
                float c[4];
                for (int row = 0; row < 4; ++row)
                    c[row] = viewProj[0 * 4 + row] * m.origin[0] + viewProj[1 * 4 + row] * m.origin[1] +
                             viewProj[2 * 4 + row] * m.origin[2] + viewProj[3 * 4 + row];
                if (std::fabs(c[3]) > 1e-30f) {
                    ox = c[0] / c[3];
                    oy = c[1] / c[3];
                }
            }
            glUniform2f(sloc("uOrigin"), ox, oy);
            glUniformMatrix4fv(sloc("uModel"), 1, GL_FALSE, m.matrix.data());
            glBindVertexArray(g.vao);
            if (!g.hasColors)
                glVertexAttrib3f(2, 0.7f, 0.7f, 0.7f);
            if (!g.hasOpacities)
                glVertexAttrib1f(3, 1);
            if (g.indexed)
                glDrawElements(g.mode, g.count, GL_UNSIGNED_INT, nullptr);
            else
                glDrawArrays(g.mode, 0, g.count);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUseProgram(s.program);
    };
    drawScreen(false);

    // Opaque first, then translucent blended without depth writes.
    for (bool translucentPass : {false, true}) {
        if (translucentPass) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
        }
        for (const Impl::GpuMesh& g : s.meshes) {
            if (g.translucent != translucentPass || s.scene.meshes[g.source].screen)
                continue;
            const SceneMesh& m = s.scene.meshes[g.source];
            const Mat4 mv = multiply(view, m.matrix);
            glUniformMatrix4fv(loc("uModelView"), 1, GL_FALSE, mv.data());
            // Normal matrix: inverse transpose of the model-view 3x3.
            const float a = mv[0], b = mv[4], c = mv[8], d = mv[1], e = mv[5], f = mv[9],
                        g2 = mv[2], h = mv[6], i = mv[10];
            const float det = a * (e * i - f * h) - b * (d * i - f * g2) + c * (d * h - e * g2);
            float nm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            if (std::fabs(det) > 1e-30f) {
                const float r = 1 / det;
                // Column-major inverse transpose = cofactor matrix / det.
                nm[0] = (e * i - f * h) * r; nm[3] = -(d * i - f * g2) * r; nm[6] = (d * h - e * g2) * r;
                nm[1] = -(b * i - c * h) * r; nm[4] = (a * i - c * g2) * r; nm[7] = -(a * h - b * g2) * r;
                nm[2] = (b * f - c * e) * r; nm[5] = -(a * f - c * d) * r; nm[8] = (a * e - b * d) * r;
            }
            glUniformMatrix3fv(loc("uNormalMatrix"), 1, GL_FALSE, nm);
            glUniform1i(loc("uLit"), g.lit ? 1 : 0);
            glUniform4f(loc("uShading"), m.ambient, m.diffuse, m.specular, m.shininess);
            glBindVertexArray(g.vao);
            if (!g.hasNormals)
                glVertexAttrib3f(1, 0, 0, 1);
            if (!g.hasColors)
                glVertexAttrib3f(2, 0.7f, 0.7f, 0.7f);   // DX's default grey
            if (!g.hasOpacities)
                glVertexAttrib1f(3, 1);
            if (g.indexed)
                glDrawElements(g.mode, g.count, GL_UNSIGNED_INT, nullptr);
            else
                glDrawArrays(g.mode, 0, g.count);
        }
    }

    // Volumes after the meshes, rays stopping at their depth.
    if (!s.volumes.empty()) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.depthFbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);

        glUseProgram(s.volumeProgram);
        auto vloc = [&](const char* name) { return glGetUniformLocation(s.volumeProgram, name); };
        const Mat4 invViewProj = inverse(viewProj);
        glUniformMatrix4fv(vloc("uViewProj"), 1, GL_FALSE, viewProj.data());
        glUniformMatrix4fv(vloc("uInvViewProj"), 1, GL_FALSE, invViewProj.data());
        const Vec3 viewDir = normalized(camera.to - camera.from);
        glUniform3f(vloc("uEye"), float(camera.from[0]), float(camera.from[1]), float(camera.from[2]));
        glUniform3f(vloc("uViewDir"), float(viewDir[0]), float(viewDir[1]), float(viewDir[2]));
        glUniform1i(vloc("uPerspective"), camera.perspective ? 1 : 0);
        glUniform2f(vloc("uViewport"), float(width), float(height));
        // DX's dz is depth in the executive's image pixels.
        const double visibleWidth = camera.perspective
            ? 2 * dist * std::tan(camera.size * 3.14159265358979323846 / 360.0)
            : camera.size;
        glUniform1f(vloc("uPixelsPerUnit"), float(imgW / std::max(visibleWidth, 1e-30)));
        glUniform1i(vloc("uValues"), 0);
        glUniform1i(vloc("uDepth"), 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s.depthCopy);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_SRC_ALPHA);   // C + destination * T
        glBindVertexArray(s.cubeVao);
        for (const Impl::GpuVolume& g : s.volumes) {
            const SceneVolume& v = s.scene.volumes[g.source];
            const Mat4& m = v.matrix;
            const Mat4 worldToGrid = inverse(m);
            const auto vc = v.valueCounts();
            glUniformMatrix4fv(vloc("uGridToWorld"), 1, GL_FALSE, m.data());
            glUniformMatrix4fv(vloc("uWorldToGrid"), 1, GL_FALSE, worldToGrid.data());
            glUniform3f(vloc("uGridMax"), float(v.counts[0] - 1), float(v.counts[1] - 1),
                        float(v.counts[2] - 1));
            glUniform3f(vloc("uTexSize"), float(vc[0]), float(vc[1]), float(vc[2]));
            glUniform1i(vloc("uPerCell"), v.perCell ? 1 : 0);
            glUniform1f(vloc("uColorMul"), v.colorMultiplier);
            glUniform1f(vloc("uOpacityMul"), v.opacityMultiplier);
            // Half the smallest cell edge per step; a mirroring matrix swaps faces.
            double edge = 1e30;
            for (int axis = 0; axis < 3; ++axis)
                edge = std::min(edge, length(Vec3{m[axis * 4], m[axis * 4 + 1], m[axis * 4 + 2]}));
            glUniform1f(vloc("uStep"), float(std::max(edge * 0.5, 1e-12)));
            const double det = double(m[0]) * (m[5] * m[10] - m[9] * m[6]) -
                               double(m[4]) * (m[1] * m[10] - m[9] * m[2]) +
                               double(m[8]) * (m[1] * m[6] - m[5] * m[2]);
            glUniform1i(vloc("uFlip"), det < 0 ? 1 : 0);
            glBindTexture(GL_TEXTURE_3D, g.texture);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    drawScreen(true);

    // DX's display gamma, into the texture the panel shows.
    glBindFramebuffer(GL_FRAMEBUFFER, s.outFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s.gammaProgram);
    glUniform1i(glGetUniformLocation(s.gammaProgram, "uLinear"), 0);
    static const float invGamma = static_cast<float>(1.0 / displayGamma());
    glUniform1f(glGetUniformLocation(s.gammaProgram, "uInvGamma"), invGamma);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.colour);
    glBindVertexArray(s.emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(prevTex3D));
    for (int u = 1; u >= 0; --u) {
        glActiveTexture(GLenum(GL_TEXTURE0 + u));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex2D[u]));
    }
    glActiveTexture(static_cast<GLenum>(prevActive));
    glDepthMask(depthMask);
    (depthOn ? glEnable : glDisable)(GL_DEPTH_TEST);
    (blendOn ? glEnable : glDisable)(GL_BLEND);
    (cullOn ? glEnable : glDisable)(GL_CULL_FACE);
    (scissorOn ? glEnable : glDisable)(GL_SCISSOR_TEST);
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glUseProgram(static_cast<GLuint>(prevProgram));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    return s.out;
}

} // namespace dxui

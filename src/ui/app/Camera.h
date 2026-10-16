// A DX camera as the Image node's inputs describe it, and the mouse moves on
// it: orbit about the look-to point, pan, zoom.
//
// Filled from what DxuiCamera reports (NetScript.cpp) and written back to the
// Image node's inputs: 4 useVector, 5 to, 6 from, 7 width, 10 up, 11 angle,
// 12 perspective (ui.mdf).

#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace dxui {

using Vec3 = std::array<double, 3>;

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 operator*(Vec3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(Vec3 a)
{
    double l = length(a);
    return l > 0 ? a * (1.0 / l) : a;
}
// Rotates v about the unit axis k by angle a (Rodrigues).
inline Vec3 rotated(Vec3 v, Vec3 k, double a)
{
    return v * std::cos(a) + cross(k, v) * std::sin(a) + k * (dot(k, v) * (1 - std::cos(a)));
}

struct DxCamera {
    Vec3 to{0, 0, 0}, from{0, 0, 1}, up{0, 1, 0};
    double size = 1;          // width (orthographic) or view angle in degrees
    bool perspective = false;
    // Which reported parts have arrived; usable once all have.
    int parts = 0;
    bool valid() const { return parts == 0x1f; }

    // Mouse drag of (dx, dy) pixels over an image of w x h pixels: half a turn
    // across the image.
    void orbit(double dx, double dy, double w, double h)
    {
        const double pi = 3.14159265358979323846;
        Vec3 v = from - to;
        Vec3 u = normalized(up);
        v = rotated(v, u, -dx / w * pi);                  // about the up axis
        Vec3 right = normalized(cross(v, u));
        if (length(right) > 0) {
            double a = dy / h * pi;                        // about the right axis
            v = rotated(v, right, a);
            up = normalized(rotated(u, right, a));
        }
        from = to + v;
    }

    // Moves the view so the scene follows the pointer.
    void pan(double dx, double dy, double w, double h)
    {
        Vec3 v = from - to;
        Vec3 u = normalized(up);
        Vec3 right = normalized(cross(v, u));
        double across = perspective
                            ? 2 * length(v) * std::tan(size * 3.14159265358979323846 / 360.0)
                            : size;
        Vec3 shift = right * (dx / w * across) + u * (dy / w * across);
        to = to + shift;
        from = from + shift;
        (void)h;
    }

    // Wheel steps; positive zooms in.
    void zoom(double steps)
    {
        double f = std::pow(1.15, -steps);
        if (perspective)
            from = to + (from - to) * f;
        else
            size *= f;
    }

    // Takes one reported part: "to", "from", "up", "perspective", "size".
    bool set(const std::string& what, const std::string& value)
    {
        Vec3 v;
        if (what == "to" && parseVector(value, v)) { to = v; parts |= 1; return true; }
        if (what == "from" && parseVector(value, v)) { from = v; parts |= 2; return true; }
        if (what == "up" && parseVector(value, v)) { up = v; parts |= 4; return true; }
        if (what == "perspective") { perspective = std::atoi(value.c_str()) != 0; parts |= 8; return true; }
        if (what == "size") { size = std::atof(value.c_str()); parts |= 16; return true; }
        return false;
    }

    static bool parseVector(const std::string& text, Vec3& v)
    {
        std::string s = text;
        for (char& c : s)
            if (c == '[' || c == ']' || c == ',')
                c = ' ';
        std::istringstream in(s);
        return static_cast<bool>(in >> v[0] >> v[1] >> v[2]);
    }

    static std::string format(double x)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", x);
        return buf;
    }
    static std::string format(const Vec3& v)
    {
        return "[" + format(v[0]) + " " + format(v[1]) + " " + format(v[2]) + "]";
    }
};

} // namespace dxui

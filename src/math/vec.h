#pragma once

#include <cmath>

namespace opennr {

// Right-handed, +z-up coordinate system per the Papyrus telemetry SDK
// (papytelemapp.h, lines 412-413).  All world-space code in this project
// must use these conventions.
//
// Car-local axes:
//   +x = toward nose
//   +y = toward left side
//   +z = up
//   +yaw rotates +x into +y
//   +pitch rotates +z into +x
//   +roll  rotates +y into +z
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float xx, float yy, float zz) : x(xx), y(yy), z(zz) {}

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
};

constexpr float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

// Build the car -> world rotation matrix from (yaw, pitch, roll) in radians.
// Apply yaw first, then pitch, then roll - this is the order specified by
// the Papyrus telemetry SDK (papytelemapp.h, lines 432-450).
struct Mat3 {
    float m[3][3]{};
};

inline Mat3 car_to_world_from_ypr(float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll),  sr = std::sin(roll);

    Mat3 r;
    r.m[0][0] = cp * cy;
    r.m[0][1] = cy * sp * sr - cr * sy;
    r.m[0][2] = cr * cy * sp + sr * sy;
    r.m[1][0] = cp * sy;
    r.m[1][1] = cr * cy + sp * sr * sy;
    r.m[1][2] = cr * sp * sy - cy * sr;
    r.m[2][0] = -sp;
    r.m[2][1] = cp * sr;
    r.m[2][2] = cp * cr;
    return r;
}

}  // namespace opennr

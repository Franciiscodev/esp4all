#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "UE4Structures.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * 3D Math utilities for camera projection (Z-Up UE4 & Y-Up non-UE4 engines),
 * World-to-Screen (ViewMatrix 4x4 ColumnMajor/RowMajor + Trig), and Bone FTransform math.
 */
namespace UE4Math {

    constexpr float DEG2RAD = static_cast<float>(M_PI) / 180.0f;
    constexpr float W2S_MATRIX_EPS = 0.001f;
    constexpr float W2S_TRIG_NEAR  = 1.0f;

    struct ScreenBox {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float centerX = 0.0f;
        float depth = 0.0f;
        bool valid = false;
    };

    struct Matrix4x4 {
        float m[4][4];

        Matrix4x4() {
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    m[i][j] = (i == j) ? 1.0f : 0.0f;
                }
            }
        }

        std::array<float, 16> ToArray() const {
            std::array<float, 16> result{};
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    result[i * 4 + j] = m[i][j];
                }
            }
            return result;
        }

        static Matrix4x4 FromArray(const std::array<float, 16>& arr) {
            Matrix4x4 result;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    result.m[i][j] = arr[i * 4 + j];
                }
            }
            return result;
        }

        Matrix4x4 operator*(const Matrix4x4& other) const {
            Matrix4x4 result;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        sum += m[i][k] * other.m[k][j];
                    }
                    result.m[i][j] = sum;
                }
            }
            return result;
        }
    };

    inline float Dot(const Vec3& a, const Vec3& b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    /**
     * Convert UE4 FTransform (Rotation, Translation, Scale3D) to 4x4 row-vector matrix.
     */
    inline Matrix4x4 TransformToMatrixWithScale(const FTransform& t) noexcept {
        Matrix4x4 m;
        m.m[3][0] = t.Translation.x;
        m.m[3][1] = t.Translation.y;
        m.m[3][2] = t.Translation.z;
        m.m[3][3] = 1.0f;

        const float x2 = t.Rotation.x + t.Rotation.x;
        const float y2 = t.Rotation.y + t.Rotation.y;
        const float z2 = t.Rotation.z + t.Rotation.z;

        const float xx2 = t.Rotation.x * x2;
        const float yy2 = t.Rotation.y * y2;
        const float zz2 = t.Rotation.z * z2;

        m.m[0][0] = (1.0f - (yy2 + zz2)) * t.Scale3D.x;
        m.m[1][1] = (1.0f - (xx2 + zz2)) * t.Scale3D.y;
        m.m[2][2] = (1.0f - (xx2 + yy2)) * t.Scale3D.z;

        const float yz2 = t.Rotation.y * z2;
        const float wx2 = t.Rotation.w * x2;
        m.m[2][1] = (yz2 - wx2) * t.Scale3D.z;
        m.m[1][2] = (yz2 + wx2) * t.Scale3D.y;

        const float xy2 = t.Rotation.x * y2;
        const float wz2 = t.Rotation.w * z2;
        m.m[1][0] = (xy2 - wz2) * t.Scale3D.y;
        m.m[0][1] = (xy2 + wz2) * t.Scale3D.x;

        const float xz2 = t.Rotation.x * z2;
        const float wy2 = t.Rotation.w * y2;
        m.m[2][0] = (xz2 + wy2) * t.Scale3D.z;
        m.m[0][2] = (xz2 - wy2) * t.Scale3D.x;

        m.m[0][3] = 0.0f;
        m.m[1][3] = 0.0f;
        m.m[2][3] = 0.0f;
        return m;
    }

    /**
     * Transform a local bone FTransform into world-space 3D coordinates using
     * a precomputed ComponentToWorld matrix.
     */
    inline Vec3 BoneTransformToWorld(const FTransform& boneLocal, const Matrix4x4& compToWorldMat) noexcept {
        const Matrix4x4 boneMat = TransformToMatrixWithScale(boneLocal);
        const Matrix4x4 worldMat = boneMat * compToWorldMat;
        return Vec3(worldMat.m[3][0], worldMat.m[3][1], worldMat.m[3][2]);
    }

    /**
     * Orthonormal camera axes in UE4 Left-Handed Z-Up (+X Forward, +Y Right, +Z Up).
     */
    inline void GetUE4CameraAxes(
        const Vec3& rotationDeg,
        Vec3& outForward,
        Vec3& outRight,
        Vec3& outUp) noexcept
    {
        const float pitch = rotationDeg.x * DEG2RAD;
        const float yaw   = rotationDeg.y * DEG2RAD;
        const float roll  = rotationDeg.z * DEG2RAD;

        const float cp = std::cos(pitch);
        const float sp = std::sin(pitch);
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float cr = std::cos(roll);
        const float sr = std::sin(roll);

        outForward = Vec3(cp * cy, cp * sy, sp);
        outRight   = Vec3(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
        outUp      = Vec3(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);
    }

    /**
     * Orthonormal camera axes in Left-Handed Y-Up (+Z Forward, +X Right, +Y Up).
     * Rotation layout: x = Pitch (+up), y = Yaw (+right), z = Roll.
     */
    inline void GetYUpCameraAxes(
        const Vec3& rotationDeg,
        Vec3& outForward,
        Vec3& outRight,
        Vec3& outUp) noexcept
    {
        const float pitch = rotationDeg.x * DEG2RAD;
        const float yaw   = rotationDeg.y * DEG2RAD;
        const float roll  = rotationDeg.z * DEG2RAD;

        const float cp = std::cos(pitch);
        const float sp = std::sin(pitch);
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float cr = std::cos(roll);
        const float sr = std::sin(roll);

        outForward = Vec3(cp * sy, sp, cp * cy);
        outRight   = Vec3(cr * cy + sr * sp * sy, -sr * cp, -cr * sy + sr * sp * cy);
        outUp      = Vec3(sr * cy - cr * sp * sy, cr * cp, -sr * sy - cr * sp * cy);
    }

    /**
     * Direct 4x4 View-Projection Matrix World-to-Screen with explicit Z-clipping (w >= 0.001f).
     * Supports both ColumnMajor (UE4/PUBG/Farlight/Messiah) and RowMajor (PUBG New State/work12).
     */
    inline bool WorldToScreenMatrix(
        const Vec3& worldPos,
        const std::array<float, 16>& matrix,
        int screenWidth,
        int screenHeight,
        Vec3& outScreen,
        MatrixLayout layout = MatrixLayout::ColumnMajor) noexcept
    {
        if (screenWidth <= 0 || screenHeight <= 0) {
            return false;
        }

        float clipX = 0.0f;
        float clipY = 0.0f;
        float clipW = 0.0f;

        if (layout == MatrixLayout::ColumnMajor) {
            clipX = worldPos.x * matrix[0] + worldPos.y * matrix[4] + worldPos.z * matrix[8]  + matrix[12];
            clipY = worldPos.x * matrix[1] + worldPos.y * matrix[5] + worldPos.z * matrix[9]  + matrix[13];
            clipW = worldPos.x * matrix[3] + worldPos.y * matrix[7] + worldPos.z * matrix[11] + matrix[15];
        } else {
            clipX = matrix[0]  * worldPos.x + matrix[1]  * worldPos.y + matrix[2]  * worldPos.z + matrix[3];
            clipY = matrix[4]  * worldPos.x + matrix[5]  * worldPos.y + matrix[6]  * worldPos.z + matrix[7];
            clipW = matrix[12] * worldPos.x + matrix[13] * worldPos.y + matrix[14] * worldPos.z + matrix[15];
        }

        if (!std::isfinite(clipW) || clipW < W2S_MATRIX_EPS) {
            return false;
        }

        const float invW  = 1.0f / clipW;
        const float halfW = static_cast<float>(screenWidth) * 0.5f;
        const float halfH = static_cast<float>(screenHeight) * 0.5f;

        outScreen.x = halfW + (clipX * invW) * halfW;
        outScreen.y = halfH - (clipY * invW) * halfH;
        outScreen.z = clipW;

        return std::isfinite(outScreen.x) && std::isfinite(outScreen.y);
    }

    /**
     * Direct Trigonometric World-to-Screen supporting both UE4 Z-Up (default) and Y-Up engines.
     */
    inline bool WorldToScreenTrig(
        const Vec3& worldPos,
        const Vec3& camPos,
        const Vec3& camRotDeg,
        float horizontalFovDeg,
        int screenWidth,
        int screenHeight,
        Vec3& outScreen,
        UpAxis upAxis = UpAxis::Z) noexcept
    {
        if (screenWidth <= 0 || screenHeight <= 0) {
            return false;
        }
        if (!std::isfinite(horizontalFovDeg) || horizontalFovDeg <= 0.1f || horizontalFovDeg >= 179.9f) {
            return false;
        }

        Vec3 vForward, vRight, vUp;
        if (upAxis == UpAxis::Y) {
            GetYUpCameraAxes(camRotDeg, vForward, vRight, vUp);
        } else {
            GetUE4CameraAxes(camRotDeg, vForward, vRight, vUp);
        }

        const Vec3 delta = worldPos - camPos;
        const float localX = Dot(delta, vRight);
        const float localY = Dot(delta, vUp);
        const float localZ = Dot(delta, vForward);

        if (!std::isfinite(localZ) || localZ < W2S_TRIG_NEAR) {
            return false;
        }

        const float halfW = static_cast<float>(screenWidth) * 0.5f;
        const float halfH = static_cast<float>(screenHeight) * 0.5f;
        const float tanHalfFov = std::tan(horizontalFovDeg * 0.5f * DEG2RAD);
        if (tanHalfFov <= 1e-6f) {
            return false;
        }

        const float fovFactor = halfW / tanHalfFov;
        outScreen.x = halfW + (localX * fovFactor) / localZ;
        outScreen.y = halfH - (localY * fovFactor) / localZ;
        outScreen.z = localZ;

        return std::isfinite(outScreen.x) && std::isfinite(outScreen.y);
    }

    inline bool WorldToScreenTrig(
        const Vec3& worldPos,
        const FMinimalViewInfo& camera,
        int screenWidth,
        int screenHeight,
        Vec3& outScreen,
        UpAxis upAxis = UpAxis::Z) noexcept
    {
        return WorldToScreenTrig(
            worldPos, camera.Location, camera.Rotation, camera.FOV,
            screenWidth, screenHeight, outScreen, upAxis);
    }

    /**
     * Build a View-Projection matrix (ColumnMajor or RowMajor, Z-Up or Y-Up)
     * mathematically identical to WorldToScreenTrig().
     */
    inline std::array<float, 16> BuildViewProjectionMatrix(
        const FMinimalViewInfo& camera,
        int screenWidth,
        int screenHeight,
        MatrixLayout layout = MatrixLayout::ColumnMajor,
        UpAxis upAxis = UpAxis::Z) noexcept
    {
        std::array<float, 16> col{};
        if (screenWidth <= 0 || screenHeight <= 0 ||
            camera.FOV <= 0.1f || camera.FOV >= 179.9f) {
            return col;
        }

        Vec3 vForward, vRight, vUp;
        if (upAxis == UpAxis::Y) {
            GetYUpCameraAxes(camera.Rotation, vForward, vRight, vUp);
        } else {
            GetUE4CameraAxes(camera.Rotation, vForward, vRight, vUp);
        }

        const float tanHalfFov = std::tan(camera.FOV * 0.5f * DEG2RAD);
        if (tanHalfFov <= 1e-6f) {
            return col;
        }

        const float aspect = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
        const float scaleX = 1.0f / tanHalfFov;
        const float scaleY = aspect / tanHalfFov;

        col[0]  = vRight.x * scaleX;
        col[4]  = vRight.y * scaleX;
        col[8]  = vRight.z * scaleX;
        col[12] = -Dot(camera.Location, vRight) * scaleX;

        col[1]  = vUp.x * scaleY;
        col[5]  = vUp.y * scaleY;
        col[9]  = vUp.z * scaleY;
        col[13] = -Dot(camera.Location, vUp) * scaleY;

        col[2]  = vForward.x;
        col[6]  = vForward.y;
        col[10] = vForward.z;
        col[14] = -Dot(camera.Location, vForward);

        col[3]  = vForward.x;
        col[7]  = vForward.y;
        col[11] = vForward.z;
        col[15] = -Dot(camera.Location, vForward);

        if (layout == MatrixLayout::RowMajor) {
            std::array<float, 16> row{};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    row[r * 4 + c] = col[c * 4 + r];
                }
            }
            return row;
        }
        return col;
    }

    inline std::array<float, 16> BuildUE4ViewProjectionMatrix(
        const FMinimalViewInfo& camera,
        int screenWidth,
        int screenHeight) noexcept
    {
        return BuildViewProjectionMatrix(
            camera, screenWidth, screenHeight, MatrixLayout::ColumnMajor, UpAxis::Z);
    }

    inline bool WorldToScreen(
        const Vec3& worldPos,
        const std::array<float, 16>& viewProjMatrix,
        int screenWidth,
        int screenHeight,
        Vec3& outScreen) noexcept
    {
        return WorldToScreenMatrix(
            worldPos, viewProjMatrix, screenWidth, screenHeight,
            outScreen, MatrixLayout::ColumnMajor);
    }

    inline Vec3 OffsetUp(const Vec3& pos, float delta, UpAxis upAxis) noexcept {
        if (upAxis == UpAxis::Y) {
            return Vec3(pos.x, pos.y + delta, pos.z);
        }
        return Vec3(pos.x, pos.y, pos.z + delta);
    }

    inline float VerticalDelta(const Vec3& a, const Vec3& b, UpAxis upAxis) noexcept {
        return (upAxis == UpAxis::Y) ? std::fabs(a.y - b.y) : std::fabs(a.z - b.z);
    }

    inline bool BuildScreenBoxFromPoints(
        const Vec3& baseScreen,
        const Vec3& headScreen,
        ScreenBox& outBox) noexcept
    {
        const float boxHeight = baseScreen.y - headScreen.y;
        if (!std::isfinite(boxHeight) || boxHeight <= 1.0f) {
            outBox = ScreenBox{};
            return false;
        }

        const float boxWidth = boxHeight * 0.5f;
        outBox.centerX = baseScreen.x;
        outBox.width   = boxWidth;
        outBox.height  = boxHeight;
        outBox.left    = baseScreen.x - boxWidth * 0.5f;
        outBox.right   = baseScreen.x + boxWidth * 0.5f;
        outBox.top     = headScreen.y;
        outBox.bottom  = baseScreen.y;
        outBox.depth   = baseScreen.z;
        outBox.valid   = true;
        return true;
    }

    inline bool ComputeScreenBoxMatrix(
        const Vec3& worldPos,
        float actorHeight,
        const std::array<float, 16>& matrix,
        int screenWidth,
        int screenHeight,
        ScreenBox& outBox,
        MatrixLayout layout = MatrixLayout::ColumnMajor,
        UpAxis upAxis = UpAxis::Z,
        float baseOffset = 0.0f) noexcept
    {
        Vec3 baseScreen, headScreen;
        const Vec3 footPos = OffsetUp(worldPos, baseOffset, upAxis);
        const Vec3 headPos = OffsetUp(worldPos, baseOffset + actorHeight, upAxis);

        if (!WorldToScreenMatrix(footPos, matrix, screenWidth, screenHeight, baseScreen, layout)) {
            outBox = ScreenBox{};
            return false;
        }
        if (!WorldToScreenMatrix(headPos, matrix, screenWidth, screenHeight, headScreen, layout)) {
            outBox = ScreenBox{};
            return false;
        }

        return BuildScreenBoxFromPoints(baseScreen, headScreen, outBox);
    }

    inline bool ComputeScreenBoxTrig(
        const Vec3& worldPos,
        float actorHeight,
        const FMinimalViewInfo& camera,
        int screenWidth,
        int screenHeight,
        ScreenBox& outBox,
        UpAxis upAxis = UpAxis::Z,
        float baseOffset = 0.0f) noexcept
    {
        Vec3 baseScreen, headScreen;
        const Vec3 footPos = OffsetUp(worldPos, baseOffset, upAxis);
        const Vec3 headPos = OffsetUp(worldPos, baseOffset + actorHeight, upAxis);

        if (!WorldToScreenTrig(footPos, camera, screenWidth, screenHeight, baseScreen, upAxis)) {
            outBox = ScreenBox{};
            return false;
        }
        if (!WorldToScreenTrig(headPos, camera, screenWidth, screenHeight, headScreen, upAxis)) {
            outBox = ScreenBox{};
            return false;
        }

        return BuildScreenBoxFromPoints(baseScreen, headScreen, outBox);
    }

    inline float Distance3D(const Vec3& a, const Vec3& b) noexcept {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    inline float DistanceMeters(const Vec3& a, const Vec3& b, float scale = 0.01f) noexcept {
        return Distance3D(a, b) * scale;
    }

    inline float Distance2D(float x1, float y1, float x2, float y2) noexcept {
        const float dx = x1 - x2;
        const float dy = y1 - y2;
        return std::sqrt(dx * dx + dy * dy);
    }

} // namespace UE4Math

#pragma once
#include <cstdint>

struct ANativeWindow;
struct ImDrawData;

/**
 * Selectable overlay rendering backend.
 *  - Auto:   Prefer Vulkan (dynamic libvulkan.so), automatically fall back to OpenGL ES
 *  - Vulkan: Prefer Vulkan (still falls back cleanly to OpenGL ES if unavailable)
 *  - OpenGL: Force EGL / OpenGL ES directly
 */
enum class GraphicsBackendType : uint8_t {
    Auto   = 0,
    Vulkan = 1,
    OpenGL = 2
};

inline const char* GraphicsBackendTypeToString(GraphicsBackendType type) noexcept {
    switch (type) {
        case GraphicsBackendType::Vulkan: return "Vulkan";
        case GraphicsBackendType::OpenGL: return "OpenGL ES";
        case GraphicsBackendType::Auto:
        default:                          return "Auto";
    }
}

/**
 * Pure abstract graphics backend interface composed by ImGuiCfg (no diamond inheritance).
 * Targets Dear ImGui 1.92 (ImGuiBackendFlags_RendererHasTextures + ImTextureData).
 */
class IGraphicsBackend {
public:
    virtual ~IGraphicsBackend() = default;

    virtual const char* GetName() const noexcept = 0;
    virtual GraphicsBackendType GetType() const noexcept = 0;

    virtual bool Create(ANativeWindow* window, int width, int height) = 0;
    virtual bool Rebind(ANativeWindow* window) = 0;
    virtual bool SetupImGui() = 0;
    virtual void NewFrame(ANativeWindow* window) = 0;
    virtual void Render(ImDrawData* drawData) = 0;
    virtual void ShutdownImGui() = 0;
    virtual void Cleanup() = 0;
};

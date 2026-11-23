#pragma once
#include <EGL/egl.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Graphics/GraphicsBackend.hpp"

struct ImTextureData;

/**
 * Native EGL / OpenGL ES (ES 3 with automatic ES 2 fallback) renderer for Dear ImGui 1.92.
 * Uses only ES 2 core calls so one compact renderer works across Android 5–17, supports
 * dynamic ImTextureData font atlases, highp UV precision, premultiplied alpha output for
 * SurfaceFlinger composition, and live surface Rebind() without losing ImGui context.
 */
class OpenGLGraphicsBackend final : public IGraphicsBackend {
public:
    OpenGLGraphicsBackend() = default;
    ~OpenGLGraphicsBackend() override { Cleanup(); }

    const char* GetName() const noexcept override {
        return (esVersion_ == 2) ? "OpenGL ES 2" : "OpenGL ES 3";
    }
    GraphicsBackendType GetType() const noexcept override { return GraphicsBackendType::OpenGL; }

    bool Create(ANativeWindow* window, int width, int height) override;
    bool Rebind(ANativeWindow* window) override;
    bool SetupImGui() override;
    void NewFrame(ANativeWindow* window) override;
    void Render(ImDrawData* drawData) override;
    void ShutdownImGui() override;
    void Cleanup() override;

private:
    static uint32_t CompileShader(uint32_t kind, const char* source);
    static uint32_t TextureNameOf(const ImTextureData& texture);
    bool CreateGlResources();
    void DestroyGlResources();
    void UpdateTexture(ImTextureData& texture);

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLConfig config_ = nullptr;
    int esVersion_ = 3;

    uint32_t program_ = 0;
    uint32_t vbo_ = 0;
    uint32_t ibo_ = 0;
    int32_t projLoc_ = -1;
    int32_t samplerLoc_ = -1;
    int32_t posLoc_ = -1;
    int32_t uvLoc_ = -1;
    int32_t colorLoc_ = -1;
    size_t maxTextureSide_ = 2048;
    std::vector<uint8_t> uploadScratch_;

    bool imguiInitialized_ = false;
};

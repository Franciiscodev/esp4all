#include "Graphics/OpenGLGraphicsBackend.hpp"

#include <GLES2/gl2.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace {

constexpr const char* kVertexShaderSrc = R"(
uniform mat4 u_projection;
attribute vec2 a_position;
attribute vec2 a_uv;
attribute vec4 a_color;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);
}
)";

// High-precision UVs prevent neighbour texel bleeding on large dynamic font atlases.
constexpr const char* kFragmentShaderSrc = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_texture;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_FragColor = v_color * texture2D(u_texture, v_uv);
}
)";

} // namespace

uint32_t OpenGLGraphicsBackend::CompileShader(uint32_t kind, const char* source) {
    const GLuint shader = glCreateShader(kind);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }

    char logBuf[512] = {};
    glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(logBuf)), nullptr, logBuf);
    std::printf("[OpenGL] Shader compile failed: %s\n", logBuf);
    glDeleteShader(shader);
    return 0;
}

uint32_t OpenGLGraphicsBackend::TextureNameOf(const ImTextureData& texture) {
    return static_cast<GLuint>(texture.GetTexID());
}

bool OpenGLGraphicsBackend::CreateGlResources() {
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShaderSrc);
    if (vs == 0) {
        return false;
    }
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);
    if (fs == 0) {
        glDeleteShader(vs);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char logBuf[512] = {};
        glGetProgramInfoLog(program_, static_cast<GLsizei>(sizeof(logBuf)), nullptr, logBuf);
        std::printf("[OpenGL] Program link failed: %s\n", logBuf);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    projLoc_ = glGetUniformLocation(program_, "u_projection");
    samplerLoc_ = glGetUniformLocation(program_, "u_texture");
    posLoc_ = glGetAttribLocation(program_, "a_position");
    uvLoc_ = glGetAttribLocation(program_, "a_uv");
    colorLoc_ = glGetAttribLocation(program_, "a_color");

    GLuint buffers[2] = {};
    glGenBuffers(2, buffers);
    vbo_ = buffers[0];
    ibo_ = buffers[1];

    GLint maxSide = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSide);
    if (maxSide > 0) {
        maxTextureSide_ = static_cast<size_t>(maxSide);
    }

    return true;
}

void OpenGLGraphicsBackend::DestroyGlResources() {
    if (vbo_ != 0 || ibo_ != 0) {
        const GLuint buffers[2] = {vbo_, ibo_};
        glDeleteBuffers(2, buffers);
        vbo_ = 0;
        ibo_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    uploadScratch_.clear();
}

bool OpenGLGraphicsBackend::Create(ANativeWindow* window, int /*width*/, int /*height*/) {
    if (!window) {
        return false;
    }

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        std::printf("[OpenGL] eglGetDisplay failed (0x%04x)\n", eglGetError());
        return false;
    }
    if (!eglInitialize(display_, nullptr, nullptr)) {
        std::printf("[OpenGL] eglInitialize failed (0x%04x)\n", eglGetError());
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    struct EsCandidate {
        int version;
        EGLint renderableBit;
    };
    const EsCandidate candidates[2] = {
        {3, 0x0040 /* EGL_OPENGL_ES3_BIT */},
        {2, EGL_OPENGL_ES2_BIT}
    };

    for (const EsCandidate& cand : candidates) {
        const EGLint configAttribs[] = {
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, cand.renderableBit,
            EGL_NONE
        };
        EGLConfig cfg = nullptr;
        EGLint numConfigs = 0;
        if (!eglChooseConfig(display_, configAttribs, &cfg, 1, &numConfigs) || numConfigs <= 0) {
            continue;
        }
        const EGLint ctxAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, cand.version,
            EGL_NONE
        };
        EGLContext ctx = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctxAttribs);
        if (ctx != EGL_NO_CONTEXT) {
            esVersion_ = cand.version;
            config_ = cfg;
            context_ = ctx;
            break;
        }
    }

    if (context_ == EGL_NO_CONTEXT) {
        std::printf("[OpenGL] Failed to create OpenGL ES 3/2 context (0x%04x)\n", eglGetError());
        Cleanup();
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE ||
        !eglMakeCurrent(display_, surface_, surface_, context_)) {
        std::printf("[OpenGL] Failed to create/bind EGL window surface (0x%04x)\n", eglGetError());
        Cleanup();
        return false;
    }

    eglSwapInterval(display_, 1);

    // Drain any stale driver error left behind during context creation.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }

    if (!CreateGlResources()) {
        Cleanup();
        return false;
    }

    return true;
}

bool OpenGLGraphicsBackend::Rebind(ANativeWindow* window) {
    if (!window || display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT || !config_) {
        return false;
    }

    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE ||
        !eglMakeCurrent(display_, surface_, surface_, context_)) {
        std::printf("[OpenGL] Rebind EGL window surface failed (0x%04x)\n", eglGetError());
        return false;
    }

    eglSwapInterval(display_, 1);
    return true;
}

bool OpenGLGraphicsBackend::SetupImGui() {
    if (imguiInitialized_) {
        return true;
    }
    if (display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT || program_ == 0) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = GetName();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = static_cast<int>(maxTextureSide_);
    platform.Renderer_TextureMaxHeight = static_cast<int>(maxTextureSide_);

    imguiInitialized_ = true;
    return true;
}

void OpenGLGraphicsBackend::NewFrame(ANativeWindow* /*window*/) {
    // OpenGL ES ImGui 1.92 backend updates textures and state directly inside Render().
}

void OpenGLGraphicsBackend::UpdateTexture(ImTextureData& texture) {
    auto packTightRgba = [&](int x, int y, int w, int h) -> const uint8_t* {
        const size_t rowBytes = static_cast<size_t>(w) * 4U;
        uploadScratch_.resize(rowBytes * static_cast<size_t>(h));
        for (int row = 0; row < h; ++row) {
            const auto* src = static_cast<const uint8_t*>(texture.GetPixelsAt(x, y + row));
            uint8_t* dst = uploadScratch_.data() + static_cast<size_t>(row) * rowBytes;
            if (texture.Format == ImTextureFormat_RGBA32) {
                std::memcpy(dst, src, rowBytes);
            } else {
                for (int col = 0; col < w; ++col) {
                    dst[col * 4 + 0] = 255;
                    dst[col * 4 + 1] = 255;
                    dst[col * 4 + 2] = 255;
                    dst[col * 4 + 3] = src[col];
                }
            }
        }
        return uploadScratch_.data();
    };

    if (texture.Status == ImTextureStatus_WantCreate) {
        GLuint texName = 0;
        glGenTextures(1, &texName);
        glBindTexture(GL_TEXTURE_2D, texName);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.Width, texture.Height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE,
                     packTightRgba(0, 0, texture.Width, texture.Height));
        texture.SetTexID(static_cast<ImTextureID>(texName));
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantUpdates) {
        glBindTexture(GL_TEXTURE_2D, TextureNameOf(texture));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int i = 0; i < texture.Updates.Size; ++i) {
            const ImTextureRect& r = texture.Updates[i];
            glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            packTightRgba(r.x, r.y, r.w, r.h));
        }
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantDestroy && texture.UnusedFrames > 0) {
        const GLuint texName = TextureNameOf(texture);
        if (texName != 0) {
            glDeleteTextures(1, &texName);
        }
        texture.SetTexID(ImTextureID_Invalid);
        texture.SetStatus(ImTextureStatus_Destroyed);
    }
}

void OpenGLGraphicsBackend::Render(ImDrawData* drawData) {
    if (!drawData || display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE || program_ == 0) {
        return;
    }

    if (drawData->Textures != nullptr) {
        for (int i = 0; i < drawData->Textures->Size; ++i) {
            ImTextureData* tex = (*drawData->Textures)[i];
            if (tex && tex->Status != ImTextureStatus_OK) {
                UpdateTexture(*tex);
            }
        }
    }

    const ImVec2 scale = drawData->FramebufferScale;
    const int fbW = static_cast<int>(drawData->DisplaySize.x * scale.x);
    const int fbH = static_cast<int>(drawData->DisplaySize.y * scale.y);
    if (fbW <= 0 || fbH <= 0) {
        return;
    }

    glViewport(0, 0, fbW, fbH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Output premultiplied RGBA so SurfaceFlinger composes translucent widgets accurately.
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);

    const float L = drawData->DisplayPos.x;
    const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const float T = drawData->DisplayPos.y;
    const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    const float ortho[16] = {
        2.0f / (R - L),     0.0f,               0.0f, 0.0f,
        0.0f,               2.0f / (T - B),     0.0f, 0.0f,
        0.0f,               0.0f,              -1.0f, 0.0f,
        (R + L) / (L - R),  (T + B) / (B - T),  0.0f, 1.0f
    };

    glUseProgram(program_);
    glUniformMatrix4fv(projLoc_, 1, GL_FALSE, ortho);
    glUniform1i(samplerLoc_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glEnableVertexAttribArray(static_cast<GLuint>(posLoc_));
    glEnableVertexAttribArray(static_cast<GLuint>(uvLoc_));
    glEnableVertexAttribArray(static_cast<GLuint>(colorLoc_));

    auto bindVertexBase = [&](size_t firstVtx) {
        const auto* base = reinterpret_cast<const uint8_t*>(firstVtx * sizeof(ImDrawVert));
        glVertexAttribPointer(static_cast<GLuint>(posLoc_), 2, GL_FLOAT, GL_FALSE,
                              sizeof(ImDrawVert), base + offsetof(ImDrawVert, pos));
        glVertexAttribPointer(static_cast<GLuint>(uvLoc_), 2, GL_FLOAT, GL_FALSE,
                              sizeof(ImDrawVert), base + offsetof(ImDrawVert, uv));
        glVertexAttribPointer(static_cast<GLuint>(colorLoc_), 4, GL_UNSIGNED_BYTE, GL_TRUE,
                              sizeof(ImDrawVert), base + offsetof(ImDrawVert, col));
    };

    static_assert(sizeof(ImDrawIdx) == 2, "OpenGL ES 2 renderer assumes 16-bit ImDrawIdx");

    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(list->VtxBuffer.Size) * static_cast<GLsizeiptr>(sizeof(ImDrawVert)),
                     list->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(list->IdxBuffer.Size) * static_cast<GLsizeiptr>(sizeof(ImDrawIdx)),
                     list->IdxBuffer.Data, GL_STREAM_DRAW);

        size_t boundVtxOffset = static_cast<size_t>(-1);
        for (int cmdIdx = 0; cmdIdx < list->CmdBuffer.Size; ++cmdIdx) {
            const ImDrawCmd& cmd = list->CmdBuffer[cmdIdx];
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) {
                    cmd.UserCallback(list, &cmd);
                }
                continue;
            }

            const float x0 = (cmd.ClipRect.x - L) * scale.x;
            const float y0 = (cmd.ClipRect.y - T) * scale.y;
            const float x1 = (cmd.ClipRect.z - L) * scale.x;
            const float y1 = (cmd.ClipRect.w - T) * scale.y;
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }

            glScissor(static_cast<GLint>(x0),
                      static_cast<GLint>(static_cast<float>(fbH) - y1),
                      static_cast<GLsizei>(x1 - x0),
                      static_cast<GLsizei>(y1 - y0));

            if (boundVtxOffset != cmd.VtxOffset) {
                boundVtxOffset = cmd.VtxOffset;
                bindVertexBase(boundVtxOffset);
            }

            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cmd.GetTexID()));
            glDrawElements(GL_TRIANGLES,
                           static_cast<GLsizei>(cmd.ElemCount),
                           GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd.IdxOffset) * sizeof(ImDrawIdx)));
        }
    }

    glDisable(GL_SCISSOR_TEST);
    eglSwapBuffers(display_, surface_);
}

void OpenGLGraphicsBackend::ShutdownImGui() {
    if (!imguiInitialized_) {
        return;
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent(display_, surface_, surface_, context_);
        }
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        for (int i = 0; i < platform.Textures.Size; ++i) {
            ImTextureData* tex = platform.Textures[i];
            if (!tex || tex->RefCount != 1 || tex->GetTexID() == ImTextureID_Invalid) {
                continue;
            }
            const GLuint texName = TextureNameOf(*tex);
            if (texName != 0) {
                glDeleteTextures(1, &texName);
            }
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
        platform.Renderer_TextureMaxWidth = 0;
        platform.Renderer_TextureMaxHeight = 0;
    }

    imguiInitialized_ = false;
}

void OpenGLGraphicsBackend::Cleanup() {
    ShutdownImGui();

    if (display_ != EGL_NO_DISPLAY) {
        if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE) {
            eglMakeCurrent(display_, surface_, surface_, context_);
            DestroyGlResources();
        }
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    config_ = nullptr;
}

#include "Graphics/GraphicsManager.hpp"
#include "Graphics/OpenGLGraphicsBackend.hpp"
#include "Graphics/VulkanGraphicsBackend.hpp"

std::unique_ptr<IGraphicsBackend> GraphicsManager::Create(GraphicsBackendType type) {
    if (type == GraphicsBackendType::OpenGL) {
        return std::make_unique<OpenGLGraphicsBackend>();
    }
    return std::make_unique<VulkanGraphicsBackend>();
}

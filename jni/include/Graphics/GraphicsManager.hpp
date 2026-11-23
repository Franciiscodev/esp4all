#pragma once
#include <memory>
#include "Graphics/GraphicsBackend.hpp"

/**
 * Factory creating the selected IGraphicsBackend implementation (Vulkan or OpenGL ES 3).
 */
class GraphicsManager {
public:
    static std::unique_ptr<IGraphicsBackend> Create(GraphicsBackendType type);
};

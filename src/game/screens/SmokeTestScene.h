#pragma once

#include <memory>

#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {

/** Throwaway scene that exercises texturing, alpha blending and reversed-Z depth. */
class SmokeTestScene {
public:
    void init(RenderDevice& device);
    void render(RenderDevice& device, const Mat4& projection, float timeSeconds);
    void shutdown();

private:
    std::unique_ptr<Texture> m_checkerTexture;
    ImmediateBatch m_batch;
};

} // namespace gdl::game

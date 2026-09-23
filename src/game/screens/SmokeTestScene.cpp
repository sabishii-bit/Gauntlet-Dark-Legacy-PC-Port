#include "game/screens/SmokeTestScene.h"

#include <array>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/render/DebugTextures.h"

namespace gdl::game {

namespace {

constexpr f32 kBackdropDepth = 0.05f;
constexpr f32 kHiddenDepth = 0.30f;
constexpr f32 kCheckerDepth = 0.50f;
constexpr f32 kTriangleDepth = 0.60f;
constexpr f32 kPanelDepth = 0.70f;

} // namespace

void SmokeTestScene::init(RenderDevice& device) {
    const auto pixels =
        makeCheckerboardRgba8(64, 8, Color::rgba(64, 40, 24), Color::rgba(224, 168, 72));
    m_checkerTexture = device.createTexture(TextureDesc{64, 64, TextureFilter::Nearest}, pixels);
    device.setClearColor(Vec4{0.02f, 0.02f, 0.03f, 1.0f});

    log::info("Smoke test: checkerboard quad, spinning RGB triangle in front, translucent panel "
              "on top, and a red quad hidden wherever the checkerboard covers it");
}

void SmokeTestScene::render(RenderDevice& device, const Mat4& projection, f32 timeSeconds) {
    m_batch.clear();
    m_batch.rect(Rect{0.0f, 0.0f, 640.0f, 448.0f}, kBackdropDepth, Color::rgba(28, 32, 56));
    device.draw(m_batch, device.whiteTexture(), projection);

    m_batch.clear();
    m_batch.rect(Rect{64.0f, 64.0f, 256.0f, 256.0f}, kCheckerDepth, Color::white());
    device.draw(m_batch, *m_checkerTexture, projection);

    m_batch.clear();
    m_batch.rect(Rect{200.0f, 200.0f, 300.0f, 200.0f}, kHiddenDepth, Color::rgba(200, 40, 40));

    const Vec2 centre{448.0f, 224.0f};
    constexpr f32 kRadius = 120.0f;
    constexpr std::array<Color, 3> kCorners{Color::rgba(255, 64, 64), Color::rgba(64, 255, 64),
                                            Color::rgba(64, 64, 255)};
    m_batch.begin(PrimitiveTopology::TriangleList);
    for (usize i = 0; i < kCorners.size(); ++i) {
        const f32 angle = timeSeconds + static_cast<f32>(i) * (kTwoPi / 3.0f);
        m_batch.vertex(Vec3{centre.x + kRadius * std::cos(angle),
                            centre.y + kRadius * std::sin(angle), kTriangleDepth},
                       kCorners[i], Vec2{0.0f, 0.0f});
    }
    m_batch.end();

    m_batch.rect(Rect{180.0f, 260.0f, 320.0f, 140.0f}, kPanelDepth, Color::rgba(40, 200, 255, 110));
    device.draw(m_batch, device.whiteTexture(), projection);
}

void SmokeTestScene::shutdown() {
    m_checkerTexture.reset();
}

} // namespace gdl::game

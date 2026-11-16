#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"
#include "expansions/shared/hud/LegendsHudArt.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("HUD art keeps one texture per name and finds it by name", "[legends][hud-art]") {
    test::FakeRenderDevice device;
    constexpr std::array<u8, 8> kTwoTexels{1, 2, 3, 4, 5, 6, 7, 8};

    // A look's pictures are filtered and clamped at their edges, never repeated across them.
    auto first = uploadHudTexture(device, 2, 1, kTwoTexels);
    REQUIRE(first != nullptr);
    CHECK(device.texturesCreated == 1);
    CHECK(device.lastTextureDesc.width == 2);
    CHECK(device.lastTextureDesc.height == 1);
    CHECK(device.lastTextureDesc.filter == TextureFilter::Linear);
    CHECK(device.lastTextureDesc.wrap == TextureWrap::ClampToEdge);
    CHECK(device.lastTextureDesc.wrapDown() == TextureWrap::ClampToEdge);
    CHECK(dynamic_cast<const test::FakeTexture&>(*first).pixels ==
          std::vector<u8>(kTwoTexels.begin(), kTwoTexels.end()));

    LegendsHudArt art;
    CHECK(art.size() == 0);
    CHECK(art.find("COLOR") == nullptr);
    const Texture* color = first.get();
    art.add("COLOR", std::move(first));
    auto border = uploadHudTexture(device, 2, 1, kTwoTexels);
    const Texture* borderTexture = border.get();
    art.add("BORDER", std::move(border));
    CHECK(art.size() == 2);
    CHECK(art.find("COLOR") == color);
    CHECK(art.find("BORDER") == borderTexture);
    CHECK(art.find("COLORI") == nullptr);
    CHECK(art.find("color") == nullptr); // names are matched exactly

    // A second picture under a name takes the first one's place: one texture a name.
    auto again = uploadHudTexture(device, 2, 1, kTwoTexels);
    const Texture* replacement = again.get();
    art.add("COLOR", std::move(again));
    CHECK(art.size() == 2);
    CHECK(art.find("COLOR") == replacement);
    CHECK(art.find("BORDER") == borderTexture);

    art.clear();
    CHECK(art.size() == 0);
    CHECK(art.find("COLOR") == nullptr);
    CHECK(art.find("BORDER") == nullptr);
}

} // namespace

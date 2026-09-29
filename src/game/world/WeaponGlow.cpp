#include "game/world/WeaponGlow.h"

#include <algorithm>
#include <format>

#include "game/combat/DamageTypes.h"
#include "game/players/ClassData.h"

namespace gdl::game {
namespace {
constexpr s32 kWizardClass = 2;
constexpr s32 kSorceressClass = 6;
} // namespace

u32 WeaponGlow::elementOf(const PowerupEffects& worn) {
    const bool handFull = (worn.special & powerup::kRightGauntlet) != 0 ||
                          (worn.weapon & (powerup::kSuperShot | powerup::kThunderHammer)) != 0;
    return handFull ? 0 : damage::element(worn.weapon);
}

bool WeaponGlow::throwsEffectAlone(s32 character) {
    const s32 shadowed = character % kStartingClassCount;
    return shadowed == kWizardClass || shadowed == kSorceressClass;
}

usize WeaponGlow::tierOf(s32 level) {
    return static_cast<usize>(std::clamp(level / 10, 0, static_cast<s32>(kTiers) - 1));
}

std::string WeaponGlow::holdTree(u32 element) {
    const std::string_view colour = damage::colourOf(element);
    return colour.empty() ? std::string{} : std::format("WEAP_HOLD_{}", colour);
}

std::string WeaponGlow::throwTree(u32 element) {
    const std::string_view colour = damage::colourOf(element);
    return colour.empty() ? std::string{} : std::format("WEAP_TW_{}", colour.front());
}

void WeaponGlow::update(RenderDevice& device, EffectTrees& effects, ItemArchive* archive,
                        u32 element, const Mat4& hand, const Vec3& offset, const Vec3& scale) {
    if (element != m_element || (m_effect != 0 && !effects.playing(m_effect))) {
        clear(effects);
        m_element = element;
    }
    if (element == 0 || archive == nullptr || !archive->loaded()) {
        return;
    }
    if (m_effect == 0) {
        const std::string tree = holdTree(element);
        if (!archive->trees.find(tree).has_value()) {
            return;
        }
        EffectTrees::Setting setting;
        setting.persistent = true;
        m_effect = effects.startSet(device, *archive, tree, Vec3{hand[3]}, setting);
    }
    if (m_effect != 0) {
        Mat4 at = glm::translate(hand, offset);
        if (scale.x != 0.0f || scale.y != 0.0f || scale.z != 0.0f) {
            at = glm::scale(at, scale);
        }
        effects.placeAt(m_effect, at);
    }
}

void WeaponGlow::clear(EffectTrees& effects) {
    if (m_effect != 0) {
        effects.stop(m_effect);
    }
    m_effect = 0;
    m_element = 0;
}

} // namespace gdl::game

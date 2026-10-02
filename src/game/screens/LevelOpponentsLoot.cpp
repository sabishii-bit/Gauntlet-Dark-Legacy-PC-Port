#include <algorithm>

#include "game/screens/LevelOpponents.h"

namespace gdl::game {
namespace {
constexpr f32 kBagGravity = 50; ///< StartBagFX's weight
constexpr f32 kBagRadius = 1;
constexpr f32 kBagLifetime = 3;
constexpr f32 kPickupDelay = 10.0f / 60.0f;
} // namespace

void LevelOpponents::releaseBag(const Bag& bag) {
    if (!m_resources.has_value()) {
        return;
    }
    auto& resources = *m_resources;
    if (bag.item) {
        resources.world.releaseItem(*bag.item, bag.position, Vec3{0}, kPickupDelay);
    } else {
        resources.world.throwItem(resources.device, bag.name, bag.position, Vec3{0}, kPickupDelay);
    }
}

void LevelOpponents::updateBags(f32 seconds) {
    if (!m_resources.has_value()) {
        return;
    }
    auto& resources = *m_resources;
    for (Bag& bag : m_bags) {
        bag.age += seconds;
        if (!bag.landed) {
            const f32 previousY = bag.position.y;
            bag.position.y += bag.velocity * seconds - 0.5f * kBagGravity * seconds * seconds;
            bag.velocity -= kBagGravity * seconds;
            bag.pitch += bag.spin * seconds;
            if (bag.velocity <= 0) {
                const auto floor = resources.world.collision().floorAt(
                    bag.position, std::max(kBagRadius, previousY - bag.position.y), kBagRadius);
                if (floor && bag.position.y <= floor->y + kBagRadius) {
                    bag.position.y = floor->y;
                    bag.landed = true;
                    resources.effects.stop(bag.effect);
                    bag.effect = resources.effects.startSet(resources.device, resources.weapons,
                                                            "BAG_HIT", bag.position, {});
                }
            }
            if (!bag.landed) {
                resources.effects.placeAt(
                    bag.effect,
                    glm::rotate(glm::translate(Mat4{1}, bag.position), bag.pitch, Vec3{1, 0, 0}));
            }
        }
        if (!resources.effects.playing(bag.effect) || bag.age >= kBagLifetime) {
            resources.effects.stop(bag.effect);
            releaseBag(bag);
            bag.effect = 0;
        }
    }
    std::erase_if(m_bags, [](const Bag& bag) { return bag.effect == 0; });
}
} // namespace gdl::game

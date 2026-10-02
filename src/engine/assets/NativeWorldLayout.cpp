#include <exception>
#include <utility>

#include "engine/assets/NativeParticleTemplate.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "formats/WorldFile.h"

namespace gdl {

bool WorldLayout::loadNative(const std::filesystem::path& file) {
    try {
        auto source = formats::WorldFile::parse(readFile(file));
        m_minBounds = source.minBounds;
        m_maxBounds = source.maxBounds;
        for (auto& entry : source.objects) {
            WorldObject object;
            object.name = std::move(entry.name);
            object.position = entry.position;
            object.flags = entry.flags;
            object.objectFlags = entry.objectFlags;
            object.noCollision = entry.noCollision;
            object.next = entry.nextIndex;
            object.child = entry.childIndex;
            object.radius = entry.radius;
            m_objects.push_back(std::move(object));
        }
        for (const auto& entry : source.locators) {
            m_locators.push_back(
                {entry.kind, entry.delay, entry.next, entry.position, entry.rotation});
        }
        for (auto& entry : source.animations) {
            WorldAnimation animation;
            animation.object = entry.objectIndex;
            animation.frames = entry.frameCount;
            animation.state = entry.state;
            animation.start = entry.startFrame;
            animation.track.flags = entry.track.flags;
            animation.track.frames = std::move(entry.track.frames);
            animation.track.values = std::move(entry.track.values);
            if (animation.object < 0 || static_cast<usize>(animation.object) >= m_objects.size() ||
                animation.track.frames.empty() ||
                animation.track.values.size() !=
                    animation.track.frames.size() * animation.track.channelCount()) {
                throw FormatError("world animation has invalid object or keys");
            }
            m_animations.push_back(std::move(animation));
        }
        for (const auto& entry : source.particles) {
            m_particles.push_back(nativeParticle(entry));
        }
        for (const auto& entry : source.itemInfos) {
            ItemInfo info;
            info.type = entry.type;
            info.subtype = entry.subtype;
            info.name = normalizeAssetName(entry.name);
            info.radius = entry.radius;
            info.height = entry.height;
            info.xSize = entry.xSize;
            info.zSize = entry.zSize;
            info.collisionType = entry.collisionType;
            info.collisionFlags = static_cast<u16>(entry.collisionFlags);
            info.collisionOffset = entry.collisionOffset;
            info.objectFlags = entry.objectFlags;
            info.properties = entry.properties;
            info.value = entry.value;
            info.armor = entry.armor;
            info.hitPoints = entry.hitPoints;
            info.activeType = entry.activeType;
            info.activeOff = entry.activeOff;
            info.activeOn = entry.activeOn;
            info.choices.assign(entry.choices.begin(), entry.choices.end());
            m_itemInfos.push_back(std::move(info));
        }
        for (const auto& entry : source.itemInstances) {
            ItemInstance instance;
            instance.info = entry.info;
            // This is a signed count in the disc record, not a character encoding.
            // NOLINTNEXTLINE(bugprone-signed-char-misuse)
            instance.minPlayers = static_cast<s32>(entry.minPlayers);
            instance.flags = entry.flags;
            instance.name = normalizeAssetName(entry.name);
            instance.position = entry.position;
            instance.rotation = entry.rotation;
            instance.params = entry.params;
            if (entry.triangleIndex >= 0 && entry.triangleCount > 0) {
                const auto first = static_cast<usize>(entry.triangleIndex);
                const auto count = static_cast<usize>(entry.triangleCount);
                if (first + count > source.collision.size()) {
                    throw FormatError("item collision outside world triangle table");
                }
                for (usize i = first; i < first + count; ++i) {
                    const auto& triangle = source.collision[i];
                    instance.collision.push_back({triangle.normal, triangle.vertices});
                }
            }
            m_itemInstances.push_back(std::move(instance));
        }
        resolveParents();
        return loaded();
    } catch (const std::exception& e) {
        log::warn("Native world layout {}: {}", file.string(), e.what());
        *this = WorldLayout{};
        return false;
    }
}

} // namespace gdl

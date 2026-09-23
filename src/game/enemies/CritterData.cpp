#include "game/enemies/CritterData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl::game {

namespace {

using Json = nlohmann::json;

Vec3 vecOf(const Json& json, const char* key) {
    const auto values = json.value(key, std::vector<f32>{});
    return values.size() >= 3 ? Vec3{values[0], values[1], values[2]} : Vec3{0.0f, 0.0f, 0.0f};
}

CritterTarget targetOf(const Json& json) {
    CritterTarget target;
    if (const auto t = json.find("target"); t != json.end() && t->is_object()) {
        target.minDistance = t->value("minDistance", 0.0f);
        target.maxDistance = t->value("maxDistance", 0.0f);
        target.yaw = t->value("yaw", 0.0f);
        target.minDot = t->value("minDot", -1.0f);
        target.maxVertical = t->value("maxVertical", 0.0f);
    }
    return target;
}

std::string lower(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

bool CritterTarget::allows(f32 distance, f32 bearing, f32 vertical) const {
    if (distance < minDistance) {
        return false;
    }
    if (maxDistance > 0.0f && distance > maxDistance) {
        return false;
    }
    if (std::cos(bearing - yaw) < minDot) {
        return false;
    }
    return maxVertical <= 0.0f || std::abs(vertical) <= maxVertical;
}

/** The body's way turned by the record's yaw, then tipped by its pitch (under nought, up),
 * at its speed. */
Vec3 CritterDamage::spewVelocity(f32 bodyYaw) const {
    const f32 heading = bodyYaw + yaw;
    const f32 level = std::cos(-pitch);
    return Vec3{std::sin(heading) * level, std::sin(-pitch), std::cos(heading) * level} * speed;
}

f32 CritterDamage::spewHalfAngle() const {
    return std::acos(std::clamp(minDot, -1.0f, 1.0f));
}

bool CritterData::load(const std::filesystem::path& file) {
    try {
        const Json root = Json::parse(readTextFile(file), nullptr, true, true);
        const auto types = root.value("types", Json::array());
        const auto descriptors = root.value("descriptors", Json::array());
        if (types.empty() || descriptors.empty()) {
            return false;
        }
        // The first type is the creature; children (chimera heads) come after it.
        const Json& type = types.front();
        const auto descriptor = static_cast<usize>(std::max(type.value("descriptor", 0), 0));
        const Json& desc = descriptors[std::min(descriptor, descriptors.size() - 1)];
        m_name = root.value("name", "");
        m_folder = lower(desc.value("name", ""));
        m_prefix = desc.value("prefix", "");
        m_kind = desc.value("type", 0);
        m_suffix = type.value("suffix", "");
        m_radius = type.value("radius", 1.0f);
        m_wallRadius = type.value("wallRadius", 1.0f);
        m_armor = type.value("armor", 0.0f);
        m_maxHealth = type.value("maxHealth", 1.0f);
        m_experience = type.value("expValue", 0.0f);
        m_wake = type.value("wakeThreshold", 0.0f);
        m_vertDrift = type.value("vertDrift", 0.0f);
        m_floorOffset = type.value("floorOffset", 0.0f);
        m_originOffset = vecOf(type, "originOffset");
        m_sight = targetOf(type);
        const u32 typeFlags = type.value("typeFlags", 0U);
        m_meter.pieces = type.value("meterPieces", 0);
        m_meter.advance = type.value("meterAdvance", 0);
        m_meter.leftInset = type.value("meterLeftInset", 0);
        m_meter.rightInset = type.value("meterRightInset", 0);
        m_meter.shown = (typeFlags & CritterMeter::kShown) != 0 && m_meter.pieces > 0;
        m_meter.backed = (typeFlags & CritterMeter::kBacked) != 0;
        m_meter.barOffset = vecOf(type, "healthBarOffset");
        const s32 moveIndex = type.value("moveIndex", 0);
        const s32 moveCount = type.value("moveCount", 0);
        const auto moves = root.value("moves", Json::array());
        for (s32 i = 0; i < moveCount; ++i) {
            const auto at = static_cast<usize>(moveIndex) + static_cast<usize>(i);
            if (at >= moves.size()) {
                break;
            }
            const Json& m = moves[at];
            CritterMove move;
            move.type = m.value("type", 0);
            move.flags = m.value("flags", 0U);
            move.priority = m.value("priority", 0);
            move.name = m.value("name", "");
            move.anim = m.value("anim", "");
            move.colnode = m.value("colnode", "");
            move.frameStart = m.value("frameStart", -1);
            move.frameEnd = m.value("frameEnd", -1);
            move.frameStart2 = m.value("frameStart2", -1);
            move.frameEnd2 = m.value("frameEnd2", -1);
            move.damage0 = m.value("damage0", -1);
            move.damage1 = m.value("damage1", -1);
            move.link = m.value("link", -1);
            move.interrupt = m.value("interrupt", 0);
            move.sound = m.value("sfx", -1);
            move.soundFrame = m.value("sfxFrame", 0);
            move.sound2 = m.value("sfx2", -1);
            move.sound2Frame = m.value("sfx2Frame", 0);
            move.target = targetOf(m);
            move.cooldown = m.value("cooldown", 0.0f);
            move.speed = m.value("speed", 0.0f);
            move.turnRate = m.value("turnRate", 0.0f);
            move.hold = m.value("hold", 0.0f);
            // A tab or nothing names no node; the packing tool's leftovers follow.
            if (!move.colnode.empty() && std::isspace(static_cast<unsigned char>(move.colnode.front())) != 0) {
                move.colnode.clear();
            }
            m_moves.push_back(move);
        }
        for (const Json& d : root.value("damages", Json::array())) {
            CritterDamage damage;
            damage.type = static_cast<s16>(d.value("type", 0));
            damage.flags = d.value("flags", 0U);
            damage.radius = d.value("radius", 0.0f);
            damage.maxDistance = d.value("maxDistance", 0.0f);
            damage.yaw = d.value("yaw", 0.0f);
            damage.minDot = d.value("minDot", 0.0f);
            damage.pitch = d.value("pitch", 0.0f);
            damage.offset = vecOf(d, "offset");
            damage.damage = d.value("damage", 0.0f);
            damage.speed = d.value("minSpeed", 0.0f);
            damage.sound = d.value("sfxIndex", -1);
            m_damages.push_back(damage);
        }
        for (const Json& s : root.value("sounds", Json::array())) {
            CritterSound sound;
            sound.tree = s.value("name", "");
            sound.soundFormat = s.value("levelFormat", "");
            sound.flags = s.value("flags", 0U);
            sound.link = s.value("link", -1);
            sound.offset = vecOf(s, "offset");
            sound.life = s.value("life", 0.0f);
            sound.scale = s.value("scale", 1.0f);
            if (sound.scale <= 0.0f) {
                sound.scale = 1.0f;
            }
            m_sounds.push_back(sound);
        }
        m_hitSoundFar = type.value("hitSoundFar", -1);
        m_hitSoundClose = type.value("hitSoundClose", -1);
        const s32 colBase = type.value("colBase", 0);
        const s32 colCount = type.value("colCount", 0);
        const auto nodes = root.value("nodes", Json::array());
        for (s32 i = 0; i < colCount; ++i) {
            const auto at = static_cast<usize>(colBase) + static_cast<usize>(i);
            if (at >= nodes.size()) {
                break;
            }
            const Json& n = nodes[at];
            CritterPart part;
            part.node = n.value("nodeName", "");
            part.position = vecOf(n, "position");
            part.radius = n.value("radius", 0.0f);
            part.damageScale = n.value("damageScale", 1.0f);
            m_parts.push_back(part);
        }
        return loaded();
    } catch (const std::exception& e) {
        log::warn("critter data {}: {}", file.string(), e.what());
        return false;
    }
}

std::string CritterSound::soundFor(char letter) const {
    std::string name = soundFormat;
    if (const auto at = name.find("%c"); at != std::string::npos) {
        name.replace(at, 2, 1, letter);
    }
    return name;
}

const CritterSound* CritterData::sound(s32 index) const {
    return index >= 0 && static_cast<usize>(index) < m_sounds.size()
               ? &m_sounds[static_cast<usize>(index)]
               : nullptr;
}

const CritterDamage* CritterData::damage(s32 index) const {
    return index >= 0 && static_cast<usize>(index) < m_damages.size()
               ? &m_damages[static_cast<usize>(index)]
               : nullptr;
}

std::optional<usize> CritterData::moveOfType(s32 type) const {
    for (usize i = 0; i < m_moves.size(); ++i) {
        if (m_moves[i].type == type) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<usize> CritterData::moveNamed(std::string_view name) const {
    for (usize i = 0; i < m_moves.size(); ++i) {
        if (m_moves[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::game

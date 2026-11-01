#include "game/mechanics/MechanicsFacade.h"

#include <algorithm>

namespace gdl::game {
namespace {

/** One channel laid over another; full leaves the other as it was. */
constexpr u8 layered(u8 over, u8 under) {
    return static_cast<u8>((u32{over} * u32{under} + 127U) / 255U);
}

/** One tint laid over another, channel by channel; white leaves the other as it was. */
constexpr Color layered(Color over, Color under) {
    return Color{layered(over.r, under.r), layered(over.g, under.g), layered(over.b, under.b),
                 layered(over.a, under.a)};
}
static_assert(layered(Color::white(), Color::rgba(165, 30, 30)) == Color::rgba(165, 30, 30));

} // namespace

MechanicsFacade::~MechanicsFacade() {
    close();
}

const MechanicsFacade& MechanicsFacade::standard() {
    static const MechanicsFacade kStandard;
    return kStandard;
}

void MechanicsFacade::open(const MechanicCatalog* catalog, const MechanicLevel& level) {
    close();
    // A snapshot of the level before belongs to it alone.
    m_binding = std::make_shared<const u8>(0);
    if (catalog == nullptr) {
        return;
    }
    for (const MechanicDescriptor& descriptor : catalog->all()) {
        Member member{&descriptor, descriptor.create(), false};
        if (member.mechanic) {
            member.mechanic->open(level);
            m_members.push_back(std::move(member));
        }
    }
}

void MechanicsFacade::close() {
    for (Member& member : m_members) {
        member.mechanic->close();
    }
    m_members.clear();
}

void MechanicsFacade::configure(const GameOptionsConfig& options) {
    for (Member& member : m_members) {
        const bool on = MechanicCatalog::enabled(options, member.descriptor->id);
        if (on != member.enabled) {
            member.enabled = on;
            member.mechanic->enable(on);
        }
    }
}

const GameplayMechanic* MechanicsFacade::find(std::string_view id) const {
    const auto found = std::ranges::find_if(
        m_members, [id](const Member& member) { return member.descriptor->id == id; });
    return found != m_members.end() ? found->mechanic.get() : nullptr;
}

std::optional<RunEffect> MechanicsFacade::runEffect(const BlowReport& blow) const {
    for (const Member& member : m_members) {
        if (const BlowRules* rules = member.mechanic->blows()) {
            if (const auto effect = rules->runEffect(blow)) {
                return effect;
            }
        }
    }
    return std::nullopt;
}

bool MechanicsFacade::generatorTakes(BlowSource source) const {
    return std::ranges::all_of(m_members, [source](const Member& member) {
        const BlowRules* rules = member.mechanic->blows();
        return rules == nullptr || rules->generatorTakes(source);
    });
}

std::optional<f32> MechanicsFacade::quietWindow(s32 count) const {
    for (const Member& member : m_members) {
        if (const RunRules* rules = member.mechanic->runs()) {
            if (const auto quiet = rules->quietWindow(count)) {
                return quiet;
            }
        }
    }
    return std::nullopt;
}

void MechanicsFacade::step(const SwarmStep& step) {
    for (Member& member : m_members) {
        if (SwarmRules* rules = member.mechanic->swarm()) {
            rules->step(step);
        }
    }
}

SwarmTuning MechanicsFacade::tuning() const {
    SwarmTuning tuning;
    for (const Member& member : m_members) {
        const SwarmRules* rules = member.mechanic->swarm();
        if (rules == nullptr) {
            continue;
        }
        const SwarmTuning own = rules->tuning();
        tuning.breedingPace *= own.breedingPace;
        tuning.broodScale *= own.broodScale;
        tuning.speedScale *= own.speedScale;
        tuning.attackPace *= own.attackPace;
        tuning.healthScale *= own.healthScale;
        tuning.frenzyReach = std::max(tuning.frenzyReach, own.frenzyReach);
        tuning.wholePool = tuning.wholePool || own.wholePool;
    }
    return tuning;
}

BodyTraits MechanicsFacade::traitsFor(const BirthRequest& birth, Mt19937& random) const {
    for (const Member& member : m_members) {
        if (const BirthRules* rules = member.mechanic->births()) {
            const BodyTraits traits = rules->traitsFor(birth, random);
            if (traits != BodyTraits{}) {
                return traits;
            }
        }
    }
    return {};
}

void MechanicsFacade::fell(const BodyFall& fall, BagDrops& drops) {
    for (Member& member : m_members) {
        if (FallRules* rules = member.mechanic->falls()) {
            rules->fell(fall, drops);
        }
    }
}

SwarmLook MechanicsFacade::look(u32 frame) const {
    SwarmLook look;
    for (const Member& member : m_members) {
        const SwarmAppearance* appearance = member.mechanic->appearance();
        if (appearance == nullptr) {
            continue;
        }
        const SwarmLook own = appearance->look(frame);
        look.tint = layered(own.tint, look.tint);
        if (own.marked && !look.marked) {
            look.marked = true;
            look.rim = own.rim;
            look.wash = own.wash;
            look.rimWidth = own.rimWidth;
        }
    }
    return look;
}

f32 MechanicsFacade::bodyScale(s32 affinity) const {
    f32 scale = 1.0f;
    for (const Member& member : m_members) {
        if (const SwarmAppearance* appearance = member.mechanic->appearance()) {
            scale *= appearance->bodyScale(affinity);
        }
    }
    return scale;
}

void MechanicsFacade::drawWorld(const WorldOverlayView& view) {
    for (Member& member : m_members) {
        if (WorldOverlay* overlay = member.mechanic->world()) {
            overlay->drawWorld(view);
        }
    }
}

void MechanicsFacade::drawHud(const HudOverlayView& view) {
    for (Member& member : m_members) {
        if (HudOverlay* overlay = member.mechanic->hud()) {
            overlay->drawHud(view);
        }
    }
}

void MechanicsFacade::saveLocal(LocalSnapshot& saved) const {
    // A snapshot of another level holds states of that level's instances: make them anew.
    if (saved.m_owner != this || saved.m_binding.lock() != m_binding ||
        saved.m_states.size() != m_members.size()) {
        saved.m_states.clear();
        for (const Member& member : m_members) {
            saved.m_states.push_back(member.mechanic->makeState());
        }
    }
    saved.m_enabled.resize(m_members.size());
    for (usize i = 0; i < m_members.size(); ++i) {
        m_members[i].mechanic->saveState(*saved.m_states[i]);
        saved.m_enabled[i] = m_members[i].enabled ? 1 : 0;
    }
    saved.m_owner = this;
    saved.m_binding = m_binding;
}

bool MechanicsFacade::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && saved.m_binding.lock() == m_binding &&
           saved.m_states.size() == m_members.size() && saved.m_enabled.size() == m_members.size();
}

bool MechanicsFacade::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize i = 0; i < m_members.size(); ++i) {
        Member& member = m_members[i];
        if (!member.mechanic->restoreState(*saved.m_states[i])) {
            return false;
        }
        const bool on = saved.m_enabled[i] != 0;
        if (on != member.enabled) {
            member.enabled = on;
            member.mechanic->enable(on);
        }
    }
    return true;
}

bool MechanicsFacade::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("MechanicsFacade.native.1");
    digest.wide(m_members.size());
    for (usize i = 0; i < m_members.size(); ++i) {
        const Member& member = m_members[i];
        digest.string(member.descriptor->id);
        digest.word(member.descriptor->schema);
        digest.flag(saved != nullptr ? saved->m_enabled[i] != 0 : member.enabled);
        if (!member.mechanic->appendState(digest,
                                          saved != nullptr ? saved->m_states[i].get() : nullptr)) {
            digest.invalidate();
            return false;
        }
    }
    return digest.good();
}

bool MechanicsFacade::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool MechanicsFacade::appendLocalDigest(NativeStateDigest& digest,
                                        const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

} // namespace gdl::game

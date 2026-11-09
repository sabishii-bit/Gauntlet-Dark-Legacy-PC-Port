#include "expansions/dreamcast/level/LegendsDreamcastGates.h"

#include <algorithm>
#include <utility>

#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::bindDreamcastGate(const formats::ItemInfoRecord& info,
                                            const formats::ItemInstanceRecord& instance,
                                            u32 nativeIndex, usize figure) {
    std::string diagnostic;
    auto definition = legendsDreamcastGate(info, instance, nativeIndex, &diagnostic);
    if (!definition) {
        if (!diagnostic.empty() && std::ranges::find(m_warnings, diagnostic) == m_warnings.end())
            m_warnings.push_back(std::move(diagnostic));
        return false;
    }
    if (figure >= m_fixtures.size())
        return false;
    const auto* tree = m_fixtures[figure].tree();
    const auto closed = tree ? tree->findSequence("CLOSE") : std::nullopt;
    const auto opening = tree ? tree->findSequence("ACTIV") : std::nullopt;
    const auto opened = tree ? tree->findSequence("OPEN") : std::nullopt;
    if (!closed || !opening || !opened) {
        m_warnings.push_back("Dreamcast key door is missing its CLOSE/ACTIV/OPEN sequences: " +
                             (instance.name.empty() ? info.name : instance.name));
        return false;
    }
    definition->position = m_fixtures[figure].position();
    definition->sound = dreamcastGateSound(m_dreamcastRealm, info.subtype);
    m_fixtures[figure].play(static_cast<s32>(*closed), ItemFigure::Playback::Loop);
    m_fixtures[figure].capturePresentation();
    // Dreamcast retains one animated tree throughout; N64 retains its separate
    // closed/opening TRAP figures. Both share one key transaction and obstacles.
    m_gates.push_back({LegendsGate{std::move(*definition)}, figure, figure,
                       static_cast<s32>(*opening), static_cast<s32>(*opened)});
    return true;
}

} // namespace gdl::game

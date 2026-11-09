#include <algorithm>
#include <utility>

#include "engine/platform/Paths.h"

#include "expansions/n64/scenes/LegendsSumnerText.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

void LegendsLevel::updateExitReminder(std::span<const CharacterSave* const> party) {
    m_exitReminder.reset();
    if (platform() != LegendsPlatform::N64) {
        return;
    }
    std::vector<LegendsProgress> progress;
    for (const auto* save : party) {
        if (save && save->character >= 0 && save->character < kClassCount) {
            progress.push_back(save->progress().legendsProgress(LegendsPlatform::N64));
        }
    }
    m_exitReminder =
        legendsN64DepartureCue(paths::utf8(m_directory.filename()), adapter().pickups(), progress);
}

void LegendsLevel::touchScenery(s32 player, const Vec3& from, const Vec3& to, f32 radius,
                                f32 height) {
    m_world.adapter()->touchDeaths(to, radius, height);
    if (m_sumner && player >= 0 && static_cast<usize>(player) < m_podiumContacts.size()) {
        m_podiumContacts[static_cast<usize>(player)] |=
            m_sumner->podiumContact(from, to, radius, height);
    }
}

void LegendsLevel::announceProgress(std::span<const PartyMember> before,
                                    std::span<const PartyMember> after) {
    if (!m_sumner) {
        return;
    }
    const auto progress = [](std::span<const PartyMember> party) {
        std::vector<LegendsProgress> result;
        for (const auto& member : party) {
            if (!member.fallen && member.save.character >= 0 &&
                member.save.character < kClassCount) {
                result.push_back(member.save.progress().legendsProgress(LegendsPlatform::N64));
            }
        }
        return result;
    };
    const auto old = progress(before);
    const auto current = progress(after);
    const auto combined = [](std::span<const LegendsProgress> party) {
        LegendsProgress result;
        for (const auto& value : party) {
            result.runestones |= value.runestones;
            result.shards |= value.shards;
            result.obelisks |= value.obelisks;
        }
        return result;
    };
    m_world.adapter()->queueTowerCeremonies(combined(old), combined(current));
    m_historyMilestone = m_sumner->milestone(current);
}

bool LegendsLevel::updateN64Sumner(std::span<PlayerActor*> players, f32 seconds, bool blocked) {
    if (!m_sumner) {
        return false;
    }
    std::vector<LegendsSumnerVisitor> visitors;
    m_hintProgress.clear();
    for (const auto* actor : players) {
        if (!actor || actor->player() < 0 || static_cast<usize>(actor->player()) >= 4) {
            continue;
        }
        const auto seat = static_cast<usize>(actor->player());
        visitors.push_back({actor->player(), m_podiumContacts[seat], true});
        m_hintProgress.push_back(actor->save().progress().legendsProgress(LegendsPlatform::N64));
    }
    m_podiumContacts.fill(false);
    m_historyMilestone = m_sumner->milestone(m_hintProgress);
    const auto* ceremony = adapter().towerCeremony();
    if (ceremony && (ceremony->active() || ceremony->pending())) {
        if (!ceremony->active() && !blocked && !m_dialogueOpen && !visitors.empty() &&
            (!m_camera || !m_camera->introActive())) {
            const auto view = camera();
            const auto focus = cameraAttention();
            if (view && focus) {
                m_world.adapter()->startTowerCeremonies(*view, *focus);
            }
        }
        return true;
    }
    const auto owner = m_sumner->update(seconds, visitors, blocked || m_dialogueOpen);
    if (!owner) {
        return false;
    }
    ImportedDialogue request;
    request.player = *owner;
    request.title = legendsN64SumnerTextKey(m_sumner->titleId());
    for (const auto& choice : m_sumner->choices()) {
        request.topics.push_back(
            {static_cast<s32>(choice.action), legendsN64SumnerTextKey(choice.textId)});
    }
    m_dialogue = std::move(request);
    m_dialogueOpen = true;
    return false;
}

std::vector<std::string> LegendsLevel::answerN64Dialogue(s32 topic) {
    if (!m_sumner || !m_dialogueOpen || topic < 0 ||
        std::ranges::find(m_sumner->choices(), static_cast<u32>(topic),
                          &LegendsSumnerChoice::action) == m_sumner->choices().end()) {
        return {};
    }
    if (topic == 0x15) {
        m_shopRequested = true;
    } else if (topic == 2) {
        // Restore the native chapter after load as well as during this visit.
        if (const auto page = m_sumner->historyPage(m_historyMilestone, m_hintProgress)) {
            const auto key = legendsN64SumnerHistoryKey(*page);
            if (!key.empty()) {
                return {key};
            }
        }
    } else if (const auto identity =
                   m_sumner->adviceTextId(static_cast<u32>(topic), m_hintProgress)) {
        const auto key = legendsN64SumnerTextKey(*identity);
        if (!key.empty()) {
            return {key};
        }
    }
    return {};
}

} // namespace gdl::game

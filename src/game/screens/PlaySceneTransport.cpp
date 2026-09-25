#include "game/screens/PlayScene.h"

namespace gdl::game {

void PlayScene::updateTransporters(s32 ticks, f32 seconds, bool held) {
    m_transporters.animate(seconds);
    if (held) {
        return;
    }
    for (usize i = 0; i < m_players.size(); ++i) {
        auto& runtime = m_players[i];
        auto& actor = runtime.actor;
        auto& transport = runtime.transport;
        if (runtime.life != PlayerLife::Standing || runtime.capture.active()) {
            transport.clear();
            continue;
        }
        if (transport.active()) {
            if (auto destination = transport.update(ticks)) {
                // Moving geometry can have changed since the departure was accepted.
                if (const auto floor = m_world->collision().floorAt(*destination, 4.0f, 10.0f)) {
                    destination->y = floor->y + PlayerActor::kFootClearance;
                    actor.place(*destination);
                    postHelp(9, i);
                } else {
                    // A failed landing must not repeatedly retrigger while standing here.
                    transport.cancel();
                }
            }
            continue;
        }
        const auto source =
            m_transporters.contact(actor.position(), actor.radius(), actor.height());
        transport.observeContact(source.has_value());
        if (!source || !transport.armed() ||
            (runtime.figure != nullptr && runtime.figure->animator().entering())) {
            continue;
        }
        if (const auto destination = m_transporters.landing(
                *source, actor.radius(), m_world->collision(), viewCamera(), cameraView());
            destination && transport.begin(*destination)) {
            const auto sound = LevelTransporters::soundForRealm(m_world->ref().realmId);
            if (!sound.empty()) {
                m_audio.playNamed(sound);
            }
        }
    }
}

} // namespace gdl::game

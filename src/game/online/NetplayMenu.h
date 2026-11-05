#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"

#include "game/netplay/RoomService.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/PointerTargets.h"

namespace gdl::game {
/** Local online UI. Contains no provider credentials beyond the displayed invitation and
 * never changes simulation state; the application consumes its explicit actions. */
class NetplayMenu {
public:
    enum class Page : u8 { Choose, Host, Join, Guest, Settings, Pause, Leave, Finished };
    enum class Action : u8 {
        None,
        Host,
        Join,
        LocalPlayers,
        Paste,
        Clear,
        Connect,
        Copy,
        SelectCharacters,
        Ready,
        Start,
        Capacity,
        GameSettings,
        Difficulty,
        FriendlyFire,
        Camera,
        Rule, ///< one of the mechanics the room may switch on, `Event::rule`
        Resume,
        SaveCompletedProgress,
        ReturnTitle,
        Leave,
        Back
    };
    struct View {
        Page page = Page::Choose;
        RoomSettings settings;
        std::vector<std::string> rules; ///< the mechanics a room may switch on, in bit order
        bool host = false; ///< this machine hosts the room, so it sets the game's rules
        u8 localPlayers = 1;
        usize players = 0;
        bool connected = false;
        bool available = true;
        bool selected = false;
        bool ready = false;
        bool canStart = false;
        bool canSaveCompletedProgress = false;
        std::string invitation;
        std::string status;
        bool operator==(const View&) const = default;
    };
    struct Event {
        Action action = Action::None;
        s32 direction = 0;
        u8 rule = 0; ///< the room rule's bit, for Action::Rule
    };
    void show(RenderDevice& device, const View& view);
    Event update(const MenuInput& raw);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    const View& view() const { return m_view; }
    usize selection() const { return m_selection; }
    static Rect rowArea(usize row);
    usize rows() const { return m_rows.size(); }

private:
    struct Row {
        std::string label;
        Action action = Action::None;
        bool enabled = true;
        bool adjustable = false;
        std::unique_ptr<Texture> texture;
        u8 rule = 0;
    };
    View m_view;
    std::vector<Row> m_rows;
    PointerTargets<usize> m_pointerTargets; ///< the enabled rows, each by its index
    std::unique_ptr<Texture> m_title;
    std::unique_ptr<Texture> m_notice;
    std::unique_ptr<Texture> m_code;
    Mat4 m_pointerTransform{1};
    std::optional<Vec2> m_pointer;
    usize m_selection = 0;
    bool m_open = false;
};
} // namespace gdl::game

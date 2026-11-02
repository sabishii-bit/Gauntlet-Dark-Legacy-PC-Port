#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {

/** Prepared dialogue geometry, independent of its game's artwork. */
struct DialoguePage {
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const DialoguePage& value) {
        visitor.template fields<4>(value);
    }
    std::vector<std::string> lines;
    Rect area;
    s32 textTop = 0;
    s32 promptY = 0;
};

/** Optional native modal skin. Resources are prepared at level load and borrowed
 * by the shared paging/input controller; drawing never uploads or changes state. */
class DialoguePresentation {
public:
    virtual ~DialoguePresentation() = default;
    virtual const TextPainter& text() const = 0;
    virtual f32 scale() const = 0;
    virtual s32 textWidth() const = 0;
    virtual usize linesPerPage() const = 0;
    virtual DialoguePage layout(std::string_view page, f32 scale,
                                std::string_view prompt) const = 0;
    virtual void draw(Canvas& canvas, const DialoguePage& page, f32 scale, std::string_view prompt,
                      u8 promptAlpha) const = 0;
};

} // namespace gdl::game

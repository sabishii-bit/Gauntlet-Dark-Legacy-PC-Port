#include "game/menu/NameEntry.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

namespace {

constexpr std::size_t kKeptLetters = 5;
constexpr std::array<std::string_view, 20> kRandomNames{
    "LARRY", "PELE",  "CHUCK", "TRENT", "SPENCR", "JOFFRY", "PABLO",  "JUSTIN", "MAT",    "CHIP",
    "FRED",  "SHAWN", "JAKE",  "CJ",    "ALEX",   "MARVIN", "WESLEY", "BRAND",  "GORDON", "TRAVIS"};

} // namespace

char NameEntry::nextLetter(char letter) {
    switch (letter) {
    case 'Z': return '_';
    case '_': return '0';
    case '9': return kEndMark;
    case kEndMark: return 'A';
    default: return static_cast<char>(letter + 1);
    }
}

char NameEntry::previousLetter(char letter) {
    switch (letter) {
    case 'A': return kEndMark;
    case kEndMark: return '9';
    case '0': return '_';
    case '_': return 'Z';
    default: return static_cast<char>(letter - 1);
    }
}

std::string_view NameEntry::randomName(std::uint32_t seed) {
    return kRandomNames[seed % kRandomNames.size()];
}

std::optional<char> NameEntry::typedLetter(char typed) {
    if (typed >= 'a' && typed <= 'z') {
        return static_cast<char>(typed - 'a' + 'A');
    }
    if ((typed >= 'A' && typed <= 'Z') || (typed >= '0' && typed <= '9')) {
        return typed;
    }
    if (typed == ' ' || typed == '_') {
        return '_';
    }
    return std::nullopt;
}

void NameEntry::begin(std::string_view existing) {
    m_phase = Phase::Editing;
    m_timer = 0;
    m_repeatDirection = 0;
    m_repeatCounter = 0;
    m_repeatStep = 0;
    m_name.assign(existing.substr(0, kMaxLength));
    if (m_name.size() >= kKeptLetters) {
        m_pending = m_name[kKeptLetters];
        m_name.resize(kKeptLetters);
    } else {
        m_pending = kEndMark;
    }
}

void NameEntry::cycle(std::int32_t direction) {
    m_pending = direction > 0 ? nextLetter(m_pending) : previousLetter(m_pending);
}

bool NameEntry::repeat(const MenuInput& input, std::int32_t ticks) {
    std::int32_t held = 0;
    if (input.upHeld && !input.downHeld) {
        held = 1;
    } else if (input.downHeld && !input.upHeld) {
        held = -1;
    }
    if (held == 0 || held != m_repeatDirection) {
        m_repeatDirection = held;
        m_repeatCounter = 0;
        m_repeatStep = 0;
        return false;
    }
    m_repeatCounter += ticks;
    if (m_repeatCounter < kRepeatLadder[m_repeatStep]) {
        return false;
    }
    m_repeatCounter -= kRepeatLadder[m_repeatStep];
    if (m_repeatStep + 1 < kRepeatLadder.size()) {
        ++m_repeatStep;
    }
    cycle(held);
    return true;
}

NameEntry::Event NameEntry::removeLast() {
    m_pending = m_name.back();
    m_name.pop_back();
    return Event::LetterRemoved;
}

NameEntry::Event NameEntry::finish() {
    if (m_name.empty()) {
        m_name = randomName(static_cast<std::uint32_t>(m_timer));
    }
    m_phase = Phase::Flashing;
    m_timer = kFlashTicks;
    return Event::Accepted;
}

NameEntry::Event NameEntry::update(const MenuInput& input, std::int32_t ticks) {
    if (m_phase == Phase::Flashing) {
        m_timer -= ticks;
        if (m_timer < 0) {
            m_timer = 0;
            m_phase = Phase::Finished;
        }
        return Event::None;
    }
    if (m_phase != Phase::Editing) {
        return Event::None;
    }

    if (input.erase && !m_name.empty()) {
        return removeLast();
    }
    Event event = Event::None;
    for (const char typed : input.typed) {
        const std::optional<char> letter = typedLetter(typed);
        if (letter.has_value() && m_name.size() < kMaxLength) {
            m_name.push_back(*letter);
            m_pending = kEndMark;
            event = Event::LetterAdded;
        }
    }
    if (event == Event::LetterAdded && m_name.size() >= kMaxLength) {
        return finish();
    }
    if (!input.select) {
        if (input.up || input.down) {
            cycle(input.up ? 1 : -1);
            m_repeatDirection = input.up ? 1 : -1;
            m_repeatCounter = 0;
            m_repeatStep = 0;
            event = Event::LetterChanged;
        } else if (repeat(input, ticks)) {
            event = Event::LetterChanged;
        }
    }
    if (input.left && !m_name.empty()) {
        return removeLast();
    }
    if (input.select || input.right) {
        bool finished = false;
        if (m_pending == kEndMark) {
            finished = input.select;
        } else {
            if (m_name.size() < kMaxLength) {
                m_name.push_back(m_pending);
            }
            finished = m_name.size() >= kMaxLength;
            m_pending = kEndMark;
            event = Event::LetterAdded;
        }
        if (finished) {
            return finish();
        }
    }
    m_timer += ticks;
    return event;
}

} // namespace gdl::game

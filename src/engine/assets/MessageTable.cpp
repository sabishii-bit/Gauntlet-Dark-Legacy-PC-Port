#include "engine/assets/MessageTable.h"

#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

/** A page as the rom stores it, without the carriage returns some lines carry. */
std::string cleanPage(std::string page) {
    std::erase(page, '\r');
    return page;
}

} // namespace

bool MessageTable::load(const std::filesystem::path& file) {
    m_fonts.clear();
    m_messages.clear();
    m_byName.clear();
    try {
        const std::vector<u8> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        m_fonts = root.value("fonts", std::vector<std::string>{});
        for (const nlohmann::json& entry : root.at("messages")) {
            MessageInfo message;
            message.name = entry.value("name", std::string{});
            message.font = entry.value("font", 0);
            message.scale = entry.value("scale", 1.0f);
            message.shadowScale = entry.value("shadowScale", 1.0f);
            for (const nlohmann::json& line : entry.value("lines", nlohmann::json::array())) {
                message.pages.push_back(cleanPage(line.get<std::string>()));
            }
            m_messages.push_back(std::move(message));
        }
    } catch (const std::exception& e) {
        log::warn("Message table {}: {}", file.string(), e.what());
        m_fonts.clear();
        m_messages.clear();
        return false;
    }
    for (u32 i = 0; i < m_messages.size(); ++i) {
        m_byName.try_emplace(m_messages[i].name, i);
    }
    return !m_messages.empty();
}

const MessageInfo& MessageTable::message(u32 index) const {
    GDL_VERIFY(index < m_messages.size(), "message index out of range");
    return m_messages[index];
}

std::optional<u32> MessageTable::find(std::string_view name) const {
    const auto it = m_byName.find(std::string(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string_view MessageTable::fontOf(const MessageInfo& message) const {
    if (message.font < 0 || static_cast<usize>(message.font) >= m_fonts.size()) {
        return {};
    }
    return m_fonts[static_cast<usize>(message.font)];
}

} // namespace gdl

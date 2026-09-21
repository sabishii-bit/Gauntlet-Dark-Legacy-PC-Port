#include "engine/assets/MessageTable.h"

#include <cctype>
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
    m_lists.clear();
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
        for (const nlohmann::json& entry : root.value("lists", nlohmann::json::array())) {
            MessageList list;
            list.name = entry.value("name", std::string{});
            list.messages = entry.value("messages", std::vector<s32>{});
            m_lists.push_back(std::move(list));
        }
    } catch (const std::exception& e) {
        log::warn("Message table {}: {}", file.string(), e.what());
        m_fonts.clear();
        m_messages.clear();
        m_lists.clear();
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

const MessageList* MessageTable::findList(std::string_view name) const {
    for (const MessageList& list : m_lists) {
        if (list.name == name) {
            return &list;
        }
    }
    return nullptr;
}

const MessageInfo* MessageTable::listed(const MessageList& list, usize entry) const {
    if (entry >= list.messages.size()) {
        return nullptr;
    }
    const s32 index = list.messages[entry];
    if (index < 0 || static_cast<usize>(index) >= m_messages.size()) {
        return nullptr;
    }
    return &m_messages[static_cast<usize>(index)];
}

std::string_view MessageTable::fontOf(const MessageInfo& message) const {
    if (message.font < 0 || static_cast<usize>(message.font) >= m_fonts.size()) {
        return {};
    }
    return m_fonts[static_cast<usize>(message.font)];
}

std::string MessageTable::textId(std::string_view prefix, std::string_view name, usize page) {
    std::string id{prefix};
    id += '.';
    for (const char c : name) {
        id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    id += '.';
    id += std::to_string(page);
    return id;
}

usize MessageTable::translate(const StringTable& strings, std::string_view prefix) {
    usize taken = 0;
    for (MessageInfo& message : m_messages) {
        if (message.name.empty() || !strings.has(textId(prefix, message.name, 1))) {
            continue;
        }
        message.pages.clear();
        for (usize page = 1; strings.has(textId(prefix, message.name, page)); ++page) {
            message.pages.emplace_back(strings.get(textId(prefix, message.name, page)));
        }
        ++taken;
    }
    return taken;
}

} // namespace gdl

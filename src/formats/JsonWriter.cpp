#include "formats/JsonWriter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

namespace gdl::formats {

namespace {

void appendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

} // namespace

void JsonWriter::newline() {
    m_text.push_back('\n');
    m_text.append(m_first.size() * 2, ' ');
}

void JsonWriter::beforeValue() {
    if (m_pendingKey) {
        m_pendingKey = false;
        return;
    }
    if (m_first.empty()) {
        return;
    }
    if (!m_first.back()) {
        m_text.push_back(',');
    }
    m_first.back() = false;
    newline();
}

JsonWriter& JsonWriter::beginObject() {
    beforeValue();
    m_text.push_back('{');
    m_first.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    const bool empty = m_first.back();
    m_first.pop_back();
    if (!empty) {
        newline();
    }
    m_text.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    beforeValue();
    m_text.push_back('[');
    m_first.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    const bool empty = m_first.back();
    m_first.pop_back();
    if (!empty) {
        newline();
    }
    m_text.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    beforeValue();
    appendEscaped(m_text, name);
    m_text += ": ";
    m_pendingKey = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    beforeValue();
    appendEscaped(m_text, text);
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    beforeValue();
    m_text += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number) {
    beforeValue();
    m_text += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t number) {
    beforeValue();
    m_text += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(double number) {
    beforeValue();
    if (std::isfinite(number)) {
        m_text += std::format("{}", number);
    } else {
        m_text += "null";
    }
    return *this;
}

JsonWriter& JsonWriter::value(float number) {
    beforeValue();
    if (std::isfinite(number)) {
        m_text += std::format("{}", number);
    } else {
        m_text += "null";
    }
    return *this;
}

JsonWriter& JsonWriter::numbers(std::span<const float> values) {
    beforeValue();
    m_text.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            m_text += ", ";
        }
        m_text += std::isfinite(values[i]) ? std::format("{}", values[i]) : "null";
    }
    m_text.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::numbers(std::span<const std::uint16_t> values) {
    beforeValue();
    m_text.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            m_text += ", ";
        }
        m_text += std::to_string(values[i]);
    }
    m_text.push_back(']');
    return *this;
}

std::string JsonWriter::take() {
    m_text.push_back('\n');
    return std::exchange(m_text, {});
}

} // namespace gdl::formats

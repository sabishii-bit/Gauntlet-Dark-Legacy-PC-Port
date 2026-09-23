#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::formats {

/** Minimal streaming JSON writer with indentation, enough for the unpacked asset manifests. */
class JsonWriter {
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();

    /** Starts a member inside an object; follow it with a value call. */
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(bool flag);
    JsonWriter& value(std::int64_t number);
    JsonWriter& value(std::uint64_t number);
    JsonWriter& value(int number) { return value(static_cast<std::int64_t>(number)); }
    JsonWriter& value(std::uint32_t number) { return value(static_cast<std::uint64_t>(number)); }
    JsonWriter& value(double number);
    JsonWriter& value(float number);

    /** Writes a whole list of numbers as one array on a single line. */
    JsonWriter& numbers(std::span<const float> values);
    JsonWriter& numbers(std::span<const std::uint16_t> values);

    std::string take();
    const std::string& text() const { return m_text; }

private:
    void beforeValue();
    void newline();

    std::string m_text;
    std::vector<bool> m_first; ///< per open container: no member written yet
    bool m_pendingKey = false;
};

} // namespace gdl::formats

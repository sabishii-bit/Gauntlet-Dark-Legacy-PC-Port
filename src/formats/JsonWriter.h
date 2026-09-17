#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

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
    JsonWriter& value(s64 number);
    JsonWriter& value(u64 number);
    JsonWriter& value(int number) { return value(static_cast<s64>(number)); }
    JsonWriter& value(u32 number) { return value(static_cast<u64>(number)); }
    JsonWriter& value(f64 number);
    JsonWriter& value(f32 number);

    /** Writes a whole list of numbers as one array on a single line. */
    JsonWriter& numbers(std::span<const f32> values);
    JsonWriter& numbers(std::span<const u16> values);

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

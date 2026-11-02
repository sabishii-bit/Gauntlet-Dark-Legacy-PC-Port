#pragma once

#include <array>
#include <bit>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/io/CanonicalDigestSink.h"
#include "engine/io/NativeStateFields.h"
#include "engine/math/Math.h"
#include "engine/math/Mt19937.h"

namespace gdl {

/** Read-only native-value visitor. Unsupported values (especially raw addresses)
 * fail to compile. Every aggregate opts in with its exact field count; private
 * types supply an ADL visitNativeState friend over their real members.
 * Borrowed resources are explicit, typed, stage-scoped semantic identities.
 * A missing binding invalidates the result instead of certifying partial state. */
class NativeStateDigest {
public:
    using Digest = CanonicalDigestSink::Digest;
    struct Resources {
        void* context = nullptr;
        bool (*append)(NativeStateDigest&, std::type_index, const void*, void*) = nullptr;
    };
    NativeStateDigest() = default;
    explicit NativeStateDigest(Resources resources) : m_resources(resources) {}

    void word(u32 v) { m_sink.word(v); }
    void wide(u64 v) { m_sink.wide(v); }
    void flag(bool v) { word(v ? 1U : 0U); }
    void integer(s32 v) { word(std::bit_cast<u32>(v)); }
    void real(f32 v) { word(std::bit_cast<u32>(v)); }
    void vector(const Vec3& v) { values(v.x, v.y, v.z); }
    void matrix(const Mat4& v) { value(v); }
    void string(std::string_view v) {
        wide(v.size());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
        m_sink.append({reinterpret_cast<const u8*>(v.data()), v.size()});
    }
    void bytes(std::span<const u8> v) {
        wide(v.size());
        m_sink.append(v);
    }
    bool good() const { return m_good; }
    void invalidate() { m_good = false; }
    // Legacy scalar-only visitors cannot encounter missing resources. New native
    // state callers must use checkedFinish() (or propagate good()).
    Digest finish() const { return m_sink.finish(); }
    std::optional<Digest> checkedFinish() const {
        return good() ? std::optional{finish()} : std::nullopt;
    }

    template <class T> void resource(const T* pointer) {
        flag(pointer != nullptr);
        if (pointer != nullptr &&
            (m_resources.append == nullptr ||
             !m_resources.append(*this, typeid(T), pointer, m_resources.context))) {
            invalidate();
        }
    }
    template <class T> void value(T*) = delete;
    template <class T> void value(const T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            flag(v);
        } else if constexpr (std::is_enum_v<T>) {
            value(static_cast<std::underlying_type_t<T>>(v));
        } else if constexpr (std::is_integral_v<T>) {
            static_assert(sizeof(T) <= sizeof(u64));
            if constexpr (sizeof(T) <= sizeof(u32)) {
                word(static_cast<u32>(v));
            } else {
                wide(static_cast<u64>(v));
            }
        } else if constexpr (std::is_same_v<T, f32>) {
            real(v);
        } else if constexpr (std::is_same_v<T, f64>) {
            wide(std::bit_cast<u64>(v));
        } else if constexpr (requires { visitNativeState(*this, v); }) {
            visitNativeState(*this, v);
        } else {
            static_assert(!std::is_same_v<T, T>,
                          "Native state requires an explicit complete visitor");
        }
    }
    template <class... T> void values(const T&... v) { (value(v), ...); }
    template <usize Count, class T> void fields(const T& v) {
        visitNativeFields<Count>(v, [this](const auto&... field) { values(field...); });
    }
    void value(const std::string& v) { string(v); }
    void value(std::string_view v) { string(v); }
    void value(const Mt19937& v) { fields<2>(v.checkpoint()); }
    void value(const std::mt19937& v) {
        // The standard fixes the engine's outputs, not its stream text: one library writes
        // its cursor, another its circular history. The next state_size outputs determine
        // every later one, so they stand for the state, drawn from a copy.
        auto copy = v;
        for (usize i = 0; i < std::mt19937::state_size; ++i) {
            word(static_cast<u32>(copy()));
        }
    }
    template <class T, T A, T C, T M>
    void value(const std::linear_congruential_engine<T, A, C, M>& v) {
        // For this family the next output uniquely identifies its retained state.
        // Read from a copy; never advance the gameplay generator to hash it. The width is
        // the modulus's, not result_type's: std::minstd_rand's is four bytes on one library
        // and eight on another.
        static_assert(M != 0 && std::gcd(A, M) == 1);
        auto copy = v;
        if constexpr (M - 1 <= std::numeric_limits<u32>::max()) {
            word(static_cast<u32>(copy()));
        } else {
            wide(static_cast<u64>(copy()));
        }
    }
    void value(const Color& v) { fields<4>(v); }
    void value(const Rect& v) { fields<4>(v); }
    template <class T> void value(const std::optional<T>& v) {
        flag(v.has_value());
        if (v) {
            value(*v);
        }
    }
    template <class A, class B> void value(const std::pair<A, B>& v) { values(v.first, v.second); }
    template <class... T> void value(const std::tuple<T...>& v) {
        std::apply([this](const auto&... field) { values(field...); }, v);
    }
    template <class T, usize N> void value(const std::array<T, N>& v) { sequence(v); }
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays): takes the caller's array as is
    template <class T, usize N> void value(const T (&v)[N]) { sequence(v); }
    template <class T, usize N> void value(std::span<T, N> v) { sequence(v); }
    template <class T, class A> void value(const std::vector<T, A>& v) { sequence(v); }
    template <class A> void value(const std::vector<bool, A>& v) {
        wide(v.size());
        for (const bool item : v) {
            flag(item);
        }
    }
    template <class T, class A> void value(const std::deque<T, A>& v) { sequence(v); }
    template <class K, class V, class C, class A> void value(const std::map<K, V, C, A>& v) {
        sequence(v);
    }
    template <class K, class C, class A> void value(const std::set<K, C, A>& v) { sequence(v); }
    template <class K, class V, class H, class E, class A>
    void value(const std::unordered_map<K, V, H, E, A>& v) {
        unordered(v);
    }
    template <class K, class H, class E, class A>
    void value(const std::unordered_set<K, H, E, A>& v) {
        unordered(v);
    }
    template <glm::length_t L, class T, glm::qualifier Q> void value(const glm::vec<L, T, Q>& v) {
        for (glm::length_t i = 0; i < L; ++i) {
            value(v[i]);
        }
    }
    template <glm::length_t C, glm::length_t R, class T, glm::qualifier Q>
    void value(const glm::mat<C, R, T, Q>& v) {
        for (glm::length_t i = 0; i < C; ++i) {
            value(v[i]);
        }
    }
    template <class T, glm::qualifier Q> void value(const glm::qua<T, Q>& v) {
        values(v.w, v.x, v.y, v.z);
    }

private:
    template <class Range> void sequence(const Range& v) {
        wide(std::size(v));
        for (const auto& item : v) {
            value(item);
        }
    }
    template <class Range> void unordered(const Range& v) {
        // Hash each complete element, then sort fixed-width digests. No pointer,
        // insertion-order or bucket-layout dependence; a failed child propagates.
        std::vector<Digest> ordered;
        ordered.reserve(v.size());
        for (const auto& item : v) {
            NativeStateDigest child(m_resources);
            child.value(item);
            m_good = m_good && child.good();
            ordered.push_back(child.finish());
        }
        std::ranges::sort(ordered);
        wide(ordered.size());
        for (const auto& item : ordered) {
            m_sink.append(item);
        }
    }
    CanonicalDigestSink m_sink;
    Resources m_resources;
    bool m_good = true;
};

} // namespace gdl

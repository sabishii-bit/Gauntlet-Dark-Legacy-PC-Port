#include <array>

#include "engine/net/SessionTransport.h"
#include "engine/net/iroh/Bridge.h"

namespace gdl {
namespace {
class IrohTransport final : public SessionTransport {
public:
    explicit IrohTransport(GdlIroh* handle) : m_handle(handle) {}
    ~IrohTransport() override { gdl_iroh_destroy(m_handle); }
    IrohTransport(const IrohTransport&) = delete;
    IrohTransport& operator=(const IrohTransport&) = delete;
    IrohTransport(IrohTransport&&) = delete;
    IrohTransport& operator=(IrohTransport&&) = delete;
    Phase phase() const override { return static_cast<Phase>(gdl_iroh_state(m_handle)); }
    std::string invitation() const override { return text(true); }
    std::string error() const override { return text(false); }
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        return static_cast<SendResult>(gdl_iroh_send(m_handle, connection, bytes.data(),
                                                     bytes.size(),
                                                     delivery == Delivery::Reliable ? 1 : 0));
    }
    std::vector<Event> poll() override {
        std::vector<Event> result;
        GdlIrohEvent event{};
        while (result.size() < kReceiveBudget && gdl_iroh_poll(m_handle, &event) != 0) {
            const std::span<const u8> bytes(event.bytes);
            if (event.kind > 2 || event.size > bytes.size()) {
                close(event.connection);
                continue;
            }
            result.push_back({static_cast<EventType>(event.kind),
                              event.connection,
                              {bytes.begin(), bytes.begin() + event.size},
                              {}});
        }
        return result;
    }
    std::optional<Statistics> statistics(Connection connection) const override {
        GdlIrohStats stats{};
        if (gdl_iroh_stats(m_handle, connection, &stats) == 0) {
            return std::nullopt;
        }
        return Statistics{stats.pingMs, stats.pendingBytes, 0, stats.relayed != 0};
    }
    void close(Connection connection) override { gdl_iroh_close(m_handle, connection); }

private:
    std::string text(bool invite) const {
        std::array<u8, 2048> buffer{};
        const auto count = gdl_iroh_text(m_handle, invite ? 1 : 0, buffer.data(), buffer.size());
        return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count)};
    }
    GdlIroh* m_handle;
};
} // namespace
std::unique_ptr<SessionTransport> openSessionTransport(const SessionTransport::Options& options,
                                                       std::string& error) {
    const std::vector<u8> invitation(options.invitation.begin(), options.invitation.end());
    auto* handle = gdl_iroh_create(invitation.data(), invitation.size(), options.localOnly ? 1 : 0);
    if (!handle) {
        error = "Invalid invitation or network worker unavailable";
        return nullptr;
    }
    error.clear();
    return std::make_unique<IrohTransport>(handle);
}
} // namespace gdl

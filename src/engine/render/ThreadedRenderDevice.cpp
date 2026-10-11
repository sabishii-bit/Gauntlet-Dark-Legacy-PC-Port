#include "engine/render/ThreadedRenderDevice.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "engine/render/AmbientOcclusion.h"
#include "engine/render/DepthOfField.h"
#include "engine/render/HeatDistortion.h"
#include "engine/render/ImmediateBatch.h"

namespace gdl {
struct ThreadedRenderDevice::Impl {
    GDL_NON_COPYABLE_NON_MOVABLE(Impl);
    struct Resource {
        u64 id = 0;
        TextureDesc desc;
        const Impl* owner = nullptr;
    };
    using Lease = std::shared_ptr<Resource>;
    struct Proxy final : Texture {
        explicit Proxy(Lease value) : lease(std::move(value)) {}
        u32 width() const override { return lease->desc.width; }
        u32 height() const override { return lease->desc.height; }
        Lease lease;
    };
    struct Resident {
        std::weak_ptr<Resource> lease;
        std::unique_ptr<Texture> texture;
    };
    using Command = std::function<void(Impl&)>;
    struct Packet {
        Packet() = default;
        ~Packet() = default;
        GDL_NON_COPYABLE_NON_MOVABLE(Packet);
        std::vector<Command> commands;
        std::map<u64, Command> uploads;
        Extent2D extent;
    };

    explicit Impl(std::unique_ptr<RenderDevice> device, Extent2D size)
        : backend(std::move(device)), extent(size),
          white(std::make_shared<Resource>(Resource{0, TextureDesc{1, 1}, this})) {
        if (!backend) {
            throw std::invalid_argument("Threaded renderer requires a backend");
        }
        backend->setFramebufferSize(extent);
        samples = backend->presentationSampleCount();
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        frame.clear();
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        worker.join();
    }
    void check() {
        const std::lock_guard lock(mutex);
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    void enqueue(Command command) {
        {
            const std::lock_guard lock(mutex);
            if (failure) {
                std::rethrow_exception(failure);
            }
            jobs.push_back(std::move(command));
        }
        ready.notify_one();
    }
    void record(Command command) {
        if (!recording) {
            throw std::logic_error("Draw command outside a recorded frame");
        }
        frame.push_back(std::move(command));
    }
    void configure(Command command) {
        if (recording) {
            record(std::move(command));
        } else {
            enqueue(std::move(command));
        }
    }
    Lease retain(const Texture* texture) const {
        if (texture == nullptr) {
            return {};
        }
        const auto* proxy = dynamic_cast<const Proxy*>(texture);
        if (proxy == nullptr || proxy->lease->owner != this) {
            throw std::invalid_argument("Texture belongs to a different renderer");
        }
        return proxy->lease;
    }
    const Texture* resolve(const Lease& lease) const {
        if (!lease) {
            return nullptr;
        }
        return lease->id == 0 ? &backend->whiteTexture() : resident.at(lease->id).texture.get();
    }
    void collect() {
        std::vector<u64> retired;
        for (const auto& [id, resource] : resident) {
            if (resource.lease.expired()) {
                retired.push_back(id);
            }
        }
        if (!retired.empty()) {
            // CPU packet completion is not a GPU fence. Submitted frames can still
            // reference these descriptors; only the worker waits for their retirement.
            backend->waitIdle();
        }
        // A lease can expire concurrently. Retire only those covered by this fence.
        for (const auto id : retired) {
            resident.erase(id);
        }
    }
    void run() {
        try {
            for (;;) {
                Command command;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return stopping || !jobs.empty(); });
                    if (jobs.empty()) {
                        break;
                    }
                    command = std::move(jobs.front());
                    jobs.pop_front();
                }
                command(*this);
            }
            backend->waitIdle();
        } catch (...) {
            const std::lock_guard lock(mutex);
            failure = std::current_exception();
            jobs.clear(); // Abandoned barrier promises wake their waiting callers.
        }
        // Vulkan texture destruction and device teardown belong to the worker too.
        try {
            backend->waitIdle();
        } catch (...) {
            // Keep the original worker exception; device loss can also fail this wait.
            const std::lock_guard lock(mutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
        resident.clear();
        backend.reset();
    }

    std::unique_ptr<RenderDevice> backend;
    std::map<u64, Resident> resident;      // worker only
    std::map<u64, Command> pendingUploads; // survive a skipped swapchain acquisition
    Extent2D extent;                       // application thread only
    Proxy white;
    u64 nextTexture = 1;
    bool recording = false;
    bool hasContent = false;
    std::vector<Command> frame;
    std::map<u64, Command> uploads;
    std::atomic<bool> busy = false;
    std::atomic<u32> samples = 1;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Command> jobs;
    bool stopping = false;
    std::exception_ptr failure;
    std::thread worker;
};

ThreadedRenderDevice::ThreadedRenderDevice(std::unique_ptr<RenderDevice> backend, Extent2D extent)
    : m_impl(std::make_unique<Impl>(std::move(backend), extent)) {}
ThreadedRenderDevice::~ThreadedRenderDevice() = default;

bool ThreadedRenderDevice::beginFrame() {
    auto& impl = *m_impl;
    impl.check();
    if (impl.recording) {
        throw std::logic_error("Recorded frame already open");
    }
    if (impl.extent.isZero() || impl.busy.exchange(true)) {
        return false;
    }
    impl.recording = true;
    impl.hasContent = false;
    impl.frame.clear();
    impl.uploads.clear();
    return true;
}
void ThreadedRenderDevice::endFrame() {
    auto& impl = *m_impl;
    if (!impl.recording) {
        throw std::logic_error("No recorded frame to submit");
    }
    impl.recording = false;
    auto packet = std::make_shared<Impl::Packet>();
    packet->commands.swap(impl.frame);
    packet->uploads.swap(impl.uploads);
    packet->extent = impl.extent;
    impl.enqueue([packet](Impl& worker) {
        for (auto& [id, command] : packet->uploads) {
            worker.pendingUploads.insert_or_assign(id, std::move(command));
        }
        worker.backend->setFramebufferSize(packet->extent);
        if (worker.backend->beginFrame()) {
            for (auto& [id, upload] : worker.pendingUploads) {
                upload(worker);
            }
            worker.pendingUploads.clear();
            for (auto& command : packet->commands) {
                command(worker);
            }
            worker.backend->endFrame();
        }
        worker.samples = worker.backend->presentationSampleCount();
        packet->commands.clear();
        worker.collect();
        worker.busy = false;
    });
}
void ThreadedRenderDevice::setClearColor(const Vec4& rgba) {
    m_impl->configure([rgba](Impl& worker) { worker.backend->setClearColor(rgba); });
}
Extent2D ThreadedRenderDevice::framebufferExtent() const {
    return m_impl->extent;
}
void ThreadedRenderDevice::setFramebufferSize(Extent2D extent) {
    m_impl->extent = extent;
}
void ThreadedRenderDevice::setPresentation(bool vsync, u32 sampleCount) {
    m_impl->configure([vsync, sampleCount](Impl& worker) {
        worker.backend->setPresentation(vsync, sampleCount);
    });
}
u32 ThreadedRenderDevice::presentationSampleCount() const {
    return m_impl->samples.load();
}
void ThreadedRenderDevice::setTextureFiltering(u32 filtering) {
    m_impl->configure(
        [filtering](Impl& worker) { worker.backend->setTextureFiltering(filtering); });
}
void ThreadedRenderDevice::setSmoothSprites(bool enabled) {
    m_impl->configure([enabled](Impl& worker) { worker.backend->setSmoothSprites(enabled); });
}
std::unique_ptr<Texture> ThreadedRenderDevice::createTexture(const TextureDesc& desc,
                                                             std::span<const u8> pixels) {
    auto lease =
        std::make_shared<Impl::Resource>(Impl::Resource{m_impl->nextTexture++, desc, m_impl.get()});
    m_impl->enqueue([lease, data = std::vector<u8>(pixels.begin(), pixels.end())](Impl& worker) {
        auto texture = worker.backend->createTexture(lease->desc, data);
        if (!texture) {
            throw std::runtime_error("Unable to upload recorded texture");
        }
        worker.resident.emplace(lease->id, Impl::Resident{lease, std::move(texture)});
    });
    return std::make_unique<Impl::Proxy>(std::move(lease));
}
void ThreadedRenderDevice::updateTexture(Texture& texture, std::span<const u8> pixels) {
    if (!m_impl->recording || m_impl->hasContent) {
        throw std::logic_error("Texture update follows a recorded draw");
    }
    const auto lease = m_impl->retain(&texture);
    if (lease->id == 0) {
        throw std::invalid_argument("Cannot update the renderer's white texture");
    }
    m_impl->uploads.insert_or_assign(
        lease->id, [lease, data = std::vector<u8>(pixels.begin(), pixels.end())](Impl& worker) {
            worker.backend->updateTexture(*worker.resident.at(lease->id).texture, data);
        });
}
const Texture& ThreadedRenderDevice::whiteTexture() const {
    return m_impl->white;
}
void ThreadedRenderDevice::draw(const ImmediateBatch& batch, const Texture& texture,
                                const Mat4& transform, const DrawState& state) {
    if (batch.empty()) {
        return;
    }
    const auto base = m_impl->retain(&texture);
    const auto lightmap = m_impl->retain(state.lightmap);
    const auto mask = m_impl->retain(state.maskedTexture);
    const auto next = m_impl->retain(state.nextTexture);
    m_impl->record([batch = ImmediateBatch{batch}, base, lightmap, mask, next, transform,
                    state = DrawState{state}](Impl& worker) mutable noexcept(false) {
        state.lightmap = worker.resolve(lightmap);
        state.maskedTexture = worker.resolve(mask);
        state.nextTexture = worker.resolve(next);
        worker.backend->draw(batch, *worker.resolve(base), transform, state);
    });
    m_impl->hasContent = true;
}
void ThreadedRenderDevice::waitIdle() {
    if (m_impl->recording) {
        throw std::logic_error("Cannot wait with an unsubmitted frame");
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    m_impl->enqueue([done](Impl& worker) {
        try {
            worker.backend->waitIdle();
            worker.collect();
            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
            throw;
        }
    });
    // Only the queued command owns the promise, including the failure path.
    done.reset();
    try {
        finished.get();
    } catch (...) {
        m_impl->check();
        throw;
    }
    m_impl->check();
}
bool ThreadedRenderDevice::applyDepthOfField(const DepthOfField& settings) {
    m_impl->record([settings](Impl& worker) { worker.backend->applyDepthOfField(settings); });
    m_impl->hasContent = true;
    return true;
}
bool ThreadedRenderDevice::applyBloom() {
    m_impl->record([](Impl& worker) { worker.backend->applyBloom(); });
    m_impl->hasContent = true;
    return true;
}
void ThreadedRenderDevice::addHeatSource(const HeatSource& source) {
    m_impl->record([source](Impl& worker) { worker.backend->addHeatSource(source); });
}
bool ThreadedRenderDevice::applyAmbientOcclusion(const AmbientOcclusion& settings) {
    m_impl->record([settings](Impl& worker) { worker.backend->applyAmbientOcclusion(settings); });
    m_impl->hasContent = true;
    return true;
}
} // namespace gdl

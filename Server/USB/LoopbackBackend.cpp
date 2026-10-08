#include "USB/LoopbackBackend.hpp"

#include <cstring>

#include "Logging/Logger.hpp"

namespace pf::server {

SpecialControl classify_control(const std::array<std::uint8_t, 8>& s) noexcept {
    if (s[0] == 0x00 && s[1] == 0x09) return SpecialControl::SetConfiguration;
    if (s[0] == 0x01 && s[1] == 0x0B) return SpecialControl::SetInterface;
    if (s[0] == 0x02 && s[1] == 0x01 && s[2] == 0 && s[3] == 0) return SpecialControl::ClearHalt;  // ENDPOINT_HALT
    return SpecialControl::None;
}

// ------------------------------------------------------------------ specs

const std::vector<std::string>& LoopbackBackend::phone_modes() {
    static const std::vector<std::string> modes{"android-adb",   "bootloader",   "recovery",        "qualcomm-edl",
                                                "mtk-preloader", "mtk-brom",     "samsung-download", "android-mtp"};
    return modes;
}

DeviceSpec LoopbackBackend::phone_mode_spec(const std::string& mode, const std::string& serial) {
    DeviceSpec s;
    s.manufacturer = "ParsFlash Test";
    s.serial = serial;
    InterfaceSpec main;
    main.number = 0;
    main.endpoints = {{0x81, TransferType::Bulk, 512},
                      {0x01, TransferType::Bulk, 512},
                      {0x84, TransferType::Bulk, 512},
                      {0x03, TransferType::Bulk, 512},
                      {0x82, TransferType::Interrupt, 64, 4},
                      {0x85, TransferType::Isochronous, 1024, 1}};
    auto set = [&](std::uint16_t vid, std::uint16_t pid, std::uint8_t c, std::uint8_t sc, std::uint8_t p,
                   std::string product) {
        s.vid = vid;
        s.pid = pid;
        main.cls = c;
        main.subclass = sc;
        main.protocol = p;
        s.product = std::move(product);
    };
    if (mode == "android-adb") set(0x18d1, 0x4ee7, 0xff, 0x42, 0x01, "Test Phone (ADB)");
    else if (mode == "bootloader") set(0x18d1, 0x4ee0, 0xff, 0x42, 0x03, "Test Phone (Fastboot)");
    else if (mode == "recovery") set(0x18d1, 0xd001, 0xff, 0x42, 0x01, "Test Phone (Recovery)");
    else if (mode == "qualcomm-edl") { set(0x05c6, 0x9008, 0xff, 0xff, 0xff, "QUSB__BULK"); s.serial.clear(); }
    else if (mode == "mtk-preloader") set(0x0e8d, 0x2000, 0x02, 0x02, 0x01, "MT65xx Preloader");
    else if (mode == "mtk-brom") { set(0x0e8d, 0x0003, 0x02, 0x02, 0x01, "MT65xx BROM"); s.serial.clear(); }
    else if (mode == "samsung-download") set(0x04e8, 0x685d, 0x02, 0x02, 0x01, "SAMSUNG USB");
    else set(0x18d1, 0x4ee1, 0x06, 0x01, 0x01, "Test Phone (MTP)");
    s.interfaces.push_back(main);
    return s;
}

// ------------------------------------------------------------------ device model

struct LoopbackBackend::VirtualDevice {
    DeviceSpec spec;
    DescriptorSet desc;
    DeviceIdentity identity;
    std::mutex m;
    bool present = true;
    bool opened = false;
    std::deque<std::uint8_t> fifo;
    Bytes stored;
    std::uint32_t counter = 0;
    std::uint8_t configuration = 1;
    struct PendingIn {
        TransferRequest req;
        TransferCallback done;
        std::shared_ptr<asio::steady_timer> timer;
    };
    std::deque<PendingIn> pending_in;  // bulk IN 0x81 waiting for FIFO data
};

namespace {

void finish(TransferRequest& req, TransferCallback& done, UsbStatus st, std::uint32_t actual) {
    TransferResult r;
    r.seq = req.seq;
    r.status = st;
    r.actual_length = actual;
    r.data_offset = req.data_offset;
    if (req.type == TransferType::Isochronous) {
        for (const auto& p : req.iso) r.iso.push_back({st == UsbStatus::Success ? p.length : 0, 0});
    }
    r.buffer = std::move(req.buffer);
    done(std::move(r));
}

}  // namespace

class LoopbackDevice final : public IUsbDevice {
public:
    LoopbackDevice(LoopbackBackend& be, std::shared_ptr<LoopbackBackend::VirtualDevice> dev)
        : be_(be), dev_(std::move(dev)) {}
    ~LoopbackDevice() override { close(); }

    void submit(TransferRequest&& req, TransferCallback done) override {
        const std::uint32_t lat = be_.latency_us_.load();
        // The task must not reference `this`: the device handle may be closed and
        // destroyed before a delayed transfer runs. The backend outlives all tasks.
        auto task = [&be = be_, dev = dev_, req = std::make_shared<TransferRequest>(std::move(req)),
                     done = std::make_shared<TransferCallback>(std::move(done))]() mutable {
            execute(be, dev, std::move(*req), std::move(*done));
        };
        if (lat == 0) {
            asio::post(be_.rt_->context(), std::move(task));
        } else {
            auto t = std::make_shared<asio::steady_timer>(be_.rt_->context(), std::chrono::microseconds(lat));
            t->async_wait([t, task = std::move(task)](std::error_code) mutable { task(); });
        }
    }

    void cancel(std::uint64_t seq) override {
        std::unique_lock lk(dev_->m);
        for (auto it = dev_->pending_in.begin(); it != dev_->pending_in.end(); ++it) {
            if (it->req.seq != seq) continue;
            auto p = std::move(*it);
            dev_->pending_in.erase(it);
            lk.unlock();
            if (p.timer) p.timer->cancel();
            finish(p.req, p.done, UsbStatus::Cancelled, 0);
            return;
        }
    }

    void cancel_all() override {
        std::deque<LoopbackBackend::VirtualDevice::PendingIn> victims;
        {
            std::lock_guard lk(dev_->m);
            victims.swap(dev_->pending_in);
        }
        for (auto& p : victims) {
            if (p.timer) p.timer->cancel();
            finish(p.req, p.done, UsbStatus::Cancelled, 0);
        }
    }

    Status reset_port() override {
        std::lock_guard lk(dev_->m);
        if (!dev_->present) return {ErrorCode::DeviceGone, "device removed"};
        dev_->fifo.clear();
        return {};
    }
    Status set_configuration(std::uint8_t v) override {
        std::lock_guard lk(dev_->m);
        if (v > 1) return {ErrorCode::InvalidArgument, "no such configuration"};
        dev_->configuration = v;
        return {};
    }
    Status set_interface(std::uint8_t iface, std::uint8_t alt) override {
        return iface == 0 && alt == 0 ? Status{} : Status{ErrorCode::InvalidArgument, "no such alt setting"};
    }
    Status clear_halt(std::uint8_t) override { return {}; }

    void close() override {
        if (closed_.exchange(true)) return;
        cancel_all();
        std::lock_guard lk(dev_->m);
        dev_->opened = false;
        be_.open_count_.fetch_sub(1);
    }
    const DescriptorSet& descriptors() const override { return dev_->desc; }
    UsbSpeed speed() const override { return dev_->identity.speed; }

private:
    static void execute(LoopbackBackend& be, const std::shared_ptr<LoopbackBackend::VirtualDevice>& dev,
                        TransferRequest&& req, TransferCallback&& done) {
        std::unique_lock lk(dev->m);
        if (!dev->present) {
            lk.unlock();
            return finish(req, done, UsbStatus::DeviceGone, 0);
        }
        std::uint8_t* data = req.data();
        switch (req.type) {
            case TransferType::Control: {
                std::uint32_t actual = 0;
                std::string switch_to;
                UsbStatus st = control(*dev, req, data, actual, switch_to);
                lk.unlock();
                finish(req, done, st, actual);
                if (!switch_to.empty()) be.switch_mode(dev->identity.port_path, switch_to, 30);
                return;
            }
            case TransferType::Bulk:
                if (req.endpoint == 0x01) {
                    if (dev->fifo.size() + req.length > (64u << 20)) {
                        lk.unlock();
                        return finish(req, done, UsbStatus::Busy, 0);
                    }
                    dev->fifo.insert(dev->fifo.end(), data, data + req.length);
                    auto ready = drain_pending(*dev);
                    lk.unlock();
                    finish(req, done, UsbStatus::Success, req.length);
                    for (auto& [p, n] : ready) finish(p.req, p.done, UsbStatus::Success, n);
                    return;
                }
                if (req.endpoint == 0x03) {
                    lk.unlock();
                    return finish(req, done, UsbStatus::Success, req.length);
                }
                if (req.endpoint == 0x84) {
                    lk.unlock();
                    std::memset(data, 0, req.length);
                    return finish(req, done, UsbStatus::Success, req.length);
                }
                if (req.endpoint == 0x81) {
                    if (!dev->fifo.empty()) {
                        const std::uint32_t n = take_fifo(*dev, data, req.length);
                        lk.unlock();
                        return finish(req, done, UsbStatus::Success, n);
                    }
                    LoopbackBackend::VirtualDevice::PendingIn p{std::move(req), std::move(done), nullptr};
                    if (p.req.timeout_ms) {
                        p.timer = std::make_shared<asio::steady_timer>(be.rt_->context(),
                                                                       std::chrono::milliseconds(p.req.timeout_ms));
                        const std::uint64_t seq = p.req.seq;
                        std::weak_ptr<LoopbackBackend::VirtualDevice> wd = dev;
                        p.timer->async_wait([wd, seq](std::error_code ec) {
                            if (ec) return;
                            auto d = wd.lock();
                            if (!d) return;
                            std::unique_lock l2(d->m);
                            for (auto it = d->pending_in.begin(); it != d->pending_in.end(); ++it) {
                                if (it->req.seq != seq) continue;
                                auto victim = std::move(*it);
                                d->pending_in.erase(it);
                                l2.unlock();
                                finish(victim.req, victim.done, UsbStatus::Timeout, 0);
                                return;
                            }
                        });
                    }
                    dev->pending_in.push_back(std::move(p));
                    return;
                }
                break;
            case TransferType::Interrupt:
                if (req.endpoint == 0x82) {
                    const std::uint32_t n = std::min<std::uint32_t>(8, req.length);
                    std::uint64_t v = ++dev->counter;
                    lk.unlock();
                    std::memcpy(data, &v, n);
                    return finish(req, done, UsbStatus::Success, n);
                }
                break;
            case TransferType::Isochronous:
                if (req.endpoint == 0x85) {
                    lk.unlock();
                    std::uint32_t total = 0;
                    for (std::size_t i = 0; i < req.iso.size(); ++i) {
                        std::memset(data + req.iso[i].offset, static_cast<int>(i & 0xFF), req.iso[i].length);
                        total += req.iso[i].length;
                    }
                    return finish(req, done, UsbStatus::Success, total);
                }
                break;
        }
        lk.unlock();
        finish(req, done, UsbStatus::Stall, 0);
    }

    static std::uint32_t take_fifo(LoopbackBackend::VirtualDevice& dev, std::uint8_t* out, std::uint32_t max) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(max, dev.fifo.size()));
        std::copy_n(dev.fifo.begin(), n, out);
        dev.fifo.erase(dev.fifo.begin(), dev.fifo.begin() + n);
        return n;
    }

    // Satisfies pending IN transfers from the FIFO; returns them for completion
    // outside the lock.
    static std::vector<std::pair<LoopbackBackend::VirtualDevice::PendingIn, std::uint32_t>> drain_pending(
        LoopbackBackend::VirtualDevice& dev) {
        std::vector<std::pair<LoopbackBackend::VirtualDevice::PendingIn, std::uint32_t>> out;
        while (!dev.pending_in.empty() && !dev.fifo.empty()) {
            auto p = std::move(dev.pending_in.front());
            dev.pending_in.pop_front();
            if (p.timer) p.timer->cancel();
            const std::uint32_t n = take_fifo(dev, p.req.data(), p.req.length);
            out.emplace_back(std::move(p), n);
        }
        return out;
    }

    static UsbStatus control(LoopbackBackend::VirtualDevice& dev, const TransferRequest& req, std::uint8_t* data,
                             std::uint32_t& actual, std::string& switch_to) {
        const auto& s = req.setup;
        const std::uint16_t w_value = static_cast<std::uint16_t>(s[2] | (s[3] << 8));
        const std::uint16_t w_length = static_cast<std::uint16_t>(s[6] | (s[7] << 8));
        const std::uint32_t max = std::min<std::uint32_t>(w_length, req.length);
        auto reply = [&](const std::uint8_t* src, std::size_t n) {
            actual = static_cast<std::uint32_t>(std::min<std::size_t>(n, max));
            std::memcpy(data, src, actual);
            return UsbStatus::Success;
        };
        if (s[0] == 0x80 && s[1] == 0x06) {  // GET_DESCRIPTOR
            const std::uint8_t type = static_cast<std::uint8_t>(w_value >> 8), index = w_value & 0xFF;
            if (type == usb_desc::kDevice) return reply(dev.desc.device.data(), dev.desc.device.size());
            if (type == usb_desc::kConfiguration && index < dev.desc.configs.size())
                return reply(dev.desc.configs[index].data(), dev.desc.configs[index].size());
            if (type == usb_desc::kString) {
                for (const auto& st : dev.desc.strings)
                    if (st.index == index) return reply(st.raw.data(), st.raw.size());
            }
            if (type == usb_desc::kBos && !dev.desc.bos.empty()) return reply(dev.desc.bos.data(), dev.desc.bos.size());
            return UsbStatus::Stall;
        }
        if (s[0] == 0x80 && s[1] == 0x00) {  // GET_STATUS
            const std::uint8_t st[2] = {0, 0};
            return reply(st, 2);
        }
        if (s[0] == 0x80 && s[1] == 0x08) return reply(&dev.configuration, 1);
        if (s[0] == 0x00 && s[1] == 0x09) {
            dev.configuration = static_cast<std::uint8_t>(w_value);
            return UsbStatus::Success;
        }
        if (s[0] == 0xC0 && s[1] == 0x01) {
            std::uint32_t v = ++dev.counter;
            return reply(reinterpret_cast<std::uint8_t*>(&v), 4);
        }
        if (s[0] == 0x40 && s[1] == 0x02) {
            dev.stored.assign(data, data + max);
            actual = max;
            return UsbStatus::Success;
        }
        if (s[0] == 0xC0 && s[1] == 0x03) return reply(dev.stored.data(), dev.stored.size());
        if (s[0] == 0x40 && s[1] == 0xF0) {
            const auto& modes = LoopbackBackend::phone_modes();
            if (w_value >= modes.size()) return UsbStatus::Stall;
            switch_to = modes[w_value];
            return UsbStatus::Success;
        }
        return UsbStatus::Stall;
    }

    LoopbackBackend& be_;
    std::shared_ptr<LoopbackBackend::VirtualDevice> dev_;
    std::atomic<bool> closed_{false};
};

// ------------------------------------------------------------------ backend

LoopbackBackend::LoopbackBackend(std::uint32_t initial_devices) {
    for (std::uint32_t i = 0; i < initial_devices; ++i) {
        char serial[32];
        std::snprintf(serial, sizeof(serial), "PFTEST%04u", i + 1);
        add_device(phone_mode_spec("android-adb", serial), "1-" + std::to_string(i + 1));
    }
}

LoopbackBackend::~LoopbackBackend() { stop(); }

Status LoopbackBackend::start() {
    if (!rt_) rt_ = std::make_unique<net::IoRuntime>(2, "pf-loopback");
    log::warn("USB", "TEST backend active: devices are emulated, not real hardware");
    return {};
}

void LoopbackBackend::stop() {
    if (rt_) {
        rt_->stop();
        rt_.reset();
    }
}

Result<std::vector<EnumeratedDevice>> LoopbackBackend::enumerate() {
    std::vector<EnumeratedDevice> out;
    std::lock_guard lk(m_);
    for (const auto& [port, d] : devices_) {
        EnumeratedDevice e;
        e.identity = d->identity;
        e.descriptors = d->desc;
        e.manufacturer = d->spec.manufacturer;
        e.product = d->spec.product;
        e.driver = "parsflash-test";
        out.push_back(std::move(e));
    }
    return out;
}

Result<std::unique_ptr<IUsbDevice>> LoopbackBackend::open(const DeviceIdentity& identity) {
    if (fail_open_) return Error{ErrorCode::DeviceAccess, "simulated capture failure"};
    if (!rt_) return Error{ErrorCode::InvalidState, "backend not started"};
    std::shared_ptr<VirtualDevice> d;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(identity.port_path);
        if (it == devices_.end() || it->second->identity.pid != identity.pid ||
            it->second->identity.vid != identity.vid)
            return Error{ErrorCode::DeviceNotFound, "no device at " + identity.port_path};
        d = it->second;
    }
    {
        std::lock_guard lk(d->m);
        if (d->opened) return Error{ErrorCode::DeviceBusy, "already captured"};
        d->opened = true;
    }
    open_count_.fetch_add(1);
    return std::unique_ptr<IUsbDevice>(std::make_unique<LoopbackDevice>(*this, d));
}

void LoopbackBackend::set_change_callback(std::function<void()> cb) {
    std::lock_guard lk(m_);
    on_change_ = std::move(cb);
}

void LoopbackBackend::notify_changed() {
    std::function<void()> cb;
    {
        std::lock_guard lk(m_);
        cb = on_change_;
    }
    if (cb) cb();
}

void LoopbackBackend::add_device(const DeviceSpec& spec, const std::string& port_path) {
    auto d = std::make_shared<VirtualDevice>();
    d->spec = spec;
    d->desc = build_descriptor_set(spec);
    {
        std::lock_guard lk(m_);
        d->identity.vid = spec.vid;
        d->identity.pid = spec.pid;
        d->identity.serial = spec.serial;
        d->identity.port_path = port_path;
        d->identity.bus = 1;
        d->identity.address = next_address_++;
        if (next_address_ > 127) next_address_ = 1;
        d->identity.port = static_cast<std::uint8_t>(std::atoi(port_path.substr(port_path.rfind('-') + 1).c_str()));
        d->identity.parent_id = "test-root-hub-1";
        d->identity.instance_id = "TEST\\" + port_path;
        d->identity.speed = spec.speed;
        if (!spec.serial.empty()) unit_serial_[port_path] = spec.serial;
        auto old = devices_.find(port_path);
        if (old != devices_.end()) {
            std::lock_guard dl(old->second->m);
            old->second->present = false;
        }
        devices_[port_path] = d;
    }
    notify_changed();
}

void LoopbackBackend::remove_device(const std::string& port_path) {
    std::shared_ptr<VirtualDevice> d;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(port_path);
        if (it == devices_.end()) return;
        d = it->second;
        devices_.erase(it);
    }
    std::deque<VirtualDevice::PendingIn> victims;
    {
        std::lock_guard lk(d->m);
        d->present = false;
        victims.swap(d->pending_in);
    }
    for (auto& p : victims) {
        if (p.timer) p.timer->cancel();
        finish(p.req, p.done, UsbStatus::DeviceGone, 0);
    }
    notify_changed();
}

void LoopbackBackend::switch_mode(const std::string& port_path, const std::string& mode, std::uint32_t delay_ms) {
    std::string serial;
    {
        std::lock_guard lk(m_);
        if (!devices_.count(port_path)) return;
        // The physical unit keeps its serial even across serial-less modes (EDL/BROM).
        serial = unit_serial_[port_path];
    }
    remove_device(port_path);
    auto spec = phone_mode_spec(mode, serial);
    if (!rt_ || delay_ms == 0) {
        add_device(spec, port_path);
        return;
    }
    auto t = std::make_shared<asio::steady_timer>(rt_->context(), std::chrono::milliseconds(delay_ms));
    t->async_wait([this, t, spec, port_path](std::error_code ec) {
        if (!ec) add_device(spec, port_path);
    });
}

std::size_t LoopbackBackend::present_count() const {
    std::lock_guard lk(m_);
    return devices_.size();
}

std::size_t LoopbackBackend::open_count() const { return open_count_.load(); }

std::unique_ptr<IUsbBackend> make_loopback_backend(std::uint32_t initial_devices) {
    return std::make_unique<LoopbackBackend>(initial_devices);
}

}  // namespace pf::server

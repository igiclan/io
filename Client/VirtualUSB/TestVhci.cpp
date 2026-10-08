#include "VirtualUSB/TestVhci.hpp"

#include "Common/Platform.hpp"
#include "Logging/Logger.hpp"

namespace pf::client {

TestVhci::TestVhci() = default;
TestVhci::~TestVhci() { close(); }

Status TestVhci::open() {
    std::lock_guard lk(m_);
    if (running_) return {};
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return {};
}

void TestVhci::close() {
    {
        std::lock_guard lk(m_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // like the driver on handle close: unplug everything, fail pending URBs
    std::map<std::uint64_t, Pending> victims;
    {
        std::lock_guard lk(m_);
        victims.swap(pending_);
        devices_.clear();
    }
    for (auto& [id, p] : victims) {
        if (p.op) p.opp.set_value(UsbStatus::DeviceGone);
        else p.urb.set_value(UrbResult{UsbStatus::DeviceGone, 0, {}, {}});
    }
}

VhciInfo TestVhci::info() const {
    VhciInfo i;
    i.abi_version = 1;
    i.driver_version = "test-double";
    i.usb2_ports = 8;
    i.usb3_ports = 8;
    i.is_test_double = true;
    return i;
}

void TestVhci::run() {
    set_current_thread_name("pf-testvhci");
    std::unique_lock lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
        if (!running_) return;
        // Batch everything queued into one "FETCH buffer", as the driver does.
        Bytes buf;
        while (!queue_.empty()) {
            buf.insert(buf.end(), queue_.front().begin(), queue_.front().end());
            queue_.pop_front();
        }
        lk.unlock();
        auto records = parse_fetch_buffer(buf);
        if (!records) {
            log::critical("VirtualUSB", "TestVhci produced an unparsable record stream: " + records.error().describe());
        } else {
            for (const auto& r : *records) {
                if (sink_) sink_(r);
            }
        }
        lk.lock();
    }
}

void TestVhci::deliver(Bytes record) {
    {
        std::lock_guard lk(m_);
        queue_.push_back(std::move(record));
    }
    cv_.notify_all();
}

Result<PluggedDevice> TestVhci::plug(const DescriptorSet& set, UsbSpeed speed, std::uint64_t cookie) {
    // Round-trip through the PLUGIN ABI exactly like the driver would receive it.
    Bytes in = encode_plugin(set, speed, cookie);
    UsbSpeed sp{};
    std::uint64_t ck = 0;
    auto decoded = decode_plugin(in, sp, ck);
    if (!decoded) return decoded.error();
    std::lock_guard lk(m_);
    const std::uint32_t h = next_handle_++;
    devices_[h] = Device{ck, *decoded, sp, h};
    events_.push_back(PlugEvent{true, h, ck, *decoded, sp});
    cv_.notify_all();
    return PluggedDevice{h, h};
}

Status TestVhci::unplug(std::uint32_t handle) {
    std::vector<Pending> victims;
    {
        std::lock_guard lk(m_);
        auto it = devices_.find(handle);
        if (it == devices_.end()) return {ErrorCode::NotFound, "handle"};
        events_.push_back(PlugEvent{false, handle, it->second.cookie, it->second.desc, it->second.speed});
        devices_.erase(it);
        for (auto p = pending_.begin(); p != pending_.end();) {
            if (p->second.handle == handle) {
                victims.push_back(std::move(p->second));
                p = pending_.erase(p);
            } else {
                ++p;
            }
        }
    }
    for (auto& p : victims) {
        if (p.op) p.opp.set_value(UsbStatus::DeviceGone);
        else p.urb.set_value(UrbResult{UsbStatus::DeviceGone, 0, {}, {}});
    }
    cv_.notify_all();
    return {};
}

Status TestVhci::complete(const VhciCompletion& c) {
    Bytes buf;
    append_completion(buf, c);
    auto parsed = parse_complete_buffer(buf);
    if (!parsed || parsed->size() != 1) return {ErrorCode::DriverError, "completion record invalid"};
    const VhciCompletion& pc = (*parsed)[0];
    Pending p;
    {
        std::lock_guard lk(m_);
        auto it = pending_.find(pc.request_id);
        if (it == pending_.end()) return {};  // late completion of a cancelled URB: ignored like the driver
        if (it->second.handle != pc.device_handle) return {ErrorCode::DriverError, "completion for wrong device"};
        p = std::move(it->second);
        pending_.erase(it);
    }
    if (p.op) {
        p.opp.set_value(pc.status);
        return {};
    }
    UrbResult r;
    r.status = pc.status;
    r.actual_length = pc.actual_length;
    r.iso = pc.iso;
    if (p.in) {
        const std::size_t n = pc.iso.empty() ? pc.actual_length : pc.in_data.size();
        if (n > pc.in_data.size()) return {ErrorCode::DriverError, "completion shorter than actual_length"};
        r.data.assign(pc.in_data.begin(), pc.in_data.begin() + static_cast<std::ptrdiff_t>(n));
    }
    p.urb.set_value(std::move(r));
    return {};
}

std::future<UrbResult> TestVhci::submit_urb(std::uint32_t handle, const TestUrb& urb, std::uint64_t* request_id) {
    VhciRequest q;
    q.kind = VhciRecordKind::Submit;
    q.device_handle = handle;
    q.endpoint = urb.endpoint;
    q.type = urb.type;
    q.flags = urb.flags;
    q.setup = urb.setup;
    q.iso = urb.iso;
    const bool in = urb.type == TransferType::Control ? (urb.setup[0] & 0x80) != 0 : (urb.endpoint & 0x80) != 0;
    if (in) q.flags |= xfer_flags::kDirIn;
    q.length = in ? urb.length : static_cast<std::uint32_t>(urb.out.size());
    q.out_data = urb.out;
    Pending p;
    p.handle = handle;
    p.in = in;
    p.op = false;
    auto fut = p.urb.get_future();
    Bytes rec;
    {
        std::lock_guard lk(m_);
        auto dev = devices_.find(handle);
        if (dev == devices_.end()) {
            p.urb.set_value(UrbResult{UsbStatus::NoDevice, 0, {}, {}});
            return fut;
        }
        q.cookie = dev->second.cookie;
        q.request_id = next_request_++;
        if (request_id) *request_id = q.request_id;
        append_request(rec, q);
        pending_.emplace(q.request_id, std::move(p));
    }
    deliver(std::move(rec));
    return fut;
}

void TestVhci::cancel_urb(std::uint32_t handle, std::uint64_t request_id) {
    Pending p;
    std::uint64_t cookie = 0;
    {
        std::lock_guard lk(m_);
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return;
        p = std::move(it->second);
        pending_.erase(it);
        if (auto d = devices_.find(handle); d != devices_.end()) cookie = d->second.cookie;
    }
    p.urb.set_value(UrbResult{UsbStatus::Cancelled, 0, {}, {}});
    VhciRequest q;
    q.kind = VhciRecordKind::Cancel;
    q.device_handle = handle;
    q.cookie = cookie;
    q.request_id = request_id;
    Bytes rec;
    append_request(rec, q);
    deliver(std::move(rec));
}

std::future<UsbStatus> TestVhci::op(std::uint32_t handle, VhciRecordKind kind, std::uint8_t a, std::uint8_t b,
                                    std::uint8_t ep) {
    VhciRequest q;
    q.kind = kind;
    q.device_handle = handle;
    q.config_value = a;
    q.interface_number = a;
    q.alt_setting = b;
    q.op_endpoint = ep;
    Pending p;
    p.handle = handle;
    p.op = true;
    p.in = false;
    auto fut = p.opp.get_future();
    Bytes rec;
    {
        std::lock_guard lk(m_);
        auto dev = devices_.find(handle);
        if (dev == devices_.end()) {
            p.opp.set_value(UsbStatus::NoDevice);
            return fut;
        }
        q.cookie = dev->second.cookie;
        q.request_id = next_request_++;
        append_request(rec, q);
        pending_.emplace(q.request_id, std::move(p));
    }
    deliver(std::move(rec));
    return fut;
}

std::future<UsbStatus> TestVhci::select_interface(std::uint32_t h, std::uint8_t iface, std::uint8_t alt) {
    return op(h, VhciRecordKind::SelectInterface, iface, alt, 0);
}
std::future<UsbStatus> TestVhci::select_configuration(std::uint32_t h, std::uint8_t value) {
    return op(h, VhciRecordKind::SelectConfig, value, 0, 0);
}
std::future<UsbStatus> TestVhci::reset_endpoint(std::uint32_t h, std::uint8_t ep) {
    return op(h, VhciRecordKind::EndpointReset, 0, 0, ep);
}

namespace {
UrbResult wait(std::future<UrbResult>& f) {
    if (f.wait_for(std::chrono::seconds(10)) != std::future_status::ready) return UrbResult{UsbStatus::Timeout, 0, {}, {}};
    return f.get();
}
}  // namespace

UrbResult TestVhci::control_in(std::uint32_t h, std::uint8_t bm, std::uint8_t req, std::uint16_t value,
                               std::uint16_t index, std::uint16_t length) {
    TestUrb u;
    u.type = TransferType::Control;
    u.endpoint = 0x80;
    u.setup = {bm, req, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
               static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8),
               static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8)};
    u.length = length;
    auto f = submit_urb(h, u);
    return wait(f);
}

UrbResult TestVhci::control_out(std::uint32_t h, std::uint8_t bm, std::uint8_t req, std::uint16_t value,
                                std::uint16_t index, const Bytes& data) {
    TestUrb u;
    u.type = TransferType::Control;
    u.endpoint = 0x00;
    const auto len = static_cast<std::uint16_t>(data.size());
    u.setup = {bm, req, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
               static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(len),
               static_cast<std::uint8_t>(len >> 8)};
    u.out = data;
    auto f = submit_urb(h, u);
    return wait(f);
}

UrbResult TestVhci::bulk_out(std::uint32_t h, std::uint8_t ep, const Bytes& data) {
    TestUrb u;
    u.endpoint = ep;
    u.out = data;
    auto f = submit_urb(h, u);
    return wait(f);
}

UrbResult TestVhci::bulk_in(std::uint32_t h, std::uint8_t ep, std::uint32_t length) {
    TestUrb u;
    u.endpoint = ep;
    u.length = length;
    auto f = submit_urb(h, u);
    return wait(f);
}

std::vector<PlugEvent> TestVhci::events() const {
    std::lock_guard lk(m_);
    return events_;
}

std::size_t TestVhci::plugged_count() const {
    std::lock_guard lk(m_);
    return devices_.size();
}

std::size_t TestVhci::pending_urbs() const {
    std::lock_guard lk(m_);
    return pending_.size();
}

std::uint32_t TestVhci::wait_for_device(std::uint16_t vid, std::uint16_t pid, std::chrono::milliseconds timeout) {
    std::unique_lock lk(m_);
    std::uint32_t found = 0;
    cv_.wait_for(lk, timeout, [&] {
        for (const auto& [h, d] : devices_) {
            auto dd = parse_device_descriptor(d.desc.device);
            if (dd && dd->idVendor == vid && dd->idProduct == pid) {
                found = h;
                return true;
            }
        }
        return !running_;
    });
    return found;
}

}  // namespace pf::client

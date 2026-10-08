// Production USB backend built on libusb-1.0.
//
// Windows: libusb's UsbDk backend (LIBUSB_OPTION_USE_USBDK) captures any device while
// it is open and returns it to its original driver on close; without UsbDk, only
// devices bound to WinUSB/libusbK can be opened. Linux: usbfs with automatic kernel
// driver detach/reattach.
//
// Enumeration never opens devices (opening = capturing with UsbDk). Descriptors for
// listing are rebuilt from libusb's cached copies; strings come from the OS inventory.
// After capture, exact descriptors are re-read from the device with GET_DESCRIPTOR.
#include "USB/UsbBackend.hpp"

#ifdef PF_HAVE_LIBUSB

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "Common/Platform.hpp"
#include "Logging/Logger.hpp"
#include "USB/DeviceInventory.hpp"

namespace pf::server {

namespace {

UsbSpeed map_speed(int s) {
    switch (s) {
        case LIBUSB_SPEED_LOW: return UsbSpeed::Low;
        case LIBUSB_SPEED_FULL: return UsbSpeed::Full;
        case LIBUSB_SPEED_HIGH: return UsbSpeed::High;
        case LIBUSB_SPEED_SUPER: return UsbSpeed::Super;
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000107
        case LIBUSB_SPEED_SUPER_PLUS: return UsbSpeed::SuperPlus;
#endif
        default: return UsbSpeed::Unknown;
    }
}

UsbStatus map_transfer_status(libusb_transfer_status s) {
    switch (s) {
        case LIBUSB_TRANSFER_COMPLETED: return UsbStatus::Success;
        case LIBUSB_TRANSFER_ERROR: return UsbStatus::IoError;
        case LIBUSB_TRANSFER_TIMED_OUT: return UsbStatus::Timeout;
        case LIBUSB_TRANSFER_CANCELLED: return UsbStatus::Cancelled;
        case LIBUSB_TRANSFER_STALL: return UsbStatus::Stall;
        case LIBUSB_TRANSFER_NO_DEVICE: return UsbStatus::DeviceGone;
        case LIBUSB_TRANSFER_OVERFLOW: return UsbStatus::Overflow;
    }
    return UsbStatus::IoError;
}

Error map_error(int rc, std::string_view what) {
    ErrorCode c = ErrorCode::Io;
    switch (rc) {
        case LIBUSB_ERROR_ACCESS: c = ErrorCode::DeviceAccess; break;
        case LIBUSB_ERROR_NO_DEVICE: c = ErrorCode::DeviceGone; break;
        case LIBUSB_ERROR_NOT_FOUND: c = ErrorCode::DeviceNotFound; break;
        case LIBUSB_ERROR_BUSY: c = ErrorCode::DeviceBusy; break;
        case LIBUSB_ERROR_TIMEOUT: c = ErrorCode::Timeout; break;
        case LIBUSB_ERROR_NOT_SUPPORTED: c = ErrorCode::DeviceAccess; break;
        default: break;
    }
    std::string msg = std::string(what) + ": " + libusb_error_name(rc);
    if (rc == LIBUSB_ERROR_ACCESS) {
#ifdef _WIN32
        msg += " (install UsbDk, or bind the device to WinUSB)";
#else
        msg += " (permission denied: install the ParsFlash udev rule or run as root)";
#endif
    }
#ifdef _WIN32
    if (rc == LIBUSB_ERROR_NOT_SUPPORTED) msg += " (device driver not capturable: install UsbDk)";
#endif
    return Error{c, msg};
}

std::string port_path_of(libusb_device* dev, std::string* chain_out = nullptr) {
    std::uint8_t ports[8];
    const int n = libusb_get_port_numbers(dev, ports, 8);
    std::string chain;
    for (int i = 0; i < n; ++i) {
        if (i) chain += '.';
        chain += std::to_string(ports[i]);
    }
    if (chain_out) *chain_out = chain;
    return std::to_string(libusb_get_bus_number(dev)) + "-" + chain;
}

// Rebuilds the exact configuration descriptor byte stream from libusb's parsed copy:
// libusb attaches every unparsed descriptor ("extra") to the descriptor it follows,
// so emitting each descriptor followed by its extra reproduces the original order.
Bytes serialize_config(const libusb_config_descriptor* c) {
    Bytes b{9, LIBUSB_DT_CONFIG, 0, 0, c->bNumInterfaces, c->bConfigurationValue, c->iConfiguration,
            c->bmAttributes, c->MaxPower};
    b.insert(b.end(), c->extra, c->extra + c->extra_length);
    for (int i = 0; i < c->bNumInterfaces; ++i) {
        const libusb_interface& itf = c->interface[i];
        for (int a = 0; a < itf.num_altsetting; ++a) {
            const libusb_interface_descriptor& d = itf.altsetting[a];
            b.insert(b.end(), {9, LIBUSB_DT_INTERFACE, d.bInterfaceNumber, d.bAlternateSetting, d.bNumEndpoints,
                               d.bInterfaceClass, d.bInterfaceSubClass, d.bInterfaceProtocol, d.iInterface});
            b.insert(b.end(), d.extra, d.extra + d.extra_length);
            for (int e = 0; e < d.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = d.endpoint[e];
                const std::uint8_t len = ep.bLength >= 9 ? 9 : 7;  // audio endpoints carry 2 extra bytes
                b.insert(b.end(), {len, LIBUSB_DT_ENDPOINT, ep.bEndpointAddress, ep.bmAttributes,
                                   static_cast<std::uint8_t>(ep.wMaxPacketSize & 0xFF),
                                   static_cast<std::uint8_t>(ep.wMaxPacketSize >> 8), ep.bInterval});
                if (len == 9) b.insert(b.end(), {ep.bRefresh, ep.bSynchAddress});
                b.insert(b.end(), ep.extra, ep.extra + ep.extra_length);
            }
        }
    }
    store_le16(&b[2], static_cast<std::uint16_t>(b.size()));
    return b;
}

Bytes device_descriptor_bytes(const libusb_device_descriptor& d) {
    Bytes b(18);
    b[0] = 18;
    b[1] = LIBUSB_DT_DEVICE;
    store_le16(&b[2], d.bcdUSB);
    b[4] = d.bDeviceClass;
    b[5] = d.bDeviceSubClass;
    b[6] = d.bDeviceProtocol;
    b[7] = d.bMaxPacketSize0;
    store_le16(&b[8], d.idVendor);
    store_le16(&b[10], d.idProduct);
    store_le16(&b[12], d.bcdDevice);
    b[14] = d.iManufacturer;
    b[15] = d.iProduct;
    b[16] = d.iSerialNumber;
    b[17] = d.bNumConfigurations;
    return b;
}

// ------------------------------------------------------------------ device

class LibusbDevice;

struct InFlight {
    LibusbDevice* owner;
    libusb_transfer* xfer;
    TransferRequest req;
    TransferCallback done;
};

class LibusbDevice final : public IUsbDevice {
public:
    LibusbDevice(libusb_device_handle* h, UsbSpeed speed) : h_(h), speed_(speed) {}
    ~LibusbDevice() override { close(); }

    Status init() {
        libusb_set_auto_detach_kernel_driver(h_, 1);  // no-op where unsupported (Windows)
        if (auto st = read_descriptors(); !st) return st;
        claim_active_interfaces();
        return {};
    }

    void submit(TransferRequest&& req, TransferCallback done) override {
        if (closing_) return fail(std::move(req), std::move(done), UsbStatus::DeviceGone);
        const int iso = static_cast<int>(req.iso.size());
        libusb_transfer* t = libusb_alloc_transfer(iso);
        if (!t) return fail(std::move(req), std::move(done), UsbStatus::NoResources);
        auto* f = new InFlight{this, t, std::move(req), std::move(done)};
        TransferRequest& r = f->req;
        const unsigned timeout = r.timeout_ms;
        switch (r.type) {
            case TransferType::Control: {
                const std::uint16_t w_length = static_cast<std::uint16_t>(r.setup[6] | (r.setup[7] << 8));
                if (w_length > r.length) return reject(f, UsbStatus::InvalidRequest);
                std::uint8_t* setup = r.data() - 8;
                std::memcpy(setup, r.setup.data(), 8);
                libusb_fill_control_transfer(t, h_, setup, &LibusbDevice::on_complete, f, timeout);
                break;
            }
            case TransferType::Bulk:
                libusb_fill_bulk_transfer(t, h_, r.endpoint, r.data(), static_cast<int>(r.length),
                                          &LibusbDevice::on_complete, f, timeout);
                break;
            case TransferType::Interrupt:
                libusb_fill_interrupt_transfer(t, h_, r.endpoint, r.data(), static_cast<int>(r.length),
                                               &LibusbDevice::on_complete, f, timeout);
                break;
            case TransferType::Isochronous: {
                // libusb packs iso packets back to back; require the same layout.
                std::uint32_t expect = 0;
                for (const auto& p : r.iso) {
                    if (p.offset != expect) return reject(f, UsbStatus::NotSupported);
                    expect += p.length;
                }
                libusb_fill_iso_transfer(t, h_, r.endpoint, r.data(), static_cast<int>(r.length), iso,
                                         &LibusbDevice::on_complete, f, timeout);
                for (int i = 0; i < iso; ++i) t->iso_packet_desc[i].length = r.iso[static_cast<std::size_t>(i)].length;
                break;
            }
        }
        if (r.flags & xfer_flags::kShortNotOk) t->flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
        if (r.flags & xfer_flags::kZeroPacket) t->flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        {
            std::lock_guard lk(m_);
            inflight_[f->req.seq] = f;
        }
        const int rc = libusb_submit_transfer(t);
        if (rc != 0) {
            {
                std::lock_guard lk(m_);
                inflight_.erase(f->req.seq);
            }
            cv_.notify_all();
            reject(f, rc == LIBUSB_ERROR_NO_DEVICE ? UsbStatus::DeviceGone
                      : rc == LIBUSB_ERROR_BUSY    ? UsbStatus::Busy
                                                   : UsbStatus::IoError);
        }
    }

    void cancel(std::uint64_t seq) override {
        std::lock_guard lk(m_);
        auto it = inflight_.find(seq);
        if (it != inflight_.end()) libusb_cancel_transfer(it->second->xfer);
    }

    void cancel_all() override {
        std::lock_guard lk(m_);
        for (auto& [seq, f] : inflight_) libusb_cancel_transfer(f->xfer);
    }

    Status reset_port() override {
        const int rc = libusb_reset_device(h_);
        if (rc == LIBUSB_ERROR_NOT_FOUND) return {ErrorCode::DeviceGone, "device re-enumerated after reset"};
        if (rc != 0) return map_error(rc, "reset");
        return {};
    }

    Status set_configuration(std::uint8_t v) override {
        release_interfaces();
        const int rc = libusb_set_configuration(h_, v == 0 ? -1 : v);
        claim_active_interfaces();
        if (rc != 0) return map_error(rc, "set configuration");
        return {};
    }

    Status set_interface(std::uint8_t iface, std::uint8_t alt) override {
        const int rc = libusb_set_interface_alt_setting(h_, iface, alt);
        if (rc != 0) return map_error(rc, "set interface");
        return {};
    }

    Status clear_halt(std::uint8_t ep) override {
        const int rc = libusb_clear_halt(h_, ep);
        if (rc != 0) return map_error(rc, "clear halt");
        return {};
    }

    void close() override {
        if (closing_.exchange(true)) return;
        cancel_all();
        {
            // Completions run on the backend's event thread; wait for all of them.
            std::unique_lock lk(m_);
            cv_.wait_for(lk, std::chrono::seconds(5), [&] { return inflight_.empty(); });
            if (!inflight_.empty())
                log::error("USB", "close: " + std::to_string(inflight_.size()) + " transfers did not complete");
        }
        release_interfaces();
        libusb_close(h_);  // Linux: kernel drivers re-attached (auto detach); UsbDk: device released
        h_ = nullptr;
    }

    const DescriptorSet& descriptors() const override { return desc_; }
    UsbSpeed speed() const override { return speed_; }

private:
    static void LIBUSB_CALL on_complete(libusb_transfer* t) {
        auto* f = static_cast<InFlight*>(t->user_data);
        LibusbDevice* self = f->owner;
        TransferResult r;
        r.seq = f->req.seq;
        r.status = map_transfer_status(t->status);
        r.actual_length = static_cast<std::uint32_t>(t->actual_length);
        r.data_offset = f->req.data_offset;
        if (f->req.type == TransferType::Isochronous) {
            std::uint32_t total = 0;
            for (int i = 0; i < t->num_iso_packets; ++i) {
                const auto& p = t->iso_packet_desc[i];
                const auto st = map_transfer_status(p.status);
                r.iso.push_back({p.actual_length, static_cast<std::int32_t>(st)});
                if (st != UsbStatus::Success) ++r.iso_error_count;
                total += p.actual_length;
            }
            r.actual_length = total;
        }
        r.buffer = std::move(f->req.buffer);
        libusb_free_transfer(t);
        TransferCallback done = std::move(f->done);
        const std::uint64_t seq = f->req.seq;
        delete f;
        {
            // Notify under the lock: once it is released close() may destroy *self.
            std::lock_guard lk(self->m_);
            self->inflight_.erase(seq);
            self->cv_.notify_all();
        }
        done(std::move(r));
    }

    static void fail(TransferRequest&& req, TransferCallback&& done, UsbStatus st) {
        TransferResult r;
        r.seq = req.seq;
        r.status = st;
        r.data_offset = req.data_offset;
        r.buffer = std::move(req.buffer);
        done(std::move(r));
    }

    void reject(InFlight* f, UsbStatus st) {
        libusb_free_transfer(f->xfer);
        TransferRequest req = std::move(f->req);
        TransferCallback done = std::move(f->done);
        delete f;
        fail(std::move(req), std::move(done), st);
    }

    Status read_descriptors() {
        std::uint8_t buf[4096];
        int n = libusb_get_descriptor(h_, LIBUSB_DT_DEVICE, 0, buf, 18);
        if (n < 18) return map_error(n < 0 ? n : LIBUSB_ERROR_IO, "GET_DESCRIPTOR(device)");
        desc_.device.assign(buf, buf + 18);
        auto dd = parse_device_descriptor(desc_.device);
        if (!dd) return {ErrorCode::DeviceAccess, "invalid device descriptor"};
        for (std::uint8_t i = 0; i < dd->bNumConfigurations; ++i) {
            Bytes cfg(9);
            n = libusb_get_descriptor(h_, LIBUSB_DT_CONFIG, i, cfg.data(), 9);
            if (n < 9) return map_error(n < 0 ? n : LIBUSB_ERROR_IO, "GET_DESCRIPTOR(config)");
            const std::uint16_t total = load_le16(&cfg[2]);
            cfg.resize(total);
            n = libusb_get_descriptor(h_, LIBUSB_DT_CONFIG, i, cfg.data(), total);
            if (n != total) return map_error(n < 0 ? n : LIBUSB_ERROR_IO, "GET_DESCRIPTOR(config total)");
            desc_.configs.push_back(std::move(cfg));
        }
        if (dd->bcdUSB >= 0x0201) {
            n = libusb_get_descriptor(h_, LIBUSB_DT_BOS, 0, buf, 5);
            if (n == 5) {
                const std::uint16_t total = load_le16(&buf[2]);
                if (total <= sizeof(buf) && libusb_get_descriptor(h_, LIBUSB_DT_BOS, 0, buf, total) == total)
                    desc_.bos.assign(buf, buf + total);
            }
        }
        if (speed_ == UsbSpeed::High || speed_ == UsbSpeed::Full) {
            n = libusb_get_descriptor(h_, 0x06, 0, buf, 10);
            if (n == 10) desc_.device_qualifier.assign(buf, buf + 10);
        }
        // strings: language table, then every index referenced by the descriptors
        n = libusb_get_string_descriptor(h_, 0, 0, buf, 255);
        if (n >= 4 && buf[1] == LIBUSB_DT_STRING) {
            desc_.strings.push_back(StringDescriptor{0, 0, Bytes(buf, buf + std::min<int>(n, buf[0]))});
            const std::uint16_t lang = load_le16(&buf[2]);
            std::vector<std::uint8_t> idx{dd->iManufacturer, dd->iProduct, dd->iSerialNumber};
            for (const auto& c : desc_.configs) {
                if (c.size() > 6) idx.push_back(c[6]);
                if (auto ci = parse_configuration(c))
                    for (const auto& itf : ci->interfaces) idx.push_back(itf.iInterface);
            }
            std::sort(idx.begin(), idx.end());
            idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
            for (auto i : idx) {
                if (i == 0) continue;
                n = libusb_get_string_descriptor(h_, i, lang, buf, 255);
                if (n >= 2 && buf[1] == LIBUSB_DT_STRING && buf[0] <= n)
                    desc_.strings.push_back(StringDescriptor{i, lang, Bytes(buf, buf + buf[0])});
            }
        }
        std::string why;
        if (!validate_descriptor_set(desc_, &why)) return {ErrorCode::DeviceAccess, "descriptors: " + why};
        return {};
    }

    void claim_active_interfaces() {
        libusb_config_descriptor* c = nullptr;
        if (libusb_get_active_config_descriptor(libusb_get_device(h_), &c) != 0 || !c) return;
        for (int i = 0; i < c->bNumInterfaces; ++i) {
            const int num = c->interface[i].altsetting[0].bInterfaceNumber;
            const int rc = libusb_claim_interface(h_, num);
            if (rc == 0) {
                claimed_.push_back(num);
            } else {
                log::warn("USB", "claim interface " + std::to_string(num) + " failed: " + libusb_error_name(rc));
            }
        }
        libusb_free_config_descriptor(c);
    }

    void release_interfaces() {
        for (int i : claimed_) libusb_release_interface(h_, i);
        claimed_.clear();
    }

    libusb_device_handle* h_;
    UsbSpeed speed_;
    DescriptorSet desc_;
    std::vector<int> claimed_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::uint64_t, InFlight*> inflight_;
    std::atomic<bool> closing_{false};
};

// ------------------------------------------------------------------ backend

class LibusbBackend final : public IUsbBackend {
public:
    explicit LibusbBackend(bool use_usbdk) : use_usbdk_(use_usbdk) {}
    ~LibusbBackend() override { stop(); }

    std::string name() const override { return usbdk_active_ ? "libusb+UsbDk" : "libusb"; }

    Status start() override {
        if (ctx_) return {};
        int rc = libusb_init(&ctx_);
        if (rc != 0) return map_error(rc, "libusb_init");
#ifdef _WIN32
        if (use_usbdk_) {
            usbdk_active_ = libusb_set_option(ctx_, LIBUSB_OPTION_USE_USBDK) == LIBUSB_SUCCESS;
            if (!usbdk_active_) log::warn("USB", "UsbDk not available: only WinUSB-bound devices can be shared");
        }
#endif
        running_ = true;
        event_thread_ = std::thread([this] {
            set_current_thread_name("pf-libusb");
            while (running_) {
                timeval tv{1, 0};
                libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
            }
        });
        if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
            rc = libusb_hotplug_register_callback(
                ctx_, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
                LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
                &LibusbBackend::on_hotplug, this, &hotplug_);
            has_notifications_ = rc == LIBUSB_SUCCESS;
        } else {
            notifier_ = make_device_change_notifier([this] { fire_change(); });
            has_notifications_ = notifier_ != nullptr;
        }
        log::info("USB", "backend " + name() + " started (" +
                             (has_notifications_ ? "hotplug notifications" : "polling") + ")");
        return {};
    }

    void stop() override {
        if (!ctx_) return;
        notifier_.reset();
        if (has_notifications_ && hotplug_) libusb_hotplug_deregister_callback(ctx_, hotplug_);
        running_ = false;
        libusb_interrupt_event_handler(ctx_);
        if (event_thread_.joinable()) event_thread_.join();
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }

    Result<std::vector<EnumeratedDevice>> enumerate() override {
        if (!ctx_) return Error{ErrorCode::InvalidState, "backend not started"};
        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ctx_, &list);
        if (n < 0) return map_error(static_cast<int>(n), "get_device_list");
        const auto inventory = read_usb_inventory();
        std::vector<EnumeratedDevice> out;
        for (ssize_t i = 0; i < n; ++i) {
            libusb_device* dev = list[i];
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(dev, &dd) != 0) continue;
            if (dd.bDeviceClass == LIBUSB_CLASS_HUB) continue;  // hubs are infrastructure
            EnumeratedDevice e;
            std::string chain;
            e.identity.port_path = port_path_of(dev, &chain);
            if (chain.empty()) continue;  // root hub
            e.identity.vid = dd.idVendor;
            e.identity.pid = dd.idProduct;
            e.identity.bus = libusb_get_bus_number(dev);
            e.identity.address = libusb_get_device_address(dev);
            e.identity.port = libusb_get_port_number(dev);
            e.identity.speed = map_speed(libusb_get_device_speed(dev));
            e.descriptors.device = device_descriptor_bytes(dd);
            for (std::uint8_t c = 0; c < dd.bNumConfigurations; ++c) {
                libusb_config_descriptor* cfg = nullptr;
                if (libusb_get_config_descriptor(dev, c, &cfg) == 0 && cfg) {
                    e.descriptors.configs.push_back(serialize_config(cfg));
                    libusb_free_config_descriptor(cfg);
                }
            }
            // Merge OS inventory: match on port chain and VID/PID.
            for (const auto& inv : inventory) {
                const bool port_match = !inv.port_path.empty() ? inv.port_path == e.identity.port_path
                                                               : inv.port_chain == chain;
                if (!port_match || inv.vid != dd.idVendor || inv.pid != dd.idProduct) continue;
                e.identity.serial = inv.serial;
                e.identity.parent_id = inv.parent_id;
                e.identity.container_id = inv.container_id;
                e.identity.instance_id = inv.instance_id;
                e.identity.location = inv.location;
                e.manufacturer = inv.manufacturer;
                e.product = inv.product;
                e.driver = inv.driver;
                break;
            }
            out.push_back(std::move(e));
        }
        libusb_free_device_list(list, 1);
        return out;
    }

    Result<std::unique_ptr<IUsbDevice>> open(const DeviceIdentity& id) override {
        if (!ctx_) return Error{ErrorCode::InvalidState, "backend not started"};
        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ctx_, &list);
        if (n < 0) return map_error(static_cast<int>(n), "get_device_list");
        libusb_device* found = nullptr;
        for (ssize_t i = 0; i < n && !found; ++i) {
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (dd.idVendor == id.vid && dd.idProduct == id.pid && port_path_of(list[i]) == id.port_path)
                found = list[i];
        }
        if (!found) {
            libusb_free_device_list(list, 1);
            return Error{ErrorCode::DeviceNotFound, "device not present at " + id.port_path};
        }
        libusb_device_handle* h = nullptr;
        const int rc = libusb_open(found, &h);
        const UsbSpeed speed = map_speed(libusb_get_device_speed(found));
        libusb_free_device_list(list, 1);
        if (rc != 0) return map_error(rc, "open");
        auto dev = std::make_unique<LibusbDevice>(h, speed);
        if (auto st = dev->init(); !st) {
            dev->close();
            return st.error();
        }
        return std::unique_ptr<IUsbDevice>(std::move(dev));
    }

    void set_change_callback(std::function<void()> cb) override {
        std::lock_guard lk(cb_m_);
        on_change_ = std::move(cb);
    }
    bool has_change_notifications() const override { return has_notifications_; }

private:
    static int LIBUSB_CALL on_hotplug(libusb_context*, libusb_device*, libusb_hotplug_event, void* user) {
        static_cast<LibusbBackend*>(user)->fire_change();
        return 0;  // keep the callback registered
    }
    void fire_change() {
        std::function<void()> cb;
        {
            std::lock_guard lk(cb_m_);
            cb = on_change_;
        }
        if (cb) cb();  // engine only schedules a rescan; never re-enters libusb here
    }

    bool use_usbdk_;
    bool usbdk_active_ = false;
    libusb_context* ctx_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread event_thread_;
    libusb_hotplug_callback_handle hotplug_ = 0;
    bool has_notifications_ = false;
    std::unique_ptr<DeviceChangeNotifier> notifier_;
    std::mutex cb_m_;
    std::function<void()> on_change_;
};

}  // namespace

std::unique_ptr<IUsbBackend> make_libusb_backend(bool use_usbdk) { return std::make_unique<LibusbBackend>(use_usbdk); }

}  // namespace pf::server

#endif  // PF_HAVE_LIBUSB

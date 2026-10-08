#include "Sessions/ClientSession.hpp"

#include <nlohmann/json.hpp>

#include "Common/Platform.hpp"
#include "Logging/Logger.hpp"

namespace pf::client {

using namespace proto;

ClientSession::ClientSession(SessionParams p, IVirtualHci& vhci, std::uint64_t cookie)
    : p_(std::move(p)), vhci_(vhci), cookie_(cookie) {}

ClientSession::~ClientSession() = default;

std::string ClientSession::resume_token() const {
    std::lock_guard lk(m_);
    return p_.resume_token;
}

DeviceInfo ClientSession::device() const {
    std::lock_guard lk(m_);
    return p_.device;
}

std::uint32_t ClientSession::vhci_handle() const {
    std::lock_guard lk(m_);
    return vhci_handle_;
}

Status ClientSession::plug() {
    DescriptorSet desc;
    UsbSpeed speed;
    {
        std::lock_guard lk(m_);
        if (vhci_handle_ != 0) return {};
        desc = p_.descriptors;
        speed = p_.speed == UsbSpeed::Unknown ? UsbSpeed::High : p_.speed;
    }
    auto r = vhci_.plug(desc, speed, cookie_);
    if (!r) return r.error();
    {
        std::lock_guard lk(m_);
        vhci_handle_ = r->handle;
    }
    LogRecord(LogLevel::Info, "VirtualUSB")
        .session(p_.session_id)
        .device(p_.device.id)
        .op("plug")
        .msg(p_.device.display_name() + " plugged into virtual port " + std::to_string(r->port));
    return {};
}

void ClientSession::unplug() {
    std::uint32_t h;
    {
        std::lock_guard lk(m_);
        h = vhci_handle_;
        vhci_handle_ = 0;
    }
    if (h) {
        // The driver fails every URB still queued for the device.
        (void)vhci_.unplug(h);
        LogRecord(LogLevel::Info, "VirtualUSB").session(p_.session_id).op("unplug").msg(p_.device.display_name());
    }
}

// ------------------------------------------------------------ VHCI -> network

void ClientSession::complete_vhci(const Pending& p, UsbStatus st, std::uint32_t actual, ByteSpan data,
                                  std::vector<IsoResult> iso, std::uint32_t iso_errors) {
    VhciCompletion c;
    c.is_op_result = p.op;
    c.device_handle = p.vhci_handle;
    c.cookie = cookie_;
    c.request_id = p.request_id;
    c.status = st;
    c.actual_length = actual;
    c.in_data = data;
    c.iso = std::move(iso);
    c.iso_error_count = iso_errors;
    if (auto s = vhci_.complete(c); !s)
        log::warn("VirtualUSB", "completion rejected by driver: " + s.error().describe());
}

void ClientSession::send_or_hold(OutFrame&& f, std::uint64_t seq, Pending p) {
    f.priority = static_cast<std::uint8_t>(p_.priority);
    std::shared_ptr<net::FrameConnection> link;
    {
        std::lock_guard lk(m_);
        link = link_.lock();
        if (!link) {
            // Not sent yet, so holding it is safe (no risk of executing twice).
            if (held_bytes_ + f.buf.size() > p_.max_held_bytes) {
                link.reset();
            } else {
                held_bytes_ += f.buf.size();
                held_.push_back(Held{std::move(f), seq, p});
                return;
            }
            // over budget: fail below
            by_request_.erase(p.request_id);
            inflight_.erase(seq);
            complete_vhci(p, UsbStatus::NoResources);
            return;
        }
        inflight_[seq] = p;
    }
    link->send(std::move(f), p.op);
}

void ClientSession::on_vhci_request(const VhciRequest& r) {
    if (r.device_handle != vhci_handle()) {
        // Record for a previous incarnation of the virtual device (replug race).
        Pending p{r.request_id, r.device_handle, 0, 0, false, r.kind != VhciRecordKind::Submit};
        complete_vhci(p, UsbStatus::DeviceGone);
        return;
    }
    if (r.kind == VhciRecordKind::Cancel) {
        std::uint64_t seq = 0;
        std::shared_ptr<net::FrameConnection> link;
        {
            std::lock_guard lk(m_);
            auto it = by_request_.find(r.request_id);
            if (it == by_request_.end()) return;
            seq = it->second;
            // drop from the held queue if it never left
            for (auto h = held_.begin(); h != held_.end(); ++h) {
                if (h->seq == seq) {
                    held_bytes_ -= h->frame.buf.size();
                    held_.erase(h);
                    break;
                }
            }
            by_request_.erase(it);
            inflight_.erase(seq);
            link = link_.lock();
        }
        metrics_.cancels.add();
        // The driver already completed the URB toward Windows; tell the server to stop.
        if (link) link->send(encode_cancel(UsbCancelMsg{seq, p_.handle}), true);
        return;
    }
    std::uint64_t seq;
    {
        std::lock_guard lk(m_);
        seq = next_seq_++;
        by_request_[r.request_id] = seq;
    }
    Pending p{r.request_id, r.device_handle, monotonic_ns(), r.length, r.is_in(), r.kind != VhciRecordKind::Submit};
    switch (r.kind) {
        case VhciRecordKind::Submit: {
            if (p_.access == AccessMode::SharedReadOnly) {
                {
                    std::lock_guard lk(m_);
                    by_request_.erase(r.request_id);
                }
                complete_vhci(p, UsbStatus::NotAuthorized);
                return;
            }
            UsbSubmitMsg m;
            m.seq = seq;
            m.device = p_.handle;
            m.endpoint = r.endpoint;
            m.type = r.type;
            m.flags = r.flags;
            m.timeout_ms = r.timeout_ms;
            m.length = r.length;
            m.setup = r.setup;
            m.iso = r.iso;
            m.out_data = r.out_data;  // copied once into the frame buffer
            metrics_.transfers.add();
            send_or_hold(encode_submit(m), seq, p);
            return;
        }
        case VhciRecordKind::SelectConfig:
            send_or_hold(encode_config(UsbConfigMsg{seq, p_.handle, ConfigOp::SetConfiguration, r.config_value, 0}), seq,
                         p);
            return;
        case VhciRecordKind::SelectInterface:
            send_or_hold(encode_config(UsbConfigMsg{seq, p_.handle, ConfigOp::SetInterface, r.interface_number,
                                                    r.alt_setting}),
                         seq, p);
            return;
        case VhciRecordKind::EndpointReset:
            send_or_hold(encode_config(UsbConfigMsg{seq, p_.handle, ConfigOp::ClearHalt, r.op_endpoint, 0}), seq, p);
            return;
        case VhciRecordKind::DeviceReset:
            metrics_.resets.add();
            send_or_hold(encode_reset(UsbResetMsg{seq, p_.handle, ResetKind::PortReset}), seq, p);
            return;
        case VhciRecordKind::DeviceRemoved:
        case VhciRecordKind::Cancel:
            {
                std::lock_guard lk(m_);
                by_request_.erase(r.request_id);
            }
            return;
    }
}

// ------------------------------------------------------------ network -> VHCI

void ClientSession::on_complete(const UsbCompleteMsg& m) {
    Pending p{};
    {
        std::lock_guard lk(m_);
        auto it = inflight_.find(m.seq);
        if (it == inflight_.end()) return;  // cancelled or failed locally already
        p = it->second;
        inflight_.erase(it);
        by_request_.erase(p.request_id);
    }
    metrics_.latency.record_ns(monotonic_ns() - p.start_ns);
    switch (m.status) {
        case UsbStatus::Success: break;
        case UsbStatus::Timeout: metrics_.timeouts.add(); break;
        case UsbStatus::Stall: metrics_.stalls.add(); break;
        case UsbStatus::Cancelled: metrics_.cancels.add(); break;
        default: metrics_.errors.add(); break;
    }
    if (p.in) metrics_.bytes_in.add(m.actual_length);
    else metrics_.bytes_out.add(m.actual_length);
    if (p.op) {
        complete_vhci(p, m.status);
        return;
    }
    complete_vhci(p, m.status, m.actual_length, p.in ? m.in_data : ByteSpan{}, m.iso, m.iso_error_count);
}

void ClientSession::on_op_result(const UsbOpResultMsg& m) {
    Pending p{};
    {
        std::lock_guard lk(m_);
        auto it = inflight_.find(m.seq);
        if (it == inflight_.end()) return;
        p = it->second;
        inflight_.erase(it);
        by_request_.erase(p.request_id);
    }
    complete_vhci(p, m.status);
}

void ClientSession::fail_all_inflight(UsbStatus st) {
    std::unordered_map<std::uint64_t, Pending> victims;
    {
        std::lock_guard lk(m_);
        victims.swap(inflight_);
        for (const auto& [seq, p] : victims) by_request_.erase(p.request_id);
    }
    for (const auto& [seq, p] : victims) complete_vhci(p, st);
}

void ClientSession::link_up(std::shared_ptr<net::FrameConnection> conn) {
    {
        // Flush held URBs and publish the link atomically: a URB arriving concurrently
        // either is in held_ (flushed here, in order) or sees the link afterwards.
        std::lock_guard lk(m_);
        for (auto& h : held_) {
            inflight_[h.seq] = h.p;
            conn->send(std::move(h.frame), h.p.op);
        }
        held_.clear();
        held_bytes_ = 0;
        link_ = conn;
    }
    sm_.transition(SessionState::Active, "link up");
}

void ClientSession::link_down() {
    {
        std::lock_guard lk(m_);
        link_.reset();
    }
    // Transfers that may have reached the device cannot be replayed safely; fail them
    // with a transport error so the application retries at its own protocol level.
    fail_all_inflight(UsbStatus::IoError);
    if (sm_.state() == SessionState::Active) sm_.transition(SessionState::Suspended, "network connection lost");
}

void ClientSession::device_removed() {
    fail_all_inflight(UsbStatus::DeviceGone);
    {
        std::lock_guard lk(m_);
        for (auto& h : held_) by_request_.erase(h.p.request_id);
        held_.clear();
        held_bytes_ = 0;
    }
    // Mirror reality: the phone left the bus, so it leaves the client's bus too.
    unplug();
    sm_.transition(SessionState::Reconnecting, "device re-enumerating on server");
}

Status ClientSession::device_reenumerated(const DescriptorSet& d, UsbSpeed speed, const DeviceInfo& info) {
    fail_all_inflight(UsbStatus::DeviceGone);
    unplug();
    {
        std::lock_guard lk(m_);
        p_.descriptors = d;
        p_.speed = speed;
        p_.device = info;
    }
    auto st = plug();
    if (!st) {
        sm_.transition(SessionState::Failed, "replug failed: " + st.error().describe());
        return st;
    }
    sm_.transition(SessionState::Active, "device re-enumerated as " + info.mode.label);
    LogRecord(LogLevel::Info, "Sessions")
        .session(p_.session_id)
        .device(info.id)
        .op("reenumerated")
        .msg("session preserved across mode switch -> " + info.mode.label + " (" +
             format_vid_pid(info.identity.vid, info.identity.pid) + ")");
    return {};
}

void ClientSession::close(const std::string& reason) {
    fail_all_inflight(UsbStatus::DeviceGone);
    {
        std::lock_guard lk(m_);
        held_.clear();
        held_bytes_ = 0;
        by_request_.clear();
        link_.reset();
    }
    unplug();
    sm_.transition(SessionState::Closed, reason);
}

std::size_t ClientSession::inflight() const {
    std::lock_guard lk(m_);
    return inflight_.size();
}

std::size_t ClientSession::held() const {
    std::lock_guard lk(m_);
    return held_.size();
}

nlohmann::json ClientSession::to_json() const {
    const DeviceInfo d = device();
    nlohmann::json j{{"session_id", p_.session_id},
                     {"server", p_.server},
                     {"handle", p_.handle},
                     {"state", std::string(pf::to_string(sm_.state()))},
                     {"device_id", d.id},
                     {"device", d.display_name()},
                     {"vid_pid", format_vid_pid(d.identity.vid, d.identity.pid)},
                     {"mode", d.mode.label},
                     {"speed", std::string(pf::to_string(p_.speed))},
                     {"access", std::string(pf::to_string(p_.access))},
                     {"priority", std::string(pf::to_string(p_.priority))},
                     {"vhci_handle", vhci_handle()},
                     {"inflight", inflight()},
                     {"held", held()},
                     {"usb", pf::to_json(metrics_)}};
    return j;
}

}  // namespace pf::client

// Windows bridge to pfvhci.sys (see Docs/DRIVER.md §IOCTL interface).
//
// Inverted call: `fetch_threads` threads each keep an overlapped IOCTL_PFVHCI_FETCH
// pended in the driver; the driver completes it as soon as URB records are queued
// (several per buffer under load). Completions are returned with IOCTL_PFVHCI_COMPLETE.
//
// STATUS: compiled (MinGW-w64) in CI; exercised against the real driver only once
// pfvhci.sys has been built with the WDK (it cannot be built in this environment).
#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "../../Shared/Driver/pfvhci_ioctl.h"
#include "Common/Platform.hpp"
#include "Logging/Logger.hpp"
#include "VirtualUSB/VirtualHci.hpp"

namespace pf::client {

namespace {

const GUID kPfvhciGuid = PFVHCI_INTERFACE_GUID_INIT;

std::string win_error(DWORD e) {
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string s = msg ? msg : ("error " + std::to_string(e));
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::wstring find_interface_path() {
    HDEVINFO set = SetupDiGetClassDevsW(&kPfvhciGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &kPfvhciGuid, 0, &ifd)) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &size, nullptr);
        std::vector<std::uint8_t> buf(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, size, nullptr, nullptr)) path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

class WinVhci final : public IVirtualHci {
public:
    explicit WinVhci(const VhciConfig& cfg) : cfg_(cfg) {}
    ~WinVhci() override { close(); }

    Status open() override {
        const std::wstring path = find_interface_path();
        if (path.empty())
            return {ErrorCode::DriverNotInstalled,
                    "ParsFlash virtual USB controller (pfvhci) is not installed - run the client installer"};
        h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                         nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            h_ = nullptr;
            return {e == ERROR_ACCESS_DENIED ? ErrorCode::AccessDenied : ErrorCode::DriverError,
                    "open pfvhci: " + win_error(e)};
        }
        PFVHCI_VERSION_INFO v{};
        DWORD got = 0;
        if (!ioctl(IOCTL_PFVHCI_GET_VERSION, nullptr, 0, &v, sizeof(v), &got) || got < sizeof(v)) {
            close();
            return {ErrorCode::DriverError, "pfvhci GET_VERSION failed"};
        }
        if (v.AbiVersion != PFVHCI_ABI_VERSION) {
            close();
            return {ErrorCode::DriverVersionMismatch, "pfvhci ABI " + std::to_string(v.AbiVersion) + ", client expects " +
                                                          std::to_string(PFVHCI_ABI_VERSION)};
        }
        info_.abi_version = v.AbiVersion;
        info_.driver_version = std::to_string(v.DriverMajor) + "." + std::to_string(v.DriverMinor) + "." +
                               std::to_string(v.DriverPatch) + "." + std::to_string(v.DriverBuild);
        info_.usb2_ports = v.NumUsb2Ports;
        info_.usb3_ports = v.NumUsb3Ports;
        running_ = true;
        for (std::uint32_t i = 0; i < cfg_.fetch_threads; ++i) {
            threads_.emplace_back([this] { fetch_loop(); });
        }
        log::info("VirtualUSB", "pfvhci " + info_.driver_version + " opened (" + std::to_string(info_.usb2_ports) +
                                    " USB2 + " + std::to_string(info_.usb3_ports) + " USB3 ports)");
        return {};
    }

    void close() override {
        if (!h_) return;
        running_ = false;
        CancelIoEx(h_, nullptr);  // completes the pended FETCH requests
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
        // Closing the handle makes the driver unplug every device this process owns.
        CloseHandle(h_);
        h_ = nullptr;
    }

    VhciInfo info() const override { return info_; }
    void set_sink(RequestSink sink) override { sink_ = std::move(sink); }

    Result<PluggedDevice> plug(const DescriptorSet& set, UsbSpeed speed, std::uint64_t cookie) override {
        if (!h_) return Error{ErrorCode::DriverNotInstalled, "pfvhci not open"};
        Bytes in = encode_plugin(set, speed, cookie);
        PFVHCI_PLUGIN_RESULT r{};
        DWORD got = 0;
        if (!ioctl(IOCTL_PFVHCI_PLUGIN, in.data(), static_cast<DWORD>(in.size()), &r, sizeof(r), &got) ||
            got < sizeof(r))
            return Error{ErrorCode::DriverError, "plug-in failed: " + win_error(GetLastError())};
        return PluggedDevice{r.DeviceHandle, r.PortNumber};
    }

    Status unplug(std::uint32_t handle) override {
        if (!h_) return {};
        PFVHCI_UNPLUG u{handle, 0};
        DWORD got = 0;
        if (!ioctl(IOCTL_PFVHCI_UNPLUG, &u, sizeof(u), nullptr, 0, &got))
            return {ErrorCode::DriverError, "unplug failed: " + win_error(GetLastError())};
        return {};
    }

    Status complete(const VhciCompletion& c) override {
        if (!h_) return {ErrorCode::DriverNotInstalled, "pfvhci not open"};
        thread_local Bytes buf;
        buf.clear();
        append_completion(buf, c);
        DWORD got = 0;
        if (!ioctl(IOCTL_PFVHCI_COMPLETE, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &got))
            return {ErrorCode::DriverError, "complete failed: " + win_error(GetLastError())};
        return {};
    }

private:
    // Synchronous wrapper over an overlapped DeviceIoControl.
    bool ioctl(DWORD code, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* got) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = DeviceIoControl(h_, code, in, in_len, out, out_len, got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h_, &ov, got, TRUE);
        const DWORD err = GetLastError();
        CloseHandle(ov.hEvent);
        SetLastError(err);
        return ok != FALSE;
    }

    void fetch_loop() {
        set_current_thread_name("pf-vhci-fetch");
        std::vector<std::uint8_t> buf(cfg_.fetch_buffer_size);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (running_) {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            BOOL ok = DeviceIoControl(h_, IOCTL_PFVHCI_FETCH, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()),
                                      &got, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h_, &ov, &got, TRUE);
            if (!ok) {
                const DWORD e = GetLastError();
                if (e == ERROR_MORE_DATA && got >= sizeof(PFVHCI_RECORD_HEADER)) {
                    // The next record does not fit: grow and fetch again (record stays queued).
                    const auto* hdr = reinterpret_cast<const PFVHCI_RECORD_HEADER*>(buf.data());
                    if (hdr->Size <= PFVHCI_MAX_TRANSFER_BYTES + 64 * 1024) buf.resize(hdr->Size);
                    continue;
                }
                if (!running_ || e == ERROR_OPERATION_ABORTED) break;
                log::error("VirtualUSB", "FETCH failed: " + win_error(e));
                Sleep(100);
                continue;
            }
            auto records = parse_fetch_buffer(ByteSpan(buf.data(), got));
            if (!records) {
                log::error("VirtualUSB", "malformed record stream from driver: " + records.error().describe());
                continue;
            }
            for (const auto& r : *records) {
                if (sink_) sink_(r);
            }
        }
        CloseHandle(ov.hEvent);
    }

    VhciConfig cfg_;
    HANDLE h_ = nullptr;
    VhciInfo info_;
    RequestSink sink_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
};

}  // namespace

std::unique_ptr<IVirtualHci> make_windows_vhci(const VhciConfig& cfg) { return std::make_unique<WinVhci>(cfg); }

}  // namespace pf::client

#endif  // _WIN32

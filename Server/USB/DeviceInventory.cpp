#include "USB/DeviceInventory.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <setupapi.h>
#include <usbiodef.h>
#else
#include <filesystem>
#include <fstream>
#endif

namespace pf::server {

std::string port_chain_from_location_path(const std::string& lp) {
    std::string chain;
    std::size_t pos = 0;
    while ((pos = lp.find("#USB(", pos)) != std::string::npos) {
        pos += 5;
        const std::size_t end = lp.find(')', pos);
        if (end == std::string::npos) break;
        if (!chain.empty()) chain += '.';
        chain += lp.substr(pos, end - pos);
        pos = end;
    }
    return chain;
}

std::string serial_from_instance_id(const std::string& id) {
    const std::size_t p = id.rfind('\\');
    if (p == std::string::npos) return {};
    std::string tail = id.substr(p + 1);
    // Windows generates "<n>&<hex>&<n>&<n>" ids for devices without a (unique) serial.
    if (tail.find('&') != std::string::npos) return {};
    return tail;
}

#ifdef _WIN32

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Reads a string or string-list property; for lists returns the first element.
std::string string_prop(HDEVINFO set, SP_DEVINFO_DATA& d, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    SetupDiGetDevicePropertyW(set, &d, &key, &type, nullptr, 0, &size, 0);
    if (size == 0) return {};
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (!SetupDiGetDevicePropertyW(set, &d, &key, &type, reinterpret_cast<PBYTE>(buf.data()), size, nullptr, 0))
        return {};
    if (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST) return {};
    return narrow(std::wstring(buf.c_str()));  // up to first NUL
}

std::string guid_prop(HDEVINFO set, SP_DEVINFO_DATA& d, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    GUID g{};
    if (!SetupDiGetDevicePropertyW(set, &d, &key, &type, reinterpret_cast<PBYTE>(&g), sizeof(g), nullptr, 0) ||
        type != DEVPROP_TYPE_GUID)
        return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                  g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool parse_vid_pid(const std::string& id, std::uint16_t& vid, std::uint16_t& pid) {
    auto v = id.find("VID_");
    auto p = id.find("PID_");
    if (v == std::string::npos || p == std::string::npos) return false;
    vid = static_cast<std::uint16_t>(std::strtoul(id.substr(v + 4, 4).c_str(), nullptr, 16));
    pid = static_cast<std::uint16_t>(std::strtoul(id.substr(p + 4, 4).c_str(), nullptr, 16));
    return true;
}

}  // namespace

std::vector<InventoryEntry> read_usb_inventory() {
    std::vector<InventoryEntry> out;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t idbuf[MAX_DEVICE_ID_LEN];
        if (!SetupDiGetDeviceInstanceIdW(set, &d, idbuf, MAX_DEVICE_ID_LEN, nullptr)) continue;
        InventoryEntry e;
        e.instance_id = narrow(idbuf);
        if (!parse_vid_pid(e.instance_id, e.vid, e.pid)) continue;
        e.serial = serial_from_instance_id(e.instance_id);
        e.location = string_prop(set, d, DEVPKEY_Device_LocationPaths);
        e.port_chain = port_chain_from_location_path(e.location);
        if (e.location.empty()) e.location = string_prop(set, d, DEVPKEY_Device_LocationInfo);
        e.parent_id = string_prop(set, d, DEVPKEY_Device_Parent);
        e.container_id = guid_prop(set, d, DEVPKEY_Device_ContainerId);
        e.product = string_prop(set, d, DEVPKEY_Device_BusReportedDeviceDesc);
        if (e.product.empty()) e.product = string_prop(set, d, DEVPKEY_Device_DeviceDesc);
        e.manufacturer = string_prop(set, d, DEVPKEY_Device_Manufacturer);
        e.driver = string_prop(set, d, DEVPKEY_Device_Service);
        out.push_back(std::move(e));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// --- CM_Register_Notification (Windows 8+). Declared locally because not every SDK
// (e.g. MinGW-w64) ships the declarations; layout matches cfgmgr32.h.
namespace {
using HCMNOTIFICATION_ = HANDLE;
enum CM_NOTIFY_FILTER_TYPE_ { kFilterDeviceInterface = 0 };
enum CM_NOTIFY_ACTION_ { kActionInterfaceArrival = 0, kActionInterfaceRemoval = 1 };
struct CM_NOTIFY_FILTER_ {
    DWORD cbSize;
    DWORD Flags;
    CM_NOTIFY_FILTER_TYPE_ FilterType;
    DWORD Reserved;
    union {
        struct {
            GUID ClassGuid;
        } DeviceInterface;
        struct {
            HANDLE hTarget;
        } DeviceHandle;
        struct {
            WCHAR InstanceId[200];
        } DeviceInstance;
    } u;
};
using NotifyCallback = DWORD(CALLBACK*)(HCMNOTIFICATION_, PVOID, CM_NOTIFY_ACTION_, PVOID, DWORD);
using RegisterFn = CONFIGRET(WINAPI*)(CM_NOTIFY_FILTER_*, PVOID, NotifyCallback, HCMNOTIFICATION_*);
using UnregisterFn = CONFIGRET(WINAPI*)(HCMNOTIFICATION_);

class CmNotifier final : public DeviceChangeNotifier {
public:
    explicit CmNotifier(std::function<void()> cb) : cb_(std::move(cb)) {}
    ~CmNotifier() override {
        if (handle_ && unregister_) unregister_(handle_);
        if (lib_) FreeLibrary(lib_);
    }
    bool start() {
        lib_ = LoadLibraryW(L"cfgmgr32.dll");
        if (!lib_) return false;
        auto reg = reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(lib_, "CM_Register_Notification")));
        unregister_ =
            reinterpret_cast<UnregisterFn>(reinterpret_cast<void*>(GetProcAddress(lib_, "CM_Unregister_Notification")));
        if (!reg || !unregister_) return false;
        CM_NOTIFY_FILTER_ f{};
        f.cbSize = sizeof(f);
        f.FilterType = kFilterDeviceInterface;
        f.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;
        return reg(&f, this, &CmNotifier::thunk, &handle_) == CR_SUCCESS;
    }

private:
    static DWORD CALLBACK thunk(HCMNOTIFICATION_, PVOID ctx, CM_NOTIFY_ACTION_, PVOID, DWORD) {
        static_cast<CmNotifier*>(ctx)->cb_();
        return ERROR_SUCCESS;
    }
    std::function<void()> cb_;
    HMODULE lib_ = nullptr;
    HCMNOTIFICATION_ handle_ = nullptr;
    UnregisterFn unregister_ = nullptr;
};
}  // namespace

std::unique_ptr<DeviceChangeNotifier> make_device_change_notifier(std::function<void()> cb) {
    auto n = std::make_unique<CmNotifier>(std::move(cb));
    if (!n->start()) return nullptr;
    return n;
}

#else  // Linux / other

namespace {
std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}
}  // namespace

std::vector<InventoryEntry> read_usb_inventory() {
    namespace fs = std::filesystem;
    std::vector<InventoryEntry> out;
    std::error_code ec;
    const fs::path root("/sys/bus/usb/devices");
    for (const auto& ent : fs::directory_iterator(root, ec)) {
        const std::string name = ent.path().filename().string();
        // device directories look like "1-4.2"; interfaces contain ':'; root hubs "usbN"
        if (name.find(':') != std::string::npos || name.rfind("usb", 0) == 0) continue;
        const auto dash = name.find('-');
        if (dash == std::string::npos) continue;
        InventoryEntry e;
        e.vid = static_cast<std::uint16_t>(std::strtoul(read_file(ent.path() / "idVendor").c_str(), nullptr, 16));
        e.pid = static_cast<std::uint16_t>(std::strtoul(read_file(ent.path() / "idProduct").c_str(), nullptr, 16));
        e.port_path = name;
        e.port_chain = name.substr(dash + 1);
        e.serial = read_file(ent.path() / "serial");
        e.manufacturer = read_file(ent.path() / "manufacturer");
        e.product = read_file(ent.path() / "product");
        e.instance_id = fs::canonical(ent.path(), ec).string();
        const auto dot = name.rfind('.');
        e.parent_id = dot == std::string::npos ? "usb" + name.substr(0, dash) : name.substr(0, dot);
        e.location = name;
        out.push_back(std::move(e));
    }
    return out;
}

std::unique_ptr<DeviceChangeNotifier> make_device_change_notifier(std::function<void()>) {
    return nullptr;  // libusb hotplug is used on Linux
}

#endif

}  // namespace pf::server

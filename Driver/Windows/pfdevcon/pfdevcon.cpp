// ParsFlash USB Redirector - pfdevcon
// https://parsflash.com
//
// Small device-installation helper used by the installer to manage the
// root-enumerated virtual USB host controller (pfvhci.sys, ROOT\ParsFlashVhci).
//
//   pfdevcon install <path-to-pfvhci.inf>   create the device node (if missing) and install the driver
//   pfdevcon update  <path-to-pfvhci.inf>   install a newer driver package on the existing node(s)
//   pfdevcon remove                         remove all device nodes (incl. non-present) and the driver package
//   pfdevcon status                         presence, problem code, driver version, IOCTL_PFVHCI_GET_VERSION
//
// Exit codes: 0 = success, 1 = error (or, for status, "not installed / not working"),
//             3010 = success, reboot required (ERROR_SUCCESS_REBOOT_REQUIRED).
//
// Build (MSVC):  cmake (see CMakeLists.txt) - x64 only.
// Build (MinGW): x86_64-w64-mingw32-g++-posix -std=c++20 -Wall -Wextra -municode
//                -I<repo>/Shared/Driver pfdevcon.cpp -lsetupapi -lnewdev -lcfgmgr32 -static -o pfdevcon.exe

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

#include "pfvhci_ioctl.h"

// cfg.h is not shipped with every toolchain (MinGW); the values are part of the Windows ABI.
#ifndef DN_STARTED
#define DN_STARTED 0x00000008
#endif
#ifndef DN_HAS_PROBLEM
#define DN_HAS_PROBLEM 0x00000400
#endif
#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitReboot = 3010;

constexpr wchar_t kHardwareId[] = PFVHCI_HARDWARE_ID; // L"Root\\ParsFlashVhci"
constexpr std::wstring_view kHardwareIdNeedle = L"parsflashvhci";
const GUID kInterfaceGuid = PFVHCI_INTERFACE_GUID_INIT;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::wstring ErrorText(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result;
    if (length != 0 && text != nullptr) {
        result.assign(text, length);
        while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r' || result.back() == L' ')) {
            result.pop_back();
        }
    }
    if (text != nullptr) {
        LocalFree(text);
    }
    return result;
}

void PrintError(const wchar_t* what, DWORD code)
{
    const std::wstring text = ErrorText(code);
    std::fwprintf(stderr, L"pfdevcon: %ls failed: error %lu (0x%08lX)%ls%ls\n", what, static_cast<unsigned long>(code),
                  static_cast<unsigned long>(code), text.empty() ? L"" : L": ", text.c_str());
}

// ---------------------------------------------------------------------------
// RAII wrappers
// ---------------------------------------------------------------------------

class DevInfoList {
public:
    explicit DevInfoList(HDEVINFO handle) noexcept : handle_(handle) {}
    ~DevInfoList()
    {
        if (valid()) {
            SetupDiDestroyDeviceInfoList(handle_);
        }
    }
    DevInfoList(const DevInfoList&) = delete;
    DevInfoList& operator=(const DevInfoList&) = delete;
    DevInfoList(DevInfoList&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
    DevInfoList& operator=(DevInfoList&& other) noexcept
    {
        if (this != &other) {
            if (valid()) {
                SetupDiDestroyDeviceInfoList(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }

private:
    HDEVINFO handle_;
};

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~FileHandle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

class RegKey {
public:
    explicit RegKey(HKEY key) noexcept : key_(key) {}
    ~RegKey()
    {
        if (valid()) {
            RegCloseKey(key_);
        }
    }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    [[nodiscard]] bool valid() const noexcept { return key_ != nullptr && key_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HKEY get() const noexcept { return key_; }

private:
    HKEY key_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

bool ContainsIgnoreCase(std::wstring_view haystack, std::wstring_view needle)
{
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
    return it != haystack.end();
}

bool IsElevated()
{
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
                                  0, &administrators)) {
        return false;
    }
    BOOL member = FALSE;
    if (!CheckTokenMembership(nullptr, administrators, &member)) {
        member = FALSE;
    }
    FreeSid(administrators);
    return member != FALSE;
}

bool IsWow64()
{
    BOOL wow64 = FALSE;
    return IsWow64Process(GetCurrentProcess(), &wow64) && wow64;
}

std::wstring FullPath(const wchar_t* path)
{
    const DWORD needed = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (needed == 0) {
        return {};
    }
    std::wstring result(needed, L'\0');
    const DWORD written = GetFullPathNameW(path, needed, result.data(), nullptr);
    if (written == 0 || written >= needed) {
        return {};
    }
    result.resize(written);
    return result;
}

std::wstring WindowsInfDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"C:\\Windows\\INF";
    }
    return std::wstring(buffer, length) + L"\\INF";
}

std::vector<std::wstring> GetMultiSzProperty(HDEVINFO set, SP_DEVINFO_DATA* device, DWORD property)
{
    std::vector<std::wstring> values;
    DWORD type = 0;
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, device, property, &type, nullptr, 0, &size);
    if (size == 0) {
        return values;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, device, property, &type, reinterpret_cast<PBYTE>(buffer.data()),
                                           static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), nullptr) ||
        (type != REG_MULTI_SZ && type != REG_SZ)) {
        return values;
    }
    for (const wchar_t* p = buffer.data(); *p != L'\0'; p += std::wcslen(p) + 1) {
        values.emplace_back(p);
    }
    return values;
}

bool HasOurHardwareId(HDEVINFO set, SP_DEVINFO_DATA* device)
{
    for (const std::wstring& id : GetMultiSzProperty(set, device, SPDRP_HARDWAREID)) {
        if (EqualsIgnoreCase(id, kHardwareId)) {
            return true;
        }
    }
    return false;
}

std::wstring InstanceId(HDEVINFO set, SP_DEVINFO_DATA* device)
{
    wchar_t buffer[MAX_DEVICE_ID_LEN + 1] = {};
    if (!SetupDiGetDeviceInstanceIdW(set, device, buffer, MAX_DEVICE_ID_LEN, nullptr)) {
        return L"?";
    }
    return buffer;
}

std::wstring RegString(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return buffer.data();
}

struct DriverInfo {
    std::wstring infPath;   // e.g. oem42.inf (published name in %windir%\INF)
    std::wstring version;
    std::wstring date;
    std::wstring provider;
};

DriverInfo QueryDriverInfo(HDEVINFO set, SP_DEVINFO_DATA* device)
{
    DriverInfo info;
    RegKey key(SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ));
    if (!key.valid()) {
        return info;
    }
    info.infPath = RegString(key.get(), L"InfPath");
    info.version = RegString(key.get(), L"DriverVersion");
    info.date = RegString(key.get(), L"DriverDate");
    info.provider = RegString(key.get(), L"ProviderName");
    return info;
}

// All device nodes with our hardware ID (root enumerator only).
struct DeviceSet {
    DevInfoList list{INVALID_HANDLE_VALUE};
    std::vector<SP_DEVINFO_DATA> devices;
    DWORD error = ERROR_SUCCESS;
};

DeviceSet FindDevices(bool presentOnly)
{
    DeviceSet result;
    const DWORD flags = DIGCF_ALLCLASSES | (presentOnly ? DIGCF_PRESENT : 0);
    result.list = DevInfoList(SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, flags));
    if (!result.list.valid()) {
        result.error = GetLastError();
        return result;
    }
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device = {};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(result.list.get(), index, &device)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS) {
                result.error = error;
            }
            break;
        }
        if (HasOurHardwareId(result.list.get(), &device)) {
            result.devices.push_back(device);
        }
    }
    return result;
}

bool NeedsReboot(HDEVINFO set, SP_DEVINFO_DATA* device)
{
    SP_DEVINSTALL_PARAMS_W params = {};
    params.cbSize = sizeof(params);
    if (!SetupDiGetDeviceInstallParamsW(set, device, &params)) {
        return false;
    }
    return (params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
}

// DIF_REMOVE (global scope) on one device node, present or not.
bool RemoveDeviceNode(HDEVINFO set, SP_DEVINFO_DATA* device, bool* reboot)
{
    SP_REMOVEDEVICE_PARAMS params = {};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    params.Scope = DI_REMOVEDEVICE_GLOBAL;
    params.HwProfile = 0;
    if (!SetupDiSetClassInstallParamsW(set, device, &params.ClassInstallHeader, sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_REMOVE, set, device)) {
        PrintError(L"DIF_REMOVE", GetLastError());
        return false;
    }
    if (NeedsReboot(set, device)) {
        *reboot = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// install / update
// ---------------------------------------------------------------------------

// Creates ROOT\<class>\NNNN with hardware ID Root\ParsFlashVhci and registers it (devcon "install" step 1).
bool CreateRootDeviceNode(const std::wstring& inf, DevInfoList* createdList, SP_DEVINFO_DATA* createdDevice)
{
    GUID classGuid = {};
    wchar_t className[MAX_CLASS_NAME_LEN] = {};
    if (!SetupDiGetINFClassW(inf.c_str(), &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) {
        PrintError(L"SetupDiGetINFClass", GetLastError());
        return false;
    }

    DevInfoList list(SetupDiCreateDeviceInfoList(&classGuid, nullptr));
    if (!list.valid()) {
        PrintError(L"SetupDiCreateDeviceInfoList", GetLastError());
        return false;
    }

    SP_DEVINFO_DATA device = {};
    device.cbSize = sizeof(device);
    if (!SetupDiCreateDeviceInfoW(list.get(), className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &device)) {
        PrintError(L"SetupDiCreateDeviceInfo", GetLastError());
        return false;
    }

    // REG_MULTI_SZ: "Root\ParsFlashVhci\0\0"
    std::vector<wchar_t> hardwareIds(kHardwareId, kHardwareId + std::wcslen(kHardwareId));
    hardwareIds.push_back(L'\0');
    hardwareIds.push_back(L'\0');
    if (!SetupDiSetDeviceRegistryPropertyW(list.get(), &device, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(hardwareIds.data()),
                                           static_cast<DWORD>(hardwareIds.size() * sizeof(wchar_t)))) {
        PrintError(L"SetupDiSetDeviceRegistryProperty(SPDRP_HARDWAREID)", GetLastError());
        return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, list.get(), &device)) {
        PrintError(L"DIF_REGISTERDEVICE", GetLastError());
        return false;
    }

    std::wprintf(L"pfdevcon: created device node %ls\n", InstanceId(list.get(), &device).c_str());
    *createdDevice = device;
    *createdList = std::move(list);
    return true;
}

int CmdInstall(const wchar_t* infArgument, bool updateOnly)
{
    const std::wstring inf = FullPath(infArgument);
    const DWORD attributes = inf.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(inf.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        std::fwprintf(stderr, L"pfdevcon: INF not found: %ls\n", infArgument);
        return kExitError;
    }
    if (IsWow64()) {
        std::fwprintf(stderr, L"pfdevcon: run the 64-bit pfdevcon.exe on 64-bit Windows\n");
        return kExitError;
    }
    if (!IsElevated()) {
        std::fwprintf(stderr, L"pfdevcon: administrator rights are required\n");
        return kExitError;
    }

    DeviceSet present = FindDevices(true);
    if (!present.list.valid()) {
        PrintError(L"SetupDiGetClassDevs", present.error);
        return kExitError;
    }

    DevInfoList createdList(INVALID_HANDLE_VALUE);
    SP_DEVINFO_DATA createdDevice = {};
    bool created = false;

    if (present.devices.empty()) {
        if (updateOnly) {
            std::fwprintf(stderr, L"pfdevcon: %ls is not installed; use \"pfdevcon install\"\n", kHardwareId);
            return kExitError;
        }
        if (!CreateRootDeviceNode(inf, &createdList, &createdDevice)) {
            return kExitError;
        }
        created = true;
    } else {
        std::wprintf(L"pfdevcon: %zu existing %ls device node(s), %ls driver\n", present.devices.size(), kHardwareId,
                     updateOnly ? L"updating" : L"(re)installing");
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        const DWORD error = GetLastError();
        PrintError(L"UpdateDriverForPlugAndPlayDevices", error);
        if (created) {
            // Do not leave a driverless phantom node behind.
            bool ignored = false;
            RemoveDeviceNode(createdList.get(), &createdDevice, &ignored);
        }
        return kExitError;
    }

    std::wprintf(L"pfdevcon: driver %ls from %ls\n", updateOnly ? L"updated" : L"installed", inf.c_str());
    if (reboot) {
        std::wprintf(L"pfdevcon: a reboot is required to complete the operation\n");
        return kExitReboot;
    }
    return kExitOk;
}

// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

bool FileMentionsHardwareId(const std::wstring& path)
{
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) {
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
        return false;
    }
    bytes.resize(read);

    std::wstring text;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) { // UTF-16LE INF
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            text.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
        }
    } else {
        text.reserve(bytes.size());
        for (unsigned char c : bytes) {
            text.push_back(static_cast<wchar_t>(c));
        }
    }
    return ContainsIgnoreCase(text, kHardwareIdNeedle);
}

// Published INFs (oemNN.inf) of driver packages that install ROOT\ParsFlashVhci,
// including packages whose device nodes are already gone.
void FindPublishedInfs(std::vector<std::wstring>* infs)
{
    const std::wstring directory = WindowsInfDirectory();
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((directory + L"\\oem*.inf").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = data.cFileName;
        if (FileMentionsHardwareId(directory + L"\\" + name)) {
            const bool known = std::any_of(infs->begin(), infs->end(),
                                           [&](const std::wstring& n) { return EqualsIgnoreCase(n, name); });
            if (!known) {
                infs->push_back(name);
            }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

using DiUninstallDriverWFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);

// Removes a driver package from the driver store. DiUninstallDriverW (Windows 10 1703+)
// when available, SetupUninstallOEMInfW otherwise.
bool UninstallDriverPackage(const std::wstring& publishedInf, bool* reboot)
{
    const std::wstring fullPath = WindowsInfDirectory() + L"\\" + publishedInf;

    // newdev.dll is a static import of this executable (UpdateDriverForPlugAndPlayDevicesW).
    if (HMODULE newdev = GetModuleHandleW(L"newdev.dll"); newdev != nullptr) {
        FARPROC proc = GetProcAddress(newdev, "DiUninstallDriverW");
        if (proc != nullptr) {
            auto uninstall = reinterpret_cast<DiUninstallDriverWFn>(reinterpret_cast<void*>(proc));
            BOOL needReboot = FALSE;
            if (uninstall(nullptr, fullPath.c_str(), 0, &needReboot)) {
                if (needReboot) {
                    *reboot = true;
                }
                std::wprintf(L"pfdevcon: driver package %ls removed\n", publishedInf.c_str());
                return true;
            }
            PrintError(L"DiUninstallDriver", GetLastError());
        }
    }

    if (SetupUninstallOEMInfW(publishedInf.c_str(), SUOI_FORCEDELETE, nullptr)) {
        std::wprintf(L"pfdevcon: driver package %ls removed\n", publishedInf.c_str());
        return true;
    }
    PrintError(L"SetupUninstallOEMInf", GetLastError());
    return false;
}

int CmdRemove()
{
    if (!IsElevated()) {
        std::fwprintf(stderr, L"pfdevcon: administrator rights are required\n");
        return kExitError;
    }

    DeviceSet all = FindDevices(false);
    if (!all.list.valid()) {
        PrintError(L"SetupDiGetClassDevs", all.error);
        return kExitError;
    }

    bool reboot = false;
    bool ok = true;
    std::vector<std::wstring> infs;

    for (SP_DEVINFO_DATA& device : all.devices) {
        const DriverInfo driver = QueryDriverInfo(all.list.get(), &device);
        if (!driver.infPath.empty() && (driver.infPath.size() > 3 && EqualsIgnoreCase(driver.infPath.substr(0, 3), L"oem"))) {
            const bool known = std::any_of(infs.begin(), infs.end(),
                                           [&](const std::wstring& n) { return EqualsIgnoreCase(n, driver.infPath); });
            if (!known) {
                infs.push_back(driver.infPath);
            }
        }
        const std::wstring id = InstanceId(all.list.get(), &device);
        if (RemoveDeviceNode(all.list.get(), &device, &reboot)) {
            std::wprintf(L"pfdevcon: removed device node %ls\n", id.c_str());
        } else {
            ok = false;
        }
    }

    FindPublishedInfs(&infs);

    if (all.devices.empty() && infs.empty()) {
        std::wprintf(L"pfdevcon: nothing to remove\n");
        return kExitOk;
    }

    for (const std::wstring& inf : infs) {
        if (!UninstallDriverPackage(inf, &reboot)) {
            ok = false;
        }
    }

    if (!ok) {
        return kExitError;
    }
    if (reboot) {
        std::wprintf(L"pfdevcon: a reboot is required to complete the removal\n");
        return kExitReboot;
    }
    return kExitOk;
}

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------

const wchar_t* ProblemName(ULONG problem)
{
    switch (problem) {
    case 0x01: return L"CM_PROB_NOT_CONFIGURED";
    case 0x0A: return L"CM_PROB_FAILED_START";
    case 0x12: return L"CM_PROB_REINSTALL";
    case 0x15: return L"CM_PROB_WILL_BE_REMOVED";
    case 0x16: return L"CM_PROB_DISABLED";
    case 0x18: return L"CM_PROB_DEVICE_NOT_THERE";
    case 0x1C: return L"CM_PROB_FAILED_INSTALL";
    case 0x1F: return L"CM_PROB_FAILED_ADD";
    case 0x20: return L"CM_PROB_DISABLED_SERVICE";
    case 0x25: return L"CM_PROB_FAILED_DRIVER_ENTRY";
    case 0x26: return L"CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD";
    case 0x27: return L"CM_PROB_DRIVER_FAILED_LOAD";
    case 0x2B: return L"CM_PROB_FAILED_POST_START";
    case 0x2D: return L"CM_PROB_PHANTOM";
    case 0x30: return L"CM_PROB_DRIVER_BLOCKED";
    case 0x34: return L"CM_PROB_UNSIGNED_DRIVER";
    default:   return L"see Device Manager error codes";
    }
}

// Opens the private interface and calls IOCTL_PFVHCI_GET_VERSION / GET_PORTS.
bool QueryDriverOverInterface()
{
    DevInfoList list(SetupDiGetClassDevsW(&kInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!list.valid()) {
        PrintError(L"SetupDiGetClassDevs(interface)", GetLastError());
        return false;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    if (!SetupDiEnumDeviceInterfaces(list.get(), nullptr, &kInterfaceGuid, 0, &interfaceData)) {
        std::wprintf(L"interface: not present (driver not started)\n");
        return false;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(list.get(), &interfaceData, nullptr, 0, &required, nullptr);
    if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
        PrintError(L"SetupDiGetDeviceInterfaceDetail", GetLastError());
        return false;
    }
    std::vector<unsigned char> storage(required + sizeof(wchar_t), 0);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(list.get(), &interfaceData, detail, required, nullptr, nullptr)) {
        PrintError(L"SetupDiGetDeviceInterfaceDetail", GetLastError());
        return false;
    }
    std::wprintf(L"interface: %ls\n", detail->DevicePath);

    FileHandle device(CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device.valid()) {
        const DWORD error = GetLastError();
        PrintError(L"CreateFile(interface)", error);
        if (error == ERROR_ACCESS_DENIED) {
            std::fwprintf(stderr, L"pfdevcon: the controller can only be opened by SYSTEM or an elevated administrator\n");
        }
        return false;
    }

    PFVHCI_VERSION_INFO version = {};
    DWORD returned = 0;
    if (!DeviceIoControl(device.get(), IOCTL_PFVHCI_GET_VERSION, nullptr, 0, &version, sizeof(version), &returned,
                         nullptr) ||
        returned < sizeof(version)) {
        PrintError(L"IOCTL_PFVHCI_GET_VERSION", GetLastError());
        return false;
    }
    std::wprintf(L"driver: pfvhci %u.%u.%u.%u, ABI %u (pfdevcon ABI %u)%ls\n", version.DriverMajor, version.DriverMinor,
                 version.DriverPatch, version.DriverBuild, version.AbiVersion, PFVHCI_ABI_VERSION,
                 version.AbiVersion == PFVHCI_ABI_VERSION ? L"" : L"  ** ABI MISMATCH **");
    std::wprintf(L"ports: %u USB 2.0, %u USB 3.x\n", version.NumUsb2Ports, version.NumUsb3Ports);

    // Port occupancy (informational).
    const size_t count = static_cast<size_t>(version.NumUsb2Ports) + version.NumUsb3Ports;
    std::vector<unsigned char> portBuffer(offsetof(PFVHCI_PORT_LIST, Ports) + count * sizeof(PFVHCI_PORT_ENTRY), 0);
    if (DeviceIoControl(device.get(), IOCTL_PFVHCI_GET_PORTS, nullptr, 0, portBuffer.data(),
                        static_cast<DWORD>(portBuffer.size()), &returned, nullptr)) {
        const auto* ports = reinterpret_cast<const PFVHCI_PORT_LIST*>(portBuffer.data());
        const size_t available = (returned - offsetof(PFVHCI_PORT_LIST, Ports)) / sizeof(PFVHCI_PORT_ENTRY);
        size_t used = 0;
        for (size_t i = 0; i < ports->Count && i < available; ++i) {
            const PFVHCI_PORT_ENTRY& entry = ports->Ports[i];
            if (entry.DeviceHandle != 0) {
                ++used;
                std::wprintf(L"  port %u: device %u, speed %u\n", entry.PortNumber, entry.DeviceHandle, entry.Speed);
            }
        }
        std::wprintf(L"ports in use: %zu\n", used);
    }

    return version.AbiVersion == PFVHCI_ABI_VERSION;
}

int CmdStatus()
{
    DeviceSet all = FindDevices(false);
    if (!all.list.valid()) {
        PrintError(L"SetupDiGetClassDevs", all.error);
        std::wprintf(L"pfvhci: not installed (device enumeration failed)\n");
        return kExitError;
    }
    if (all.devices.empty()) {
        std::wprintf(L"pfvhci: not installed (no %ls device node)\n", kHardwareId);
        return kExitError;
    }

    bool working = false;
    for (SP_DEVINFO_DATA& device : all.devices) {
        std::wprintf(L"device: %ls\n", InstanceId(all.list.get(), &device).c_str());

        ULONG status = 0;
        ULONG problem = 0;
        const CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
        if (cr == CR_NO_SUCH_DEVINST) {
            std::wprintf(L"  state: not present (phantom node)\n");
        } else if (cr != CR_SUCCESS) {
            std::wprintf(L"  state: unknown (CM_Get_DevNode_Status CONFIGRET 0x%lX)\n", static_cast<unsigned long>(cr));
        } else if ((status & DN_HAS_PROBLEM) != 0) {
            std::wprintf(L"  state: problem code %lu (%ls)\n", static_cast<unsigned long>(problem), ProblemName(problem));
        } else if ((status & DN_STARTED) != 0) {
            std::wprintf(L"  state: started\n");
            working = true;
        } else {
            std::wprintf(L"  state: present, not started\n");
        }

        const DriverInfo driver = QueryDriverInfo(all.list.get(), &device);
        if (driver.infPath.empty()) {
            std::wprintf(L"  driver: none installed\n");
        } else {
            std::wprintf(L"  driver: %ls %ls (%ls), %ls\n", driver.version.c_str(), driver.date.c_str(),
                         driver.provider.c_str(), driver.infPath.c_str());
        }
    }

    if (!working) {
        return kExitError;
    }
    return QueryDriverOverInterface() ? kExitOk : kExitError;
}

void Usage()
{
    std::wprintf(L"ParsFlash USB Redirector - virtual USB host controller installer (https://parsflash.com)\n"
                 L"\n"
                 L"usage:\n"
                 L"  pfdevcon install <path-to-pfvhci.inf>   create %ls (if missing) and install the driver\n"
                 L"  pfdevcon update  <path-to-pfvhci.inf>   update the driver of the existing device node(s)\n"
                 L"  pfdevcon remove                         remove the device node(s) and the driver package\n"
                 L"  pfdevcon status                         show installation state and query the driver\n"
                 L"\n"
                 L"exit codes: 0 success, 1 error / not working, 3010 reboot required\n",
                 kHardwareId);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        Usage();
        return kExitError;
    }
    const std::wstring_view command = argv[1];

    if (EqualsIgnoreCase(command, L"install") || EqualsIgnoreCase(command, L"update")) {
        if (argc != 3) {
            Usage();
            return kExitError;
        }
        return CmdInstall(argv[2], EqualsIgnoreCase(command, L"update"));
    }
    if (EqualsIgnoreCase(command, L"remove") && argc == 2) {
        return CmdRemove();
    }
    if (EqualsIgnoreCase(command, L"status") && argc == 2) {
        return CmdStatus();
    }
    if (EqualsIgnoreCase(command, L"help") || command == L"-h" || command == L"--help" || command == L"/?") {
        Usage();
        return kExitOk;
    }
    Usage();
    return kExitError;
}

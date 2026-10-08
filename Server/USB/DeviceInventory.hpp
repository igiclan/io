// OS-level USB topology/property inventory that does not require opening devices.
//   Windows: SetupAPI over GUID_DEVINTERFACE_USB_DEVICE + device properties
//            (instance id, location paths, parent, container id, bus reported
//            description, manufacturer, function driver service)
//   Linux  : /sys/bus/usb/devices
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pf::server {

struct InventoryEntry {
    std::uint16_t vid = 0, pid = 0;
    std::string port_chain;    // "4.2" (ports from the root hub, dot separated)
    std::string port_path;     // "<bus>-<chain>" when the bus number is known (Linux)
    std::string serial;
    std::string manufacturer;
    std::string product;
    std::string instance_id;   // Windows device instance id / Linux sysfs path
    std::string parent_id;
    std::string container_id;
    std::string location;      // Windows LocationPaths[0] / LocationInformation
    std::string driver;        // function driver service name
};

std::vector<InventoryEntry> read_usb_inventory();

// Notifies `cb` when USB devices arrive/leave (Windows: CM_Register_Notification).
// Returns nullptr when OS notifications are unavailable (caller polls instead).
class DeviceChangeNotifier {
public:
    virtual ~DeviceChangeNotifier() = default;
};
std::unique_ptr<DeviceChangeNotifier> make_device_change_notifier(std::function<void()> cb);

// Extracts "4.2" from "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(4)#USB(2)".
std::string port_chain_from_location_path(const std::string& location_path);
// Serial from a Windows instance id "USB\VID_18D1&PID_4EE7\28131FDH2000AB" ("" if generated).
std::string serial_from_instance_id(const std::string& instance_id);

}  // namespace pf::server

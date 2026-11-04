#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <windows.h>
namespace pbc {
enum class Kind { Mouse, Headset, Gamepad, Keyboard };
enum class Protocol { Logitech, Nova, Pangu, MchoseK99V3 };
struct Reading {
    int level = -1;
    bool charging = false, full = false;
    std::wstring error;
};
struct Device {
    std::wstring key, name, subtitle, path, interfaceName;
    USHORT vid = 0, pid = 0, page = 0, usage = 0, inSize = 0, outSize = 0, featureSize = 0;
    int slot = 1;
    Kind kind = Kind::Mouse;
    Protocol protocol = Protocol::Logitech;
    bool connected = false, registered = true, visible = true, dormant = false, queried = false;
    Reading battery;
    SYSTEMTIME updated{};
};
struct Preference {
    bool registered = true, visible = true;
    int order = 0x7fffffff;
};
struct Settings {
    int interval = 60;
    bool startAtLogin = false;
    std::map<std::wstring, Preference> devices;
    std::set<std::wstring> paused;
};
struct ScanResult {
    std::vector<Device> devices;
    std::wstring error;
};
Reading ParseLogitech(std::span<const uint8_t>, uint8_t, uint8_t, uint8_t);
Reading ParseNova(std::span<const uint8_t>);
Reading ParsePangu(std::span<const uint8_t>);
ScanResult ScanDevices(const Settings &, bool enroll);
std::wstring DeviceKey(Protocol, USHORT, int slot = 1);
Settings LoadSettings(const std::wstring &);
bool SaveSettings(const Settings &, const std::wstring &, std::wstring &);
bool SetAutostart(bool, std::wstring &);
bool IsAutostartEnabled();
bool MigrateAutostart(std::wstring &);
bool RunAutostartTests(const std::wstring &, std::wstring &);
std::wstring ExecutablePath();
bool RunParserTests(std::wstring &);
void SortDevices(std::vector<Device> &, const Settings &);
bool MoveDevice(std::vector<Device> &, Settings &, const std::wstring &, const std::wstring &, bool);
void WatchNova(const std::wstring &, HANDLE, HWND, UINT, UINT);
void PreserveReading(Device &, const Device &, bool recent);
bool WriteProbe(const std::wstring &, const ScanResult &);
} // namespace pbc

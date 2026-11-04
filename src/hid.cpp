#include "core.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <hidsdi.h>
#include <hidpi.h>
#include <iomanip>
#include <optional>
#include <setupapi.h>
#include <string_view>
namespace pbc {
namespace {
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE && h)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit operator bool() const {
        return h && h != INVALID_HANDLE_VALUE;
    }
};
struct Interface {
    std::wstring path;
    HIDD_ATTRIBUTES attr{};
    HIDP_CAPS caps{};
};
using Bytes = std::vector<uint8_t>;
std::wstring ErrorText(DWORD e) {
    wchar_t *t = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&t, 0, nullptr);
    std::wstring s = t ? t : L"设备访问失败";
    if (t)
        LocalFree(t);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n'))
        s.pop_back();
    return s;
}
std::vector<Interface> Enumerate() {
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO info = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        throw std::wstring(L"无法枚举 HID：") + ErrorText(GetLastError());
    std::vector<Interface> found;
    SP_DEVICE_INTERFACE_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0;; ++i) {
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &g, i, &d)) {
            DWORD e = GetLastError();
            SetupDiDestroyDeviceInfoList(info);
            if (e != ERROR_NO_MORE_ITEMS)
                throw std::wstring(L"HID 枚举失败：") + ErrorText(e);
            return found;
        }
        DWORD n = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &d, nullptr, 0, &n, nullptr);
        if (n < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            continue;
        std::vector<uint8_t> buffer(n);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &d, detail, n, nullptr, nullptr))
            continue;
        std::wstring path = detail->DevicePath;
        bool mchoseChannel = path.find(L"vid_3837&pid_3033&mi_01#") != std::wstring::npos;
        if (path.find(L"vid_046d") == std::wstring::npos && path.find(L"vid_1038") == std::wstring::npos &&
            path.find(L"vid_20bc") == std::wstring::npos && !mchoseChannel)
            continue;
        Handle h(CreateFileW(path.c_str(), 0, 3, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!h)
            continue;
        Interface x;
        x.path = path;
        x.attr.Size = sizeof(x.attr);
        if (!HidD_GetAttributes(h.h, &x.attr))
            continue;
        if (mchoseChannel) {
            // MCHOSE reuses 3837:3033 across keyboard models.
            wchar_t product[128]{};
            if (!HidD_GetProductString(h.h, product, sizeof(product)) ||
                std::wstring_view(product) != L"MCHOSE K99 V3 2.4G")
                continue;
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (!HidD_GetPreparsedData(h.h, &pp))
            continue;
        auto st = HidP_GetCaps(pp, &x.caps);
        HidD_FreePreparsedData(pp);
        if (st == HIDP_STATUS_SUCCESS)
            found.push_back(std::move(x));
    }
}
Bytes Transfer(HANDLE h, const Bytes *write, DWORD length, DWORD timeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        throw std::wstring(L"无法创建 I/O 事件");
    OVERLAPPED ov{};
    ov.hEvent = event.h;
    Bytes data(length);
    if (write)
        data = *write;
    DWORD count = 0;
    BOOL ok = write ? WriteFile(h, data.data(), length, &count, &ov)
                    : ReadFile(h, data.data(), length, &count, &ov);
    if (!ok) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING)
            throw ErrorText(e);
        DWORD wait = WaitForSingleObject(event.h, timeout);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &count, TRUE);
            if (wait == WAIT_TIMEOUT)
                return {};
            throw std::wstring(L"HID 等待失败");
        }
        if (!GetOverlappedResult(h, &ov, &count, FALSE))
            throw ErrorText(GetLastError());
    }
    if (write && count != length)
        throw std::wstring(L"设备未接受完整查询");
    data.resize(count);
    return data;
}
void Drain(HANDLE h, DWORD size) {
    for (int i = 0; i < 6; ++i)
        if (Transfer(h, nullptr, size, 5).empty())
            break;
}
Bytes LogiCall(HANDLE h, uint8_t slot, uint8_t f, uint8_t fn, const Bytes &args = {}) {
    Bytes q(20, 0);
    q[0] = 0x11;
    q[1] = slot;
    q[2] = f;
    static uint8_t software = 3;
    software = uint8_t(software % 15 + 1);
    q[3] = (fn << 4) | software;
    std::copy_n(args.begin(), (std::min)(args.size(), size_t(16)), q.begin() + 4);
    Transfer(h, &q, 20, 350);
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        auto r = Transfer(h, nullptr, 20, 80);
        if (r.size() < 4 || r[1] != slot)
            continue;
        if (r[2] == f && r[3] == q[3])
            return r;
        if (r.size() >= 7 && r[2] == 0xff && r[4] == f && r[5] == q[3])
            return {};
    }
    return {};
}
uint8_t Feature(HANDLE h, uint8_t slot, uint16_t id) {
    auto r = LogiCall(h, slot, 0, 0, {uint8_t(id >> 8), uint8_t(id), 0});
    return r.size() >= 7 ? r[4] : 0;
}
std::wstring LogiName(HANDLE h, uint8_t slot) {
    auto f = Feature(h, slot, 5);
    if (!f)
        return {};
    auto r = LogiCall(h, slot, f, 0);
    if (r.size() < 5 || !r[4] || r[4] > 90)
        return {};
    int n = r[4];
    std::string text;
    for (int off = 0; off < n; off += 16) {
        r = LogiCall(h, slot, f, 1, {uint8_t(off)});
        if (r.size() < 5)
            return {};
        int count = (std::min)({16, n - off, int(r.size() - 4)});
        text.append(reinterpret_cast<const char *>(r.data() + 4), count);
    }
    return std::wstring(text.begin(), text.end());
}
void Fill(Device &d, const Interface &x) {
    d.path = x.path;
    d.vid = x.attr.VendorID;
    d.pid = x.attr.ProductID;
    d.page = x.caps.UsagePage;
    d.usage = x.caps.Usage;
    d.inSize = x.caps.InputReportByteLength;
    d.outSize = x.caps.OutputReportByteLength;
    d.featureSize = x.caps.FeatureReportByteLength;
    auto a = x.path.find(L'#'), b = x.path.find(L'#', a + 1);
    if (a != std::wstring::npos)
        d.interfaceName = x.path.substr(a + 1, b - a - 1);
}
Reading ParseMchoseK99V3(std::span<const uint8_t> r) {
    // Windows prefixes the unnumbered 64-byte HID payload with report ID zero.
    if (r.size() != 65 || r[0] != 0 || r[1] != 0xaa || r[2] != 0x05 || r[5] != 56 || r[6] != 0 || r[7] != 0)
        return {};
    unsigned checksum = 0;
    for (auto byte : r.subspan(5))
        checksum += byte;
    int level = r[41] & 0x7f; // 1-byte HID prefix + 8-byte header + payload offset 32.
    if (uint8_t(checksum) != r[4] || level > 100)
        return {};
    return {.level = level, .charging = (r[41] & 0x80) != 0};
}
std::optional<MchoseLink> ParseMchoseLink(std::span<const uint8_t> r) {
    // The receiver uses the same link-loss notification for sleep and going offline.
    if (r.size() == 65 && r[0] == 0 && r[1] == 0xfa && r[2] == 0xfb && r[3] == 6 && r[4] <= 1)
        return r[4] == 0 ? MchoseLink::Down : MchoseLink::Up;
    return std::nullopt;
}
void QueryDevice(Device &d) {
    d.queried = true;
    try {
        Handle h(CreateFileW(d.path.c_str(), GENERIC_READ | GENERIC_WRITE, 3, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr));
        if (!h) {
            d.battery.error = L"接口不可访问：" + ErrorText(GetLastError());
            return;
        }
        Drain(h.h, d.inSize);
        if (d.protocol == Protocol::Logitech) {
            auto f = Feature(h.h, uint8_t(d.slot), 0x1004);
            if (f) {
                auto r = LogiCall(h.h, uint8_t(d.slot), f, 1);
                d.battery = ParseLogitech(r, uint8_t(d.slot), f, r.size() > 3 ? r[3] : 0);
            } else {
                f = (d.pid == 0xc547 || d.pid == 0xc095) ? 0 : Feature(h.h, uint8_t(d.slot), 0x1000);
                if (f) {
                    auto r = LogiCall(h.h, uint8_t(d.slot), f, 0);
                    if (r.size() >= 7 && r[0] == 0x11 && r[1] == d.slot && r[2] == f && (r[3] & 0xf0) == 0 &&
                        r[4] <= 100 && r[6] <= 4) {
                        d.battery.level = r[4];
                        d.battery.charging = r[6] == 1 || r[6] == 2 || r[6] == 4;
                        d.battery.full = r[6] == 3;
                    } else
                        d.battery.error = L"未收到有效电池回复";
                } else
                    d.battery.error = L"设备休眠或没有支持的电池功能";
            }
        } else if (d.protocol == Protocol::MchoseK99V3) {
            // Read the first 56 bytes of profile 0's function config, containing the battery byte.
            Bytes q(d.outSize, 0);
            constexpr uint8_t request[] = {0, 0x55, 0x05, 0, 0x38, 0x38};
            std::copy(std::begin(request), std::end(request), q.begin());
            d.battery.error = L"未收到有效的键盘电量回复";
            if (!Transfer(h.h, &q, d.outSize, 400).empty()) {
                auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
                while (std::chrono::steady_clock::now() < until) {
                    auto r = Transfer(h.h, nullptr, d.inSize, 100);
                    auto reading = ParseMchoseK99V3(r);
                    if (reading.level >= 0) {
                        d.battery = std::move(reading);
                        break;
                    }
                }
            }
        } else {
            Bytes q(d.outSize, 0);
            q[0] = d.protocol == Protocol::Nova ? 0 : 2;
            q[1] = d.protocol == Protocol::Nova ? 0xb0 : 0x15;
            Transfer(h.h, &q, d.outSize, 400);
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
            while (std::chrono::steady_clock::now() < until) {
                auto r = Transfer(h.h, nullptr, d.inSize, 100);
                bool match = d.protocol == Protocol::Nova
                                 ? (r.size() >= 4 && (r[0] == 0xb0 || (r[0] == 0 && r[1] == 0xb0)))
                                 : (r.size() >= 5 && r[0] == 2 && r[1] == 0x15);
                if (!match)
                    continue;
                d.battery = d.protocol == Protocol::Nova ? ParseNova(r) : ParsePangu(r);
                break;
            }
        }
        if (d.battery.level < 0 && d.battery.error.empty())
            d.battery.error = L"设备未回复，请唤醒后刷新";
        d.connected = d.battery.level >= 0;
        GetLocalTime(&d.updated);
    } catch (const std::wstring &e) {
        d.battery.error = e;
        d.connected = false;
    }
}
} // namespace
std::wstring DeviceKey(Protocol p, USHORT pid, int slot) {
    wchar_t b[64];
    swprintf_s(b, L"%s-%04X-%d",
               p == Protocol::Logitech      ? L"logi"
               : p == Protocol::Nova        ? L"nova"
               : p == Protocol::MchoseK99V3 ? L"mchose"
                                            : L"pangu",
               pid, slot);
    return b;
}
Reading ParseLogitech(std::span<const uint8_t> r, uint8_t slot, uint8_t f, uint8_t addr) {
    Reading b;
    if (r.size() < 8 || r[0] != 0x11 || r[1] != slot || r[2] != f || r[3] != addr) {
        b.error = L"无效的 HID++ 电池回复";
        return b;
    }
    if (r[4] > 100 || r[6] > 4) {
        b.error = L"电池返回异常状态";
        return b;
    }
    b.level = r[4];
    b.charging = r[6] == 1 || r[6] == 2 || r[6] == 4;
    b.full = r[6] == 3;
    return b;
}
Reading ParseNova(std::span<const uint8_t> r) {
    Reading b;
    if (r.size() > 1 && r[0] == 0 && r[1] == 0xb0)
        r = r.subspan(1);
    if (r.size() < 4 || r[0] != 0xb0 || r[2] > 100 || r[3] > 3) {
        b.error = L"无效的耳机状态回复";
        return b;
    }
    if (r[3] == 0) {
        b.error = L"耳机已关机或超出范围";
        return b;
    }
    b.level = r[2];
    b.charging = r[3] == 1 || r[3] == 2;
    return b;
}
Reading ParsePangu(std::span<const uint8_t> r) {
    Reading b;
    if (r.size() < 5 || r[0] != 2 || r[1] != 0x15 || r[2] > 100) {
        b.error = L"无效的盘古状态回复";
        return b;
    }
    b.level = r[2];
    b.charging = (r[4] & 0xf) != 0;
    return b;
}
ScanResult ScanDevices(const Settings &s, bool enroll) {
    ScanResult result;
    try {
        auto interfaces = Enumerate();
        bool hasReceiver = std::any_of(interfaces.begin(), interfaces.end(), [](auto &x) {
            return x.attr.VendorID == 0x046d && x.attr.ProductID == 0xc547;
        });
        for (const auto &x : interfaces) {
            if (hasReceiver && x.attr.VendorID == 0x046d && x.attr.ProductID == 0xc095)
                continue;
            Device d;
            bool match = false;
            if (x.attr.VendorID == 0x046d && x.caps.UsagePage == 0xff00 && x.caps.Usage == 2 &&
                x.caps.InputReportByteLength == 20 && x.caps.OutputReportByteLength == 20) {
                if (x.attr.ProductID == 0xc547 || x.attr.ProductID == 0xc095) {
                    d.name = L"G502 X PLUS";
                    d.subtitle = L"Logitech · LIGHTSPEED";
                    match = true;
                } else if (x.attr.ProductID == 0xc543 || x.attr.ProductID == 0xc09a) {
                    Handle h(CreateFileW(x.path.c_str(), GENERIC_READ | GENERIC_WRITE, 3, nullptr,
                                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
                    if (h) {
                        Drain(h.h, 20);
                        auto name = LogiName(h.h, x.attr.ProductID == 0xc09a ? 0xff : 1);
                        if (name == L"PRO 2 LIGHTSPEED" || name == L"G PRO 2 LIGHTSPEED") {
                            d.name = L"PRO 2 LIGHTSPEED";
                            d.subtitle = L"Logitech · LIGHTSPEED";
                            match = true;
                        }
                    }
                }
                d.protocol = Protocol::Logitech;
                d.kind = Kind::Mouse;
                d.slot = (x.attr.ProductID == 0xc095 || x.attr.ProductID == 0xc09a) ? 0xff : 1;
            } else if (x.attr.VendorID == 0x1038 && x.attr.ProductID == 0x227e &&
                       x.caps.UsagePage == 0xffc0 && x.caps.Usage == 1 &&
                       x.caps.InputReportByteLength == 65 && x.caps.OutputReportByteLength == 65) {
                d.name = L"Nova 7 Gen 2";
                d.subtitle = L"SteelSeries · 2.4 GHz";
                d.protocol = Protocol::Nova;
                d.kind = Kind::Headset;
                match = true;
            } else if (x.attr.VendorID == 0x20bc && x.attr.ProductID == 0x5162 &&
                       x.caps.UsagePage == 0xff80 && x.caps.Usage == 1 &&
                       x.caps.InputReportByteLength == 64 && x.caps.OutputReportByteLength == 64) {
                d.name = L"北通盘古";
                d.subtitle = L"BEITONG · BTP-PG01A";
                d.protocol = Protocol::Pangu;
                d.kind = Kind::Gamepad;
                match = true;
            } else if (x.attr.VendorID == 0x3837 && x.attr.ProductID == 0x3033 && x.caps.UsagePage == 1 &&
                       x.caps.Usage == 0 && x.caps.InputReportByteLength == 65 &&
                       x.caps.OutputReportByteLength == 65 && x.caps.FeatureReportByteLength == 0) {
                d.name = L"K99 V3";
                d.subtitle = L"MCHOSE · 2.4 GHz";
                d.protocol = Protocol::MchoseK99V3;
                d.kind = Kind::Keyboard;
                match = true;
            }
            if (!match)
                continue;
            Fill(d, x);
            d.key = DeviceKey(d.protocol, d.pid, d.slot);
            auto it = s.devices.find(d.key);
            d.registered = it != s.devices.end() ? it->second.registered : enroll;
            d.visible = it != s.devices.end() ? it->second.visible : true;
            d.connected = true;
            d.dormant = s.paused.contains(d.key);
            if (d.registered && !d.dormant)
                QueryDevice(d);
            if (d.protocol == Protocol::MchoseK99V3 && d.queried && !d.connected)
                d.dormant = true;
            result.devices.push_back(std::move(d));
        }
    } catch (const std::wstring &e) {
        result.error = e;
    }
    return result;
}
void PreserveReading(Device &current, const Device &previous, bool recent) {
    if (previous.battery.level < 0)
        return;
    if (current.dormant && !current.queried)
        current.connected = previous.connected;
    bool implausible = recent && current.queried && current.battery.level >= 0 &&
                       previous.battery.level - current.battery.level > 25;
    if (current.battery.level < 0 || implausible) {
        auto error = implausible ? std::wstring(L"读数异常，保留上次电量") : current.battery.error;
        current.battery = previous.battery;
        current.battery.error = error;
        current.updated = previous.updated;
        current.dormant = true;
    }
}
namespace {
template <class Receive> void WatchHidReports(const std::wstring &path, HANDLE stop, Receive receive) {
    Handle file(
        CreateFileW(path.c_str(), GENERIC_READ, 3, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!file)
        return;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return;
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        std::array<uint8_t, 65> data{};
        OVERLAPPED ov{};
        ov.hEvent = event.h;
        ResetEvent(event.h);
        DWORD count = 0;
        if (!ReadFile(file.h, data.data(), DWORD(data.size()), &count, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING)
                break;
            HANDLE waits[] = {stop, event.h};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(file.h, &ov);
                GetOverlappedResult(file.h, &ov, &count, TRUE);
                break;
            }
            if (!GetOverlappedResult(file.h, &ov, &count, FALSE))
                break;
        }
        if (!receive(std::span<const uint8_t>(data.data(), count)))
            break;
    }
}
} // namespace

void WatchNova(const std::wstring &path, HANDLE stop, HWND target, UINT updateMsg, UINT closedMsg) {
    Reading last;
    bool sent = false;
    WatchHidReports(path, stop, [&](auto report) {
        if (report.size() < 4 || !(report[0] == 0xb0 || (report[0] == 0 && report[1] == 0xb0)))
            return true;
        auto parsed = ParseNova(report);
        if (sent && parsed.level == last.level && parsed.charging == last.charging &&
            parsed.full == last.full && parsed.error == last.error)
            return true;
        last = parsed;
        sent = true;
        auto result = new Reading(std::move(parsed));
        if (PostMessageW(target, updateMsg, 0, reinterpret_cast<LPARAM>(result)))
            return true;
        delete result;
        return false;
    });
    PostMessageW(target, closedMsg, 0, 0);
}

void WatchMchose(const std::wstring &path, HANDLE stop, HWND target, UINT message) {
    std::optional<MchoseLink> last;
    WatchHidReports(path, stop, [&](auto report) {
        auto state = ParseMchoseLink(report);
        if (!state || state == last)
            return true;
        last = state;
        return PostMessageW(target, message, WPARAM(*state), 0) != FALSE;
    });
    PostMessageW(target, message, WPARAM(MchoseLink::Closed), 0);
}
bool RunParserTests(std::wstring &e) {
    auto c = [&](bool ok, const wchar_t *name) {
        if (!ok && e.empty())
            e = name;
    };
    std::array<uint8_t, 8> a{0x11, 1, 6, 0x1d, 73, 8, 0, 0};
    c(ParseLogitech(a, 1, 6, 0x1d).level == 73, L"logi level");
    c(ParseLogitech(a, 2, 6, 0x1d).level == -1, L"logi slot");
    auto b = a;
    b[4] = 255;
    c(ParseLogitech(b, 1, 6, 0x1d).level == -1, L"logi range");
    b = a;
    b[6] = 1;
    c(ParseLogitech(b, 1, 6, 0x1d).charging, L"logi charge");
    std::array<uint8_t, 5> n{0, 0xb0, 3, 79, 3};
    c(ParseNova(n).level == 79, L"nova prefix");
    c(ParseNova(std::span(n).subspan(1)).level == 79, L"nova normalized");
    auto m = n;
    m[4] = 0;
    c(ParseNova(m).level == -1, L"nova offline");
    m = n;
    m[4] = 2;
    c(ParseNova(m).charging, L"nova charge");
    std::array<uint8_t, 5> p{2, 0x15, 81, 0, 0};
    c(ParsePangu(p).level == 81, L"pangu level");
    auto q = p;
    q[4] = 0x51;
    c(ParsePangu(q).charging, L"pangu charge");
    q = p;
    q[1] = 0x25;
    c(ParsePangu(q).level == -1, L"pangu unrelated");
    c(ParseNova({}).level == -1 && ParsePangu({}).level == -1, L"short reports");
    // Actual 100% reply from K99 V3. Plugging in changes byte 41 and the checksum.
    std::array<uint8_t, 65> k99{0x00, 0xaa, 0x05, 0x00, 0xcf, 0x38, 0x00, 0x00, 0x00, 0x50, 0x00, 0xaa, 0xbb,
                                0x01, 0x01, 0x00, 0x01, 0x06, 0x64, 0x00, 0x00, 0x01, 0x00, 0xff, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0xff, 0x01, 0x01, 0x01, 0x00, 0x64, 0x01, 0x01, 0x01, 0xff,
                                0x00, 0x00, 0x64, 0x00, 0x02, 0x02, 0x00, 0x01, 0x01, 0x00, 0xff, 0xff, 0xff,
                                0x00, 0x00, 0x64, 0x01, 0x01, 0x01, 0xff, 0x00, 0x00, 0x10, 0x10, 0x10, 0x10};
    auto keyboard = ParseMchoseK99V3(k99);
    c(keyboard.level == 100 && !keyboard.charging, L"k99 unplugged");
    k99[41] = 0xe4;
    c(ParseMchoseK99V3(k99).level == -1, L"k99 checksum");
    k99[4] = 0x4f;
    keyboard = ParseMchoseK99V3(k99);
    c(keyboard.level == 100 && keyboard.charging, L"k99 charging at 100 percent");
    c(ParseMchoseK99V3(std::span(k99).first(41)).level == -1, L"k99 truncated");
    k99[41] = 0xff;
    k99[4] = 0x6a;
    c(ParseMchoseK99V3(k99).level == -1, L"k99 invalid battery level");
    k99[41] = 0xe4;
    k99[4] = 0x4f;
    k99[2] = 0x03;
    c(ParseMchoseK99V3(k99).level == -1, L"k99 receiver info is not battery");
    std::array<uint8_t, 65> link{0, 0xfa, 0xfb, 6, 0};
    c(ParseMchoseLink(link) == MchoseLink::Down, L"k99 link lost");
    link[4] = 1;
    c(ParseMchoseLink(link) == MchoseLink::Up, L"k99 link restored");
    link[3] = 7;
    c(!ParseMchoseLink(link) && !ParseMchoseLink(k99), L"k99 unrelated notifications");
    Device previous;
    previous.battery.level = 73;
    Device current;
    current.queried = true;
    current.battery.level = 15;
    current.battery.charging = true;
    PreserveReading(current, previous, true);
    c(current.battery.level == 73 && current.dormant && !current.battery.charging,
      L"sleep bogus charge preserved");
    current = {};
    current.dormant = true;
    PreserveReading(current, previous, true);
    c(current.battery.level == 73 && current.dormant, L"idle cache retained");
    current = {};
    current.queried = true;
    current.battery.level = 15;
    PreserveReading(current, previous, false);
    c(current.battery.level == 15 && !current.dormant, L"old cache may legitimately discharge");
    return e.empty();
}
bool WriteProbe(const std::wstring &file, const ScanResult &scan) {
    std::ofstream out(std::filesystem::path(file), std::ios::binary);
    if (!out)
        return false;
    auto utf8 = [](const std::wstring &s) {
        if (s.empty())
            return std::string{};
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
        std::string t(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), t.data(), n, nullptr, nullptr);
        return t;
    };
    out << "{\"devices\":[";
    bool first = true;
    for (const auto &d : scan.devices) {
        if (!first)
            out << ",";
        first = false;
        out << "{\"name\":\"" << utf8(d.name) << "\",\"vid\":\"" << std::hex << std::setw(4)
            << std::setfill('0') << d.vid << "\",\"pid\":\"" << std::setw(4) << d.pid << "\",\"page\":\""
            << std::setw(4) << d.page << "\",\"usage\":" << std::dec << d.usage << ",\"input\":" << d.inSize
            << ",\"output\":" << d.outSize << ",\"feature\":" << d.featureSize
            << ",\"level\":" << d.battery.level << ",\"charging\":" << (d.battery.charging ? "true" : "false")
            << ",\"connected\":" << (d.connected ? "true" : "false") << "}";
    }
    out << "],\"scan_ok\":" << (scan.error.empty() ? "true" : "false") << "}\n";
    return bool(out);
}
} // namespace pbc

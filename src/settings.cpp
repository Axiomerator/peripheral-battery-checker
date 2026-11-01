#include "core.h"
#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
namespace pbc {
std::wstring ExecutablePath() {
    std::wstring p(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
    p.resize(n);
    return p;
}
Settings LoadSettings(const std::wstring &dir) {
    Settings s;
    auto p = (std::filesystem::path(dir) / L"settings.ini").wstring();
    s.interval = GetPrivateProfileIntW(L"app", L"interval", 60, p.c_str());
    if (s.interval != 30 && s.interval != 60 && s.interval != 120 && s.interval != 300)
        s.interval = 60;
    s.startAtLogin = IsAutostartEnabled();
    std::vector<wchar_t> names(32768);
    GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), p.c_str());
    for (const wchar_t *n = names.data(); *n; n += wcslen(n) + 1) {
        if (wcsncmp(n, L"device:", 7))
            continue;
        Preference x;
        x.registered = GetPrivateProfileIntW(n, L"registered", 1, p.c_str()) != 0;
        x.visible = GetPrivateProfileIntW(n, L"visible", 1, p.c_str()) != 0;
        x.order = int(GetPrivateProfileIntW(n, L"order", INT_MAX, p.c_str()));
        if (x.order < 0)
            x.order = INT_MAX;
        s.devices.emplace(n + 7, x);
    }
    return s;
}
bool SaveSettings(const Settings &s, const std::wstring &dir, std::wstring &error) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = L"无法创建设置目录";
        return false;
    }
    auto p = std::filesystem::path(dir) / L"settings.ini", tmp = std::filesystem::path(dir) / L"settings.tmp";
    std::wstring t = L"[app]\r\ninterval=" + std::to_wstring(s.interval) + L"\r\n";
    for (auto &[key, x] : s.devices)
        t += L"\r\n[device:" + key + L"]\r\nregistered=" + (x.registered ? L"1" : L"0") + L"\r\nvisible=" +
             (x.visible ? L"1" : L"0") + L"\r\norder=" + std::to_wstring(x.order) + L"\r\n";
    {
        std::ofstream out(tmp, std::ios::binary);
        uint16_t bom = 0xfeff;
        out.write(reinterpret_cast<const char *>(&bom), 2);
        out.write(reinterpret_cast<const char *>(t.data()), std::streamsize(t.size() * 2));
        if (!out) {
            error = L"无法保存设置";
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法替换设置文件";
        return false;
    }
    return true;
}

void SortDevices(std::vector<Device> &devices, const Settings &settings) {
    auto rank = [&](const Device &d) {
        auto it = settings.devices.find(d.key);
        return it == settings.devices.end() ? INT_MAX : it->second.order;
    };
    std::stable_sort(devices.begin(), devices.end(), [&](const Device &x, const Device &y) {
        auto left = rank(x), right = rank(y);
        if (left != right)
            return left < right;
        return x.kind == y.kind ? x.key < y.key : x.kind < y.kind;
    });
}
bool MoveDevice(std::vector<Device> &devices, Settings &settings, const std::wstring &key,
                const std::wstring &target, bool after) {
    if (key == target)
        return false;
    auto source = std::find_if(devices.begin(), devices.end(), [&](auto &d) { return d.key == key; });
    auto destination = std::find_if(devices.begin(), devices.end(), [&](auto &d) { return d.key == target; });
    if (source == devices.end() || destination == devices.end())
        return false;
    auto oldIndex = source - devices.begin();
    auto newIndex = destination - devices.begin() + (after ? 1 : 0);
    if (oldIndex < newIndex)
        --newIndex;
    if (oldIndex == newIndex)
        return false;
    Device moved = std::move(*source);
    devices.erase(source);
    devices.insert(devices.begin() + newIndex, std::move(moved));
    for (size_t i = 0; i < devices.size(); ++i) {
        auto &d = devices[i];
        auto &p = settings.devices[d.key];
        p.registered = d.registered;
        p.visible = d.visible;
        p.order = int(i);
    }
    return true;
}

} // namespace pbc

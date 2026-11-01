#include "core.h"
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <wrl/client.h>

namespace pbc {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t appValue[] = L"PeripheralBattery";
constexpr wchar_t shortcutName[] = L"Peripheral Battery.lnk";
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t approvedRoot[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
struct Locations {
    std::filesystem::path link;
    std::wstring run, approved, oldApproved;
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
HRESULT LocationsForUser(Locations &out) {
    PWSTR folder = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder);
    if (SUCCEEDED(hr)) {
        out = {std::filesystem::path(folder) / shortcutName, runKey,
               std::wstring(approvedRoot) + L"StartupFolder", std::wstring(approvedRoot) + L"Run"};
    }
    CoTaskMemFree(folder);
    return hr;
}
HRESULT DeleteValue(const std::wstring &path, const wchar_t *name) {
    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &key);
    if (st == ERROR_FILE_NOT_FOUND || st == ERROR_PATH_NOT_FOUND)
        return S_OK;
    if (st == ERROR_SUCCESS) {
        st = RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
    return st == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}
bool DisabledByWindows(const std::wstring &path, const wchar_t *name) {
    // Explorer owns this optional cache. Only recognize known disabled states;
    // never invent or write a binary approval record.
    DWORD data[3]{};
    DWORD size = sizeof(data);
    return RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_BINARY, nullptr, data, &size) ==
               ERROR_SUCCESS &&
           size >= sizeof(DWORD) && (data[0] == 3 || data[0] == 7);
}
bool SamePath(const wchar_t *left, const std::filesystem::path &right) {
    auto l = std::filesystem::path(left).lexically_normal().wstring();
    auto r = right.lexically_normal().wstring();
    return CompareStringOrdinal(l.c_str(), -1, r.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool ReadLink(const std::filesystem::path &path, const std::filesystem::path &exe) {
    Apartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return false;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ)))
        return false;
    std::wstring target(32768, L'\0'), directory(32768, L'\0');
    wchar_t arguments[128]{};
    return SUCCEEDED(link->GetPath(target.data(), int(target.size()), nullptr, SLGP_RAWPATH)) &&
           SUCCEEDED(link->GetArguments(arguments, int(std::size(arguments)))) &&
           SUCCEEDED(link->GetWorkingDirectory(directory.data(), int(directory.size()))) &&
           SamePath(target.c_str(), exe) && SamePath(directory.c_str(), exe.parent_path()) &&
           wcscmp(arguments, L"--tray") == 0 && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES;
}
HRESULT WriteLink(const std::filesystem::path &path, const std::filesystem::path &exe,
                  const wchar_t *arguments = L"--tray") {
    Apartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return apartment.result;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr))
        hr = link->SetPath(exe.c_str());
    if (SUCCEEDED(hr))
        hr = link->SetArguments(arguments);
    if (SUCCEEDED(hr))
        hr = link->SetWorkingDirectory(exe.parent_path().c_str());
    if (SUCCEEDED(hr))
        hr = link->SetDescription(L"Peripheral Battery — 登录后驻留托盘");
    if (SUCCEEDED(hr))
        hr = link->SetIconLocation(exe.c_str(), 0);
    if (SUCCEEDED(hr))
        hr = link->SetShowCmd(SW_SHOWNORMAL);
    if (SUCCEEDED(hr))
        hr = link.As(&file);
    if (SUCCEEDED(hr))
        hr = file->Save(path.c_str(), TRUE);
    return hr;
}
bool Enabled(const Locations &where, const std::filesystem::path &exe) {
    return ReadLink(where.link, exe) && !DisabledByWindows(where.approved, shortcutName);
}
HRESULT RemoveLink(const std::filesystem::path &path) {
    if (DeleteFileW(path.c_str()))
        return S_OK;
    DWORD st = GetLastError();
    return st == ERROR_FILE_NOT_FOUND || st == ERROR_PATH_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}
HRESULT SetEnabled(const Locations &where, const std::filesystem::path &exe, bool enabled) {
    HRESULT hr = S_OK;
    if (enabled) {
        std::error_code ec;
        std::filesystem::create_directories(where.link.parent_path(), ec);
        if (ec)
            return HRESULT_FROM_WIN32(ec.value());
        auto temporary = where.link;
        temporary += L".tmp"; // Not a runnable Startup shortcut if interrupted before replacement.
        hr = WriteLink(temporary, exe);
        if (SUCCEEDED(hr) && !ReadLink(temporary, exe))
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (SUCCEEDED(hr) && !MoveFileExW(temporary.c_str(), where.link.c_str(),
                                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            hr = HRESULT_FROM_WIN32(GetLastError());
        RemoveLink(temporary);
        if (FAILED(hr))
            return hr;
    } else {
        hr = RemoveLink(where.link);
    }
    // An explicit switch action removes only this application's stale Explorer
    // approval record. Windows will recreate its own record when needed.
    for (auto result : {DeleteValue(where.approved, shortcutName), DeleteValue(where.run, appValue),
                        DeleteValue(where.oldApproved, appValue)}) {
        if (SUCCEEDED(hr) && FAILED(result))
            hr = result;
    }
    SHChangeNotify(enabled ? SHCNE_UPDATEITEM : SHCNE_DELETE, SHCNF_PATHW, where.link.c_str(), nullptr);
    if (SUCCEEDED(hr) && Enabled(where, exe) != enabled)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return hr;
}
std::wstring Failure(HRESULT hr) {
    wchar_t message[512]{}, code[32]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(hr), 0, message,
                   DWORD(std::size(message)), nullptr);
    swprintf_s(code, L"0x%08X", unsigned(hr));
    return L"无法更新当前用户启动项（" + std::wstring(code) + L"）。" + message;
}
bool LegacyEnabled(const Locations &where, const std::filesystem::path &exe) {
    std::wstring value(32768, L'\0');
    DWORD bytes = DWORD(value.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, where.run.c_str(), appValue, RRF_RT_REG_SZ, nullptr, value.data(),
                     &bytes) != ERROR_SUCCESS)
        return false;
    return std::wstring(value.c_str()) == L"\"" + exe.wstring() + L"\" --tray" &&
           !DisabledByWindows(where.oldApproved, appValue);
}
} // namespace

bool IsAutostartEnabled() {
    Locations where;
    return SUCCEEDED(LocationsForUser(where)) && Enabled(where, ExecutablePath());
}
bool SetAutostart(bool enabled, std::wstring &error) {
    Locations where;
    HRESULT hr = LocationsForUser(where);
    if (SUCCEEDED(hr))
        hr = SetEnabled(where, ExecutablePath(), enabled);
    error = FAILED(hr) ? Failure(hr) : L"";
    return SUCCEEDED(hr);
}
bool MigrateAutostart(std::wstring &error) {
    Locations where;
    HRESULT hr = LocationsForUser(where);
    if (SUCCEEDED(hr) && LegacyEnabled(where, ExecutablePath()))
        hr = SetEnabled(where, ExecutablePath(), true);
    error = FAILED(hr) ? Failure(hr) : L"";
    return SUCCEEDED(hr);
}

bool RunAutostartTests(const std::wstring &directory, std::wstring &error) {
    // A private folder and test-only registry keys: CTest never changes actual logon settings.
    auto root = std::filesystem::path(directory) / (L"自启 测试 " + std::to_wstring(GetCurrentProcessId()));
    auto registry =
        L"Software\\PeripheralBattery\\Tests\\Autostart-" + std::to_wstring(GetCurrentProcessId());
    Locations where{root / shortcutName, registry + L"\\Run", registry + L"\\StartupFolder",
                    registry + L"\\ApprovedRun"};
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        error = L"Cannot create startup test directory";
        return false;
    }
    std::ofstream log(std::filesystem::path(directory) / L"startup-regression.txt");
    bool pass = bool(log);
    auto check = [&](bool ok, const char *name) {
        log << (ok ? "PASS: " : "FAIL: ") << name << '\n';
        pass = pass && ok;
    };
    auto exe = root / L"Peripheral Battery 测试.exe";
    check(CopyFileW(ExecutablePath().c_str(), exe.c_str(), TRUE) != FALSE, "Unicode target with spaces");
    auto writeRegistry = [&](const std::wstring &path, const wchar_t *name, DWORD type, const void *data,
                             DWORD bytes) {
        HKEY key = nullptr;
        LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                     &key, nullptr);
        if (st == ERROR_SUCCESS) {
            st = RegSetValueExW(key, name, 0, type, static_cast<const BYTE *>(data), bytes);
            RegCloseKey(key);
        }
        return st == ERROR_SUCCESS;
    };
    auto oldCommand = L"\"" + exe.wstring() + L"\" --tray";
    check(writeRegistry(where.run, appValue, REG_SZ, oldCommand.c_str(),
                        DWORD((oldCommand.size() + 1) * sizeof(wchar_t))) &&
              LegacyEnabled(where, exe),
          "Recognize previous Run registration");
    check(SUCCEEDED(SetEnabled(where, exe, true)) && Enabled(where, exe) && !LegacyEnabled(where, exe),
          "Create/read back native shortcut and remove legacy Run value");
    check(SUCCEEDED(WriteLink(where.link, exe, L"--settings")) && !Enabled(where, exe),
          "Reject wrong startup arguments");
    check(SUCCEEDED(SetEnabled(where, exe, true)), "Repair mismatched shortcut");
    check(!ReadLink(where.link, root / L"Missing.exe"), "Reject another executable path");
    BYTE disabled[12]{3};
    check(writeRegistry(where.approved, shortcutName, REG_BINARY, disabled, sizeof(disabled)) &&
              !Enabled(where, exe),
          "Read Windows disabled state");
    check(SUCCEEDED(SetEnabled(where, exe, true)) && Enabled(where, exe),
          "Explicit enable clears this application's stale disabled record");
    check(SUCCEEDED(SetEnabled(where, exe, false)) && !Enabled(where, exe) &&
              GetFileAttributesW(where.link.c_str()) == INVALID_FILE_ATTRIBUTES,
          "Disable removes shortcut");
    check(SUCCEEDED(SetEnabled(where, exe, false)), "Repeated disable succeeds");
    auto obstacle = root / L"not-a-directory";
    {
        std::ofstream file(obstacle);
        file << "test";
    }
    auto invalid = where;
    invalid.link = obstacle / shortcutName;
    check(FAILED(SetEnabled(invalid, exe, true)), "Report shortcut write failure");
    RemoveLink(obstacle);
    RemoveLink(where.link);
    RemoveLink(exe);
    RemoveDirectoryW(root.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
    error = pass ? L"" : L"Startup regression failed; see startup-regression.txt";
    return pass;
}
} // namespace pbc

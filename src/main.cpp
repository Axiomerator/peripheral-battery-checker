#include "core.h"
#include "draw.h"
#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <condition_variable>
#include <dbt.h>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <memory>
#include <imm.h>
#include <mutex>
#include <shellapi.h>
#include <thread>
#include <windowsx.h>
using namespace pbc;
using namespace Gdiplus;
namespace {
constexpr UINT TrayMsg = WM_APP + 1, ResultMsg = WM_APP + 2, OpenMsg = WM_APP + 3, QuitMsg = WM_APP + 4,
               NovaMsg = WM_APP + 5, NovaClosed = WM_APP + 6, MchoseMsg = WM_APP + 7;
constexpr wchar_t HostClass[] = L"PBC.Host.1", ViewClass[] = L"PBC.View.1";
enum Cmd {
    Refresh = 100,
    Overview,
    SettingsTab,
    Hide,
    Minimize,
    Quit,
    Seconds30,
    Seconds60,
    Seconds120,
    Seconds300,
    Autostart,
    OpenMain,
    OpenSettings
};
// The page and every control share one back buffer, so no child control can
// erase a white background between two independently presented frames.
struct Button {
    int id = 0;
    std::wstring text;
    RectF rect;
    bool active = false, toggle = false, scrolling = false;
};
struct View {
    HWND hwnd{};
    bool mini = false, initialized = false;
    float scale = 1;
    int width = 760, height = 720;
    std::vector<Button> buttons;
    float offsets[2]{};
    int hoverId = 0, pressedId = 0, focusId = 0, wheel = 0;
    bool keyboardFocus = false, dragging = false, scrollDrag = false;
    PointF down, pointer;
    float thumbGrab = 0;
    std::wstring dragKey, dropKey;
    bool dropAfter = false;
};
struct App {
    HINSTANCE instance{};
    HWND host{};
    View main, popup;
    Settings settings, workerSettings;
    std::vector<Device> devices;
    std::wstring directory, notice;
    bool settingsTab = false, busy = true, first = true, hover = false, quitting = false;
    NOTIFYICONDATAW nid{};
    HICON tray{};
    UINT taskbarMsg{};
    HDEVNOTIFY notification{};
    HANDLE watchStop{};
    std::thread novaWatch;
    std::wstring novaPath;
    HANDLE mchoseStop{};
    std::thread mchoseWatch;
    std::wstring mchosePath;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false, request = true;
    std::map<std::wstring, ULONGLONG> activity;
    std::map<HANDLE, std::wstring> inputPaths;
    std::map<HANDLE, ULONGLONG> inputTicks;
    SYSTEMTIME updated{};
    ULONGLONG suppressClick = 0;
    std::wstring capture;
    bool capturePopup = false, verifyControls = false, captured = false;
    bool verifyLayout = false;
};
App *a = nullptr;
const GUID trayGuid = {0x836e02b4, 0xc773, 0x4d1c, {0x83, 0x2b, 0x51, 0x1d, 0x71, 0x77, 0xad, 0x49}};
LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
std::vector<const Device *> Visible() {
    std::vector<const Device *> r;
    for (auto &d : a->devices)
        if (d.registered && d.visible)
            r.push_back(&d);
    return r;
}
std::wstring TimeText() {
    if (!a->updated.wYear)
        return L"等待首次读取";
    wchar_t t[64];
    swprintf_s(t, L"更新于 %02u:%02u:%02u", a->updated.wHour, a->updated.wMinute, a->updated.wSecond);
    return t;
}
std::wstring Status(const Device &d) {
    if (!d.registered)
        return L"尚未登记";
    if (d.dormant && d.battery.error == L"读数异常，保留上次电量")
        return d.battery.error;
    if (d.dormant && d.protocol == Protocol::MchoseK99V3)
        return d.battery.level >= 0 ? L"未连接 · 保留电量" : L"未连接";
    if (d.dormant)
        return d.connected ? L"静置 · 已暂停查询" : L"休眠或未连接 · 保留上次电量";
    if (d.battery.level < 0)
        return L"暂不可用";
    if (d.battery.full)
        return L"已充满";
    return d.battery.charging ? L"正在充电" : L"电池供电";
}
void UpdatePause() {
    auto now = GetTickCount64();
    for (auto &d : a->devices) {
        if (d.kind != Kind::Mouse)
            continue;
        auto [it, inserted] = a->activity.try_emplace(d.key, now);
        bool paused = now - it->second > 15000ULL;
        if (paused)
            a->settings.paused.insert(d.key);
        else
            a->settings.paused.erase(d.key);
    }
}
void RefreshNow() {
    UpdatePause();
    a->busy = true;
    {
        std::lock_guard lock(a->mutex);
        a->workerSettings = a->settings;
        a->request = true;
    }
    a->cv.notify_one();
    if (a->main.hwnd)
        InvalidateRect(a->main.hwnd, nullptr, FALSE);
    if (a->popup.hwnd)
        InvalidateRect(a->popup.hwnd, nullptr, FALSE);
}
void Save() {
    std::wstring e;
    if (!SaveSettings(a->settings, a->directory, e))
        a->notice = e;
    {
        std::lock_guard lock(a->mutex);
        a->workerSettings = a->settings;
    }
}
void Tray(bool add = false) {
    auto old = a->tray;
    a->tray = MakeTrayIcon(a->devices, GetSystemMetrics(SM_CXSMICON));
    a->nid.hIcon = a->tray;
    a->nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    std::wstring tip = L"Peripheral Battery";
    for (auto *d : Visible()) {
        auto line = L"\n" + d->name + L"  " +
                    (d->battery.level < 0 ? L"未知" : std::to_wstring(d->battery.level) + L"%") +
                    (d->battery.charging && !d->dormant ? L" · 充电中" : L"");
        if (tip.size() + line.size() >= std::size(a->nid.szTip))
            break;
        tip += line;
    }
    wcscpy_s(a->nid.szTip, tip.c_str());
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &a->nid);
    if (add) {
        a->nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &a->nid);
    }
    if (old)
        DestroyIcon(old);
}

int Page(const View &v) {
    return !v.mini && a->settingsTab ? 1 : 0;
}
float &Offset(View &v) {
    return v.offsets[Page(v)];
}
bool CompactSettings(const View &v) {
    return !v.mini && a->settingsTab && v.height < 560;
}
RectF GeneralSettingsArea(const View &v) {
    return RectF(32, float(v.height) - 198, float(v.width) - 64, 130);
}
RectF ListArea(const View &v) {
    float top = v.mini ? 65.f : CompactSettings(v) ? 150.f : 232.f;
    float bottom = float(v.height) - (v.mini ? 64.f : 68.f);
    if (!v.mini && a->settingsTab)
        bottom = GeneralSettingsArea(v).Y - 16;
    return RectF(0, top, float(v.width), (std::max)(1.f, bottom - top));
}
std::vector<const Device *> Rows(const View &v) {
    if (v.mini || !a->settingsTab)
        return Visible();
    std::vector<const Device *> result;
    for (auto &d : a->devices)
        result.push_back(&d);
    return result;
}
float RowStep(const View &v) {
    return v.mini ? 82.f : a->settingsTab ? 84.f : 116.f;
}
float ContentHeight(const View &v) {
    if (!v.mini && a->settingsTab)
        return float(a->devices.size()) * 84;
    return Rows(v).empty() ? 144.f : float(Rows(v).size()) * RowStep(v) - (v.mini ? 10.f : 12.f);
}
float MaxOffset(const View &v) {
    return (std::max)(0.f, ContentHeight(v) - ListArea(v).Height);
}
RectF ScrollTrack(const View &v) {
    auto r = ListArea(v);
    return RectF(float(v.width) - 17, r.Y + 2, 7, r.Height - 4);
}
RectF ScrollThumb(View &v) {
    auto track = ScrollTrack(v);
    float height = (std::max)(28.f, track.Height * ListArea(v).Height / ContentHeight(v));
    height = (std::min)(height, track.Height);
    float range = MaxOffset(v);
    return RectF(track.X, track.Y + (range > 0 ? Offset(v) / range : 0) * (track.Height - height),
                 track.Width, height);
}
void SetOffset(View &v, float offset) {
    offset = (std::clamp)(offset, 0.f, MaxOffset(v));
    if (offset != Offset(v)) {
        Offset(v) = offset;
        InvalidateRect(v.hwnd, nullptr, FALSE);
    }
}
void Add(View &v, int id, std::wstring label, float x, float y, float w, float h, bool active = false,
         bool toggle = false, bool scrolling = false) {
    v.buttons.push_back({id, std::move(label), RectF(x, y, w, h), active, toggle, scrolling});
}
RectF ButtonRect(View &v, const Button &b) {
    auto r = b.rect;
    if (b.scrolling)
        r.Y -= Offset(v);
    return r;
}
int HitButton(View &v, PointF p) {
    for (auto &b : v.buttons) {
        if (b.scrolling && !ListArea(v).Contains(p))
            continue;
        if (ButtonRect(v, b).Contains(p))
            return b.id;
    }
    return 0;
}
void Layout(View &v) {
    v.buttons.clear();
    Offset(v) = (std::clamp)(Offset(v), 0.f, MaxOffset(v));
    float w = float(v.width);
    if (v.mini) {
        Add(v, OpenMain, L"控制中心", 24, float(v.height - 52), w - 126, 32);
        Add(v, Refresh, L"刷新", w - 90, float(v.height - 52), 66, 32);
    } else {
        Add(v, Minimize, L"最小化", w - 86, 10, 32, 30);
        Add(v, Hide, L"关闭到托盘", w - 48, 10, 32, 30);
        float tabY = CompactSettings(v) ? 58.f : 142.f;
        Add(v, Overview, L"设备概览", 32, tabY, 104, 34, !a->settingsTab);
        Add(v, SettingsTab, L"设置", 144, tabY, 80, 34, a->settingsTab);
        Add(v, Refresh, a->settingsTab ? L"扫描设备" : L"立即刷新", w - 146, tabY, 114, 34);
        if (a->settingsTab) {
            for (size_t i = 0; i < a->devices.size(); ++i) {
                auto &d = a->devices[i];
                float y = ListArea(v).Y + float(i) * 84;
                Add(v, 300 + int(i) * 2, d.registered ? L"已登记" : L"登记", w - 234, y + 16, 88, 32,
                    d.registered, true, true);
                Add(v, 301 + int(i) * 2, d.visible ? L"显示" : L"隐藏", w - 134, y + 16, 100, 32,
                    d.registered && d.visible, true, true);
            }
            float y = GeneralSettingsArea(v).Y;
            const int secs[] = {30, 60, 120, 300};
            const wchar_t *labels[] = {L"30 秒", L"60 秒", L"2 分钟", L"5 分钟"};
            for (int i = 0; i < 4; ++i)
                Add(v, Seconds30 + i, labels[i], w - 328 + i * 74, y + 20, 66, 30,
                    a->settings.interval == secs[i]);
            Add(v, Autostart, L"开机自启", w - 162, y + 78, 120, 32, a->settings.startAtLogin, true);
        }
    }
    if (std::none_of(v.buttons.begin(), v.buttons.end(), [&](auto &b) { return b.id == v.focusId; }))
        v.focusId = 0;
}
void Resize(View &v) {
    UINT dpi = GetDpiForWindow(v.hwnd);
    v.scale = float(dpi ? dpi : 96) / 96;
    if (!v.initialized) {
        v.initialized = true;
        MONITORINFO m{sizeof(m)};
        GetMonitorInfoW(MonitorFromWindow(v.hwnd, MONITOR_DEFAULTTONEAREST), &m);
        int preferredHeight = v.mini ? (std::min)(520, 129 + (std::max)(1, int(Visible().size())) * 82) : 720;
        int width = (std::min)(v.mini ? 396 : 760, int((m.rcWork.right - m.rcWork.left - 32) / v.scale));
        int height = (std::min)(preferredHeight, int((m.rcWork.bottom - m.rcWork.top - 32) / v.scale));
        SetWindowPos(v.hwnd, nullptr, 0, 0, int(width * v.scale), int(height * v.scale),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT client{};
    GetClientRect(v.hwnd, &client);
    v.width = int(client.right / v.scale + .5f);
    v.height = int(client.bottom / v.scale + .5f);
    Layout(v);
    InvalidateRect(v.hwnd, nullptr, FALSE);
}
void Ensure(View &v, bool mini) {
    if (v.hwnd)
        return;
    v.mini = mini;
    v.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, ViewClass, mini ? L"外设电量 · 速览" : L"Peripheral Battery · 外设电量",
        WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX, 100, 100, 760, 720, nullptr, nullptr, a->instance, &v);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(v.hwnd, 20, &dark, sizeof(dark));
    int corner = 2;
    DwmSetWindowAttribute(v.hwnd, 33, &corner, sizeof(corner));
    Resize(v);
}

void HidePopup() {
    if (a->popup.hwnd)
        ShowWindow(a->popup.hwnd, SW_HIDE);
    KillTimer(a->host, 12);
    a->hover = false;
}
void Main(bool settings) {
    HidePopup();
    a->main.hoverId = a->main.pressedId = 0;
    a->settingsTab = settings;
    if (!a->verifyLayout)
        a->settings.startAtLogin = IsAutostartEnabled();
    bool created = !a->main.hwnd;
    Ensure(a->main, false);
    Resize(a->main);
    RECT r;
    GetWindowRect(a->main.hwnd, &r);
    MONITORINFO m{sizeof(m)};
    GetMonitorInfoW(MonitorFromWindow(a->main.hwnd, MONITOR_DEFAULTTONEAREST), &m);
    int x = m.rcWork.left + (m.rcWork.right - m.rcWork.left - r.right + r.left) / 2,
        y = m.rcWork.top + (m.rcWork.bottom - m.rcWork.top - r.bottom + r.top) / 2;
    SetWindowPos(a->main.hwnd, HWND_TOP, x, y, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW | (created ? 0 : SWP_NOMOVE));
    SetForegroundWindow(a->main.hwnd);
}
void Popup(bool activate) {
    Ensure(a->popup, true);
    Resize(a->popup);
    NOTIFYICONIDENTIFIER id{sizeof(id)};
    id.hWnd = a->host;
    id.uID = 1;
    id.guidItem = trayGuid;
    RECT icon{};
    POINT p;
    GetCursorPos(&p);
    if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon))) {
        p.x = (icon.left + icon.right) / 2;
        p.y = icon.top;
    }
    MONITORINFO m{sizeof(m)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &m);
    int w = int(a->popup.width * a->popup.scale), h = int(a->popup.height * a->popup.scale);
    int x = (std::clamp)(LONG(p.x - w / 2), m.rcWork.left + 12,
                         (std::max)(m.rcWork.left + 12, m.rcWork.right - w - 12));
    int y = (std::clamp)(LONG(p.y - h - 12), m.rcWork.top + 12,
                         (std::max)(m.rcWork.top + 12, m.rcWork.bottom - h - 12));
    SetWindowPos(a->popup.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    ShowWindow(a->popup.hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    a->hover = !activate;
    if (activate)
        SetForegroundWindow(a->popup.hwnd);
    else
        SetTimer(a->host, 12, 350, nullptr);
}
void Command(int id) {

    if (id == Quit) {
        PostMessageW(a->host, WM_CLOSE, 0, 0);
        return;
    }
    if (id == Hide || id == Minimize) {
        ShowWindow(a->main.hwnd, SW_HIDE);
        return;
    }
    if (id == Overview || id == OpenMain) {
        Main(false);
        return;
    }
    if (id == SettingsTab || id == OpenSettings) {
        Main(true);
        return;
    }
    if (id == Refresh) {
        a->notice.clear();
        RefreshNow();
        return;
    }
    if (id == Autostart) {
        std::wstring e;
        bool updated = SetAutostart(!a->settings.startAtLogin, e);
        a->settings.startAtLogin = IsAutostartEnabled();
        a->notice = updated ? L"" : e;
        if (!updated)
            MessageBoxW(a->main.hwnd, e.c_str(), L"开机自启", MB_OK | MB_ICONERROR);
    } else if (id >= Seconds30 && id <= Seconds300) {
        const int choices[] = {30, 60, 120, 300};
        a->settings.interval = choices[id - Seconds30];
    } else if (id >= 300) {
        size_t i = size_t(id - 300) / 2;
        if (i >= a->devices.size())
            return;
        auto &d = a->devices[i];
        if ((id - 300) % 2 == 0) {
            d.registered = !d.registered;
            if (!d.registered) {
                d.battery = {};
                d.connected = false;
                if (d.protocol == Protocol::MchoseK99V3)
                    a->settings.paused.erase(d.key);
            }
        } else {
            if (!d.registered)
                return;
            d.visible = !d.visible;
        }
        auto &preference = a->settings.devices[d.key];
        preference.registered = d.registered;
        preference.visible = d.visible;
        RefreshNow();
    } else
        return;
    Save();
    Tray();
    if (a->main.hwnd) {
        Layout(a->main);
        InvalidateRect(a->main.hwnd, nullptr, FALSE);
    }
}
void ContextMenu() {
    HidePopup();
    auto m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, OpenMain, L"打开控制中心");
    AppendMenuW(m, MF_STRING, OpenSettings, L"设置");
    AppendMenuW(m, MF_STRING, Refresh, L"立即刷新");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Quit, L"退出 Peripheral Battery");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(a->host);
    int c = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, a->host, nullptr);
    DestroyMenu(m);
    if (c)
        PostMessageW(a->host, WM_COMMAND, c, 0);
    PostMessageW(a->host, WM_NULL, 0, 0);
}

void DrawButton(Graphics &g, const Button &b, const View &v) {
    auto saved = g.Save();
    g.TranslateTransform(b.rect.X, b.rect.Y);
    float w = b.rect.Width, h = b.rect.Height;
    bool pressed = b.id == v.pressedId && b.id == v.hoverId;
    Color fill = b.active ? Color(255, 31, 62, 58) : Surface(), ink = b.active ? Mint() : Ink();
    if (b.id == v.hoverId)
        fill = b.active ? Color(255, 38, 78, 68) : Color(255, 33, 43, 57);
    if (pressed)
        fill = Color(255, 46, 65, 77);
    if (b.id == Hide || b.id == Minimize) {
        if (b.id == v.hoverId || pressed)
            Round(g, 0, 0, w, h, 8, b.id == Hide ? Color(255, 89, 42, 51) : fill);
        if (b.id == Hide) {
            Line(g, w / 2 - 4, h / 2 - 4, w / 2 + 4, h / 2 + 4, Muted());
            Line(g, w / 2 + 4, h / 2 - 4, w / 2 - 4, h / 2 + 4, Muted());
        } else
            Line(g, w / 2 - 5, h / 2, w / 2 + 5, h / 2, Muted());
    } else {
        Round(g, .5f, .5f, w - 1, h - 1, 8, fill, b.active ? Color(255, 46, 94, 82) : Edge());
        if (b.toggle) {
            Text(g, b.text, 8, 0, w - 44, h, 11, ink);
            Round(g, w - 32, h / 2 - 7, 24, 14, 7, b.active ? Mint() : Color(255, 70, 83, 101));
            Round(g, w - (b.active ? 21.f : 30.f), h / 2 - 5, 10, 10, 5, b.active ? Bg() : Ink());
        } else
            Text(g, b.text, 4, 0, w - 8, h, 12, ink, b.active, 1);
    }
    if (v.keyboardFocus && b.id == v.focusId)
        Round(g, 2, 2, w - 4, h - 4, 6, Color(0, 0, 0, 0), Mint());
    g.Restore(saved);
}
void Grip(Graphics &g, float x, float y, Color color) {
    SolidBrush brush(color);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 2; ++column)
            g.FillEllipse(&brush, x + column * 4.f, y + row * 5.f, 2.f, 2.f);
}

RectF BatteryNumberBounds(float right, float y, bool mini) {
    return mini ? RectF(right - 110, y + 11, 82, 40) : RectF(right - 170, y + 12, 122, 60);
}

void Card(Graphics &g, const Device &d, float y, int width, bool mini) {
    float x = mini ? 16.f : 32.f, w = width - x * 2, h = mini ? 72.f : 104.f;
    Round(g, x, y, w, h, mini ? 12.f : 14.f, Surface(), Edge());
    Color c = d.dormant ? Muted() : BatteryColor(d.battery.level);
    Grip(g, x + 9, y + h / 2 - 6, Muted());
    float size = mini ? 32.f : 44.f, ix = x + 27, iy = y + (mini ? 18.f : 22.f);
    Round(g, ix - 5, iy - 5, size + 10, size + 10, 12, Color(255, 29, 46, 51));
    DeviceIcon(g, d.kind, ix, iy, size, c);
    float tx = x + (mini ? 75.f : 100.f);
    auto number = BatteryNumberBounds(x + w, y, mini);
    bool charging = d.battery.charging && !d.dormant;
    float labelWidth = mini ? number.X - tx - (charging ? 24.f : 8.f) : w - 250;
    Text(g, d.name, tx, y + (mini ? 10.f : 17.f), labelWidth, 26, mini ? 13.f : 16.f, Ink(), true);
    if (mini)
        Text(g, Status(d), tx, y + 36, labelWidth, 20, 10, Muted());
    else {
        Text(g, d.subtitle, tx, y + 47, w - 250, 20, 11, Muted());
        auto status = Status(d);
        if (d.battery.level < 0 && !d.battery.error.empty() && !d.dormant)
            status = d.battery.error;
        Text(g, status, tx, y + 72, w - 270, 18, 10, d.battery.level < 0 ? Muted() : c);
        Round(g, x + w - 184, y + 80, 160, 4, 2, Color(255, 43, 55, 69));
        if (d.battery.level >= 0)
            Round(g, x + w - 184, y + 80, 160 * d.battery.level / 100.f, 4, 2, c);
    }
    auto value = d.battery.level < 0 ? L"—" : std::to_wstring(d.battery.level);
    Text(g, value, number.X, number.Y, number.Width, number.Height, mini ? 28.f : 44.f, c, true, 2);
    if (d.battery.level >= 0)
        Text(g, L"%", x + w - (mini ? 28.f : 44.f), y + (mini ? 26.f : 37.f), 24, 24, mini ? 12.f : 16.f,
             Muted());
    if (charging)
        Bolt(g, number.X - 18, y + 26, 16, c);
}

void DrawFrame(Graphics &g, View &v) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Bg());
    g.ScaleTransform(v.scale, v.scale);
    float w = float(v.width), h = float(v.height);
    auto list = ListArea(v);
    auto rows = Rows(v);
    if (v.mini) {
        Brand(g, 20, 19, 28);
        Text(g, L"外设电量", 60, 16, 180, 24, 16, Ink(), true);
        Text(g, a->busy ? L"正在读取" : L"LIVE", w - 112, 21, 88, 18, 10, Mint(), true, 2);
        Line(g, 16, h - 64, w - 16, h - 64, Edge(), 1);
    } else {
        Brand(g, 28, 15, 25);
        Text(g, L"PERIPHERAL BATTERY", 64, 12, w - 184, 30, 11, Muted(), true);
        if (!CompactSettings(v)) {
            Text(g, a->settingsTab ? L"把电量，留在视线里。" : L"所有设备，一眼掌握。", 32, 62, w - 64, 44,
                 27, Ink(), true);
            Text(g, a->settingsTab ? L"登记需要监测的设备，按自己的方式显示。" : L"无线连接，清晰可见。", 34,
                 110, w - 68, 22, 12, Muted());
        }
        float dividerY = CompactSettings(v) ? 110.f : 192.f;
        Line(g, 32, dividerY, w - 32, dividerY, Edge(), 1);
        int online = 0;
        for (auto *d : rows)
            if (d->battery.level >= 0 && !d->dormant)
                ++online;
        Text(g,
             a->settingsTab
                 ? L"拖动设备调整顺序"
                 : L"已连接 " + std::to_wstring(online) + L" / " + std::to_wstring(rows.size()) + L" 台设备",
             34, dividerY + 7, w - 340, 20, 10, Muted());
        Text(g, L"拖动卡片排序 · 前三台显示在托盘", w - 320, dividerY + 7, 286, 20, 10, Muted(), false, 2);
        Line(g, 32, h - 53, w - 32, h - 53, Edge(), 1);
        Text(g, a->notice.empty() ? (a->busy ? L"正在读取设备…" : TimeText()) : a->notice, 34, h - 43,
             w - 230, 27, 10, a->notice.empty() ? Muted() : Color(255, 255, 161, 125));
        Text(g, L"轻量常驻  /  " + std::to_wstring(a->settings.interval) + L"s", w - 220, h - 43, 184, 27, 10,
             Muted(), false, 2);
    }
    auto saved = g.Save();
    g.SetClip(list);
    g.TranslateTransform(0, -Offset(v));
    if (!v.mini && a->settingsTab) {
        for (size_t i = 0; i < rows.size(); ++i) {
            auto &d = *rows[i];
            float y = list.Y + float(i) * 84;
            if (y + 78 < list.Y + Offset(v) || y > list.GetBottom() + Offset(v))
                continue;
            if (v.dragging && d.key == v.dragKey)
                Round(g, 32, y, w - 64, 76, 10, Color(255, 24, 43, 44), Edge());
            Grip(g, 42, y + 29, Muted());
            DeviceIcon(g, d.kind, 60, y + 16, 32, d.registered ? Mint() : Muted());
            Text(g, d.name, 110, y + 7, w - 364, 24, 14, Ink(), true);
            wchar_t id[70];
            swprintf_s(id, L"%04X:%04X · %s", d.vid, d.pid,
                       d.dormant && d.protocol != Protocol::MchoseK99V3 ? L"静置 / 休眠"
                       : d.connected                                    ? L"已连接"
                                                                        : L"未连接");
            Text(g, id, 110, y + 36, w - 364, 20, 10, Muted());
            Line(g, 40, y + 78, w - 40, y + 78, Edge(), 1);
        }
    } else {
        if (rows.empty()) {
            Text(g, v.mini ? L"前往设置登记并显示设备" : L"让你的设备出现在这里", 24, list.Y + 32, w - 48, 40,
                 v.mini ? 13.f : 21.f, Ink(), true, 1);
            if (!v.mini)
                Text(g, L"打开设置，登记设备并开启显示。", 40, list.Y + 80, w - 80, 26, 13, Muted(), false,
                     1);
        }
        for (size_t i = 0; i < rows.size(); ++i) {
            float y = list.Y + float(i) * RowStep(v);
            float rowHeight = v.mini ? 72.f : 104.f;
            if (y + rowHeight < list.Y + Offset(v) || y > list.Y + Offset(v) + list.Height)
                continue;
            Card(g, *rows[i], y, v.width, v.mini);
            if (v.dragging && rows[i]->key == v.dragKey)
                Round(g, v.mini ? 16.f : 32.f, y, w - (v.mini ? 32.f : 64.f), rowHeight, v.mini ? 12.f : 14.f,
                      Color(0, 0, 0, 0), Mint());
        }
    }
    for (auto &b : v.buttons)
        if (b.scrolling && b.rect.GetBottom() >= list.Y + Offset(v) &&
            b.rect.Y <= list.GetBottom() + Offset(v))
            DrawButton(g, b, v);
    if (v.dragging && !v.dropKey.empty()) {
        auto it = std::find_if(rows.begin(), rows.end(), [&](auto *d) { return d->key == v.dropKey; });
        if (it != rows.end()) {
            float y = list.Y + float(it - rows.begin()) * RowStep(v);
            y += v.dropAfter ? RowStep(v) - 4 : 2;
            Line(g, v.mini ? 20.f : 36.f, y, w - (v.mini ? 20.f : 36.f), y, Mint(), 3);
        }
    }
    g.Restore(saved);
    if (!v.mini && a->settingsTab) {
        float y = GeneralSettingsArea(v).Y;
        Round(g, 32, y, w - 64, 130, 14, Surface(), Edge());
        Text(g, L"刷新间隔", 48, y + 16, 170, 26, 13, Ink(), true);
        Text(g, L"后台按需唤醒，界面即时呈现", 48, y + 45, w - 380, 20, 10, Muted());
        Line(g, 48, y + 72, w - 48, y + 72, Edge(), 1);
        Text(g, L"随 Windows 启动", 48, y + 80, 240, 26, 13, Ink(), true);
        Text(g, L"启动后安静驻留托盘", 48, y + 105, 240, 18, 10, Muted());
    }
    if (MaxOffset(v) > 0) {
        auto track = ScrollTrack(v), thumb = ScrollThumb(v);
        Round(g, track.X + 2, track.Y, 3, track.Height, 1.5f, Color(255, 30, 41, 53));
        Round(g, thumb.X + 1, thumb.Y, 5, thumb.Height, 2.5f,
              v.scrollDrag ? Mint() : Color(255, 83, 103, 119));
    }
    for (auto &b : v.buttons)
        if (!b.scrolling)
            DrawButton(g, b, v);
    // Subtle resize affordance, kept inside the dark client surface.
    Line(g, w - 13, h - 6, w - 6, h - 13, Edge(), 1);
    Line(g, w - 8, h - 6, w - 6, h - 8, Edge(), 1);
}
void Paint(View &v) {
    PAINTSTRUCT ps;
    auto dc = BeginPaint(v.hwnd, &ps);
    RECT cr;
    GetClientRect(v.hwnd, &cr);
    if (cr.right > 0 && cr.bottom > 0) {
        Bitmap bmp(cr.right, cr.bottom, PixelFormat32bppPARGB);
        {
            Graphics g(&bmp);
            DrawFrame(g, v);
        }
        Graphics out(dc);
        out.DrawImage(&bmp, 0, 0);
    }
    EndPaint(v.hwnd, &ps);
}

void Worker() {
    a->workerSettings = a->settings;
    a->worker = std::thread([] {
        bool initial = true;
        for (;;) {
            Settings s;
            bool enroll;
            {
                std::unique_lock lock(a->mutex);
                if (!a->request) {
                    int delay = a->workerSettings.interval;
                    a->cv.wait_for(lock, std::chrono::seconds(delay), [] { return a->stop || a->request; });
                }
                if (a->stop)
                    return;
                a->request = false;
                s = a->workerSettings;
                enroll = initial && a->first;
            }
            auto r = std::make_unique<ScanResult>();
            try {
                *r = ScanDevices(s, enroll);
            } catch (...) {
                r->error = L"设备扫描失败，请重试";
            }
            initial = false;
            if (!PostMessageW(a->host, ResultMsg, 0, (LPARAM)r.get()))
                return;
            r.release();
        }
    });
}
bool Capture(const std::wstring &path, HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    Bitmap bmp(rect.right, rect.bottom, PixelFormat32bppARGB);
    Graphics graphics(&bmp);
    auto dc = graphics.GetHDC();
    BOOL ok = PrintWindow(window, dc, 3);
    graphics.ReleaseHDC(dc);
    CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    return ok && bmp.Save(path.c_str(), &png, nullptr) == Ok;
}
void VerifyControls() {
    bool pass = true;
    auto original = a->settings;
    bool originalTab = std::any_of(a->main.buttons.begin(), a->main.buttons.end(),
                                   [](auto &b) { return b.id == SettingsTab; });
    if (!a->devices.empty()) {
        auto key = a->devices[0].key;
        bool registered = a->devices[0].registered, visible = a->devices[0].visible;
        Command(301);
        auto loaded = LoadSettings(a->directory);
        pass = pass && (loaded.devices[key].visible != visible);
        Command(301);
        Command(300);
        loaded = LoadSettings(a->directory);
        pass = pass && (loaded.devices[key].registered != registered);
        Command(300);
    }
    Command(Seconds120);
    pass = pass && LoadSettings(a->directory).interval == 120;
    if (originalTab)
        pass = pass && std::any_of(a->main.buttons.begin(), a->main.buttons.end(),
                                   [](auto &b) { return b.id == SettingsTab; });
    pass = pass && GetWindow(a->main.hwnd, GW_CHILD) == nullptr;
    a->settings = original;
    for (auto &d : a->devices) {
        auto it = original.devices.find(d.key);
        if (it != original.devices.end()) {
            d.registered = it->second.registered;
            d.visible = it->second.visible;
        }
    }
    Save();
    a->notice = pass ? L"设置持久化验证通过" : L"设置验证失败";
    std::filesystem::path file = std::filesystem::path(a->directory).parent_path() / L"ui-actions.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const char *text = pass ? "PASS: visibility, registration, interval persistence, single-canvas "
                                  "controls and restoration\n"
                                : "FAIL\n";
        DWORD n;
        WriteFile(h, text, DWORD(strlen(text)), &n, nullptr);
        CloseHandle(h);
    }
    Main(false);
    RefreshNow();
}
void StopMchoseWatch() {
    if (a->mchoseStop) {
        SetEvent(a->mchoseStop);
        a->mchoseWatch.join();
        CloseHandle(a->mchoseStop);
        a->mchoseStop = nullptr;
    }
    a->mchosePath.clear();
    // A closed receiver may have queued a notification just before shutdown.
    MSG pending{};
    while (PeekMessageW(&pending, a->host, MchoseMsg, MchoseMsg, PM_REMOVE)) {
    }
}

void Apply(ScanResult &r) {
    a->busy = false;
    a->notice = r.error;
    auto previous = a->devices;
    bool preferencesChanged = false, pauseChanged = false;
    for (auto &d : r.devices) {
        auto old = std::find_if(previous.begin(), previous.end(), [&](auto &x) { return x.key == d.key; });
        if (d.protocol == Protocol::MchoseK99V3 && a->settings.paused.contains(d.key)) {
            // A query already in flight may finish after the link-loss notification.
            d.battery = {};
            d.dormant = true;
            d.queried = false;
            d.connected = false;
        }
        if (old != previous.end()) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            FILETIME t1{}, t2{};
            SystemTimeToFileTime(&now, &t1);
            SystemTimeToFileTime(&old->updated, &t2);
            ULARGE_INTEGER u1{}, u2{};
            u1.LowPart = t1.dwLowDateTime;
            u1.HighPart = t1.dwHighDateTime;
            u2.LowPart = t2.dwLowDateTime;
            u2.HighPart = t2.dwHighDateTime;
            bool recent =
                old->updated.wYear && u1.QuadPart >= u2.QuadPart && u1.QuadPart - u2.QuadPart < 3000000000ULL;
            PreserveReading(d, *old, recent);
        }
        if (d.dormant && (d.kind == Kind::Mouse || d.kind == Kind::Headset))
            a->settings.paused.insert(d.key);
        auto p = a->settings.devices.find(d.key);
        if (p != a->settings.devices.end()) {
            d.registered = p->second.registered;
            d.visible = p->second.visible;
        } else {
            a->settings.devices[d.key] = {d.registered, d.visible};
            preferencesChanged = true;
        }
    }
    for (auto &d : previous)
        if (std::none_of(r.devices.begin(), r.devices.end(), [&](auto &x) { return x.key == d.key; })) {
            d.connected = false;
            d.dormant = true;
            d.battery.error = L"休眠或接收器未连接";
            if (d.protocol == Protocol::MchoseK99V3 && r.error.empty()) {
                d.path.clear();
                pauseChanged |= a->settings.paused.erase(d.key) > 0;
            }
            r.devices.push_back(d);
        }
    a->devices = std::move(r.devices);
    SortDevices(a->devices, a->settings);
    auto keyboard = std::find_if(a->devices.begin(), a->devices.end(), [](const auto &d) {
        return d.protocol == Protocol::MchoseK99V3 && d.registered && !d.path.empty();
    });
    const std::wstring path = keyboard == a->devices.end() ? L"" : keyboard->path;
    if (path != a->mchosePath) {
        StopMchoseWatch();
        if (!path.empty()) {
            a->mchoseStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (a->mchoseStop) {
                a->mchosePath = path;
                a->mchoseWatch = std::thread(WatchMchose, path, a->mchoseStop, a->host, MchoseMsg);
            }
        }
    }
    for (auto &d : a->devices)
        if (d.protocol == Protocol::Nova && d.registered && d.connected && !d.path.empty() &&
            a->novaPath.empty()) {
            a->watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (a->watchStop) {
                a->novaPath = d.path;
                a->novaWatch = std::thread(WatchNova, d.path, a->watchStop, a->host, NovaMsg, NovaClosed);
            }
            break;
        }
    GetLocalTime(&a->updated);
    if (preferencesChanged)
        Save();
    else if (pauseChanged) {
        std::lock_guard lock(a->mutex);
        a->workerSettings = a->settings;
    }
    Tray();
    if (a->main.hwnd)
        Resize(a->main);
    if (a->popup.hwnd)
        Resize(a->popup);
    if (a->verifyControls) {
        a->verifyControls = false;
        VerifyControls();
    }
    if (!a->capture.empty() && !a->captured) {
        a->captured = true;
        if (a->capturePopup)
            Popup(true);
        SetTimer(a->host, 20, 400, nullptr);
    }
}

void CancelPointer(View &v) {
    KillTimer(v.hwnd, 31);
    v.dragKey.clear();
    v.dropKey.clear();
    v.dragging = v.scrollDrag = false;
    v.pressedId = 0;
    if (GetCapture() == v.hwnd)
        ReleaseCapture();
    InvalidateRect(v.hwnd, nullptr, FALSE);
}
int HitRow(View &v, PointF point) {
    auto area = ListArea(v);
    float margin = v.mini ? 16.f : 32.f;
    if (!area.Contains(point) || point.X < margin || point.X >= v.width - margin)
        return -1;
    float y = point.Y - area.Y + Offset(v);
    int index = int(y / RowStep(v));
    auto rows = Rows(v);
    if (index < 0 || index >= int(rows.size()))
        return -1;
    float rowHeight = v.mini ? 72.f : a->settingsTab ? 78.f : 104.f;
    return y - index * RowStep(v) <= rowHeight ? index : -1;
}
void DragTarget(View &v, PointF point) {
    auto rows = Rows(v);
    if (rows.empty())
        return;
    auto area = ListArea(v);
    float y = (std::clamp)(point.Y, area.Y, area.GetBottom()) - area.Y + Offset(v);
    int index = (std::clamp)(int(y / RowStep(v)), 0, int(rows.size()) - 1);
    v.dropKey = rows[index]->key;
    v.dropAfter = y >= (float(index) + .5f) * RowStep(v);
}
void PointerMove(View &v, PointF point) {
    v.pointer = point;
    int hover = HitButton(v, point);
    bool changed = hover != v.hoverId;
    v.hoverId = hover;
    if (v.scrollDrag) {
        auto track = ScrollTrack(v), thumb = ScrollThumb(v);
        float travel = track.Height - thumb.Height;
        SetOffset(v, travel > 0 ? (point.Y - track.Y - v.thumbGrab) * MaxOffset(v) / travel : 0);
    } else if (!v.dragKey.empty()) {
        if (!v.dragging && (std::abs(point.X - v.down.X) > 4 || std::abs(point.Y - v.down.Y) > 4)) {
            v.dragging = true;
            SetTimer(v.hwnd, 31, 40, nullptr);
        }
        if (v.dragging) {
            DragTarget(v, point);
            changed = true;
        }
    }
    if (changed)
        InvalidateRect(v.hwnd, nullptr, FALSE);
}
void PointerDown(View &v, PointF point) {
    if (v.mini) {
        a->hover = false;
        KillTimer(a->host, 12);
    }
    SetFocus(v.hwnd);
    v.keyboardFocus = false;
    v.down = v.pointer = point;
    if (MaxOffset(v) > 0) {
        auto track = ScrollTrack(v), thumb = ScrollThumb(v);
        RectF hit(track.X - 4, track.Y, track.Width + 8, track.Height);
        if (hit.Contains(point)) {
            v.scrollDrag = true;
            v.thumbGrab = thumb.Contains(point) ? point.Y - thumb.Y : thumb.Height / 2;
            SetCapture(v.hwnd);
            PointerMove(v, point);
            return;
        }
    }
    v.pressedId = v.hoverId = HitButton(v, point);
    if (v.pressedId) {
        v.focusId = v.pressedId;
        SetCapture(v.hwnd);
    } else {
        int row = HitRow(v, point);
        if (row >= 0) {
            v.dragKey = Rows(v)[row]->key;
            SetCapture(v.hwnd);
        }
    }
    InvalidateRect(v.hwnd, nullptr, FALSE);
}
void PointerUp(View &v, PointF point) {
    int command = !v.dragging && v.pressedId == HitButton(v, point) ? v.pressedId : 0;
    bool drop = v.dragging;
    auto source = v.dragKey, target = v.dropKey;
    bool after = v.dropAfter;
    CancelPointer(v);
    if (drop && MoveDevice(a->devices, a->settings, source, target, after)) {
        Save();
        Tray();
        a->main.focusId = a->popup.focusId = 0;
        if (a->main.hwnd) {
            Layout(a->main);
            InvalidateRect(a->main.hwnd, nullptr, FALSE);
        }
        if (a->popup.hwnd) {
            Layout(a->popup);
            InvalidateRect(a->popup.hwnd, nullptr, FALSE);
        }
    } else if (command)
        Command(command);
}
void FocusNext(View &v, bool backwards) {
    if (v.buttons.empty())
        return;
    auto it = std::find_if(v.buttons.begin(), v.buttons.end(), [&](auto &b) { return b.id == v.focusId; });
    int index = it == v.buttons.end() ? (backwards ? 0 : -1) : int(it - v.buttons.begin());
    index = (index + (backwards ? -1 : 1) + int(v.buttons.size())) % int(v.buttons.size());
    auto &button = v.buttons[index];
    v.focusId = button.id;
    v.keyboardFocus = true;
    if (button.scrolling) {
        auto list = ListArea(v);
        float top = button.rect.Y - Offset(v), bottom = top + button.rect.Height;
        if (top < list.Y)
            SetOffset(v, button.rect.Y - list.Y);
        else if (bottom > list.GetBottom())
            SetOffset(v, button.rect.GetBottom() - list.GetBottom());
    }
    InvalidateRect(v.hwnd, nullptr, FALSE);
}
LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto v = reinterpret_cast<View *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        v = static_cast<View *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)v);
        v->hwnd = h;
    }
    if (!v)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_PAINT:
        Paint(*v);
        return 0;
    case WM_PRINTCLIENT: {
        RECT rect{};
        GetClientRect(h, &rect);
        Bitmap frame(rect.right, rect.bottom, PixelFormat32bppPARGB);
        {
            Graphics g(&frame);
            DrawFrame(g, *v);
        }
        Graphics out(reinterpret_cast<HDC>(w));
        out.DrawImage(&frame, 0, 0);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED)
            Resize(*v);
        return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO *>(l);
        auto dpi = GetDpiForWindow(h);
        float scale = float(dpi ? dpi : 96) / 96;
        info->ptMinTrackSize = {LONG((v->mini ? 320 : 640) * scale), LONG((v->mini ? 240 : 420) * scale)};
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor);
        info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                               monitor.rcWork.top - monitor.rcMonitor.top};
        info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                           monitor.rcWork.bottom - monitor.rcWork.top};
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED)
            Command(LOWORD(w));
        return 0;
    case WM_LBUTTONDOWN:
        PointerDown(*v, PointF(GET_X_LPARAM(l) / v->scale, GET_Y_LPARAM(l) / v->scale));
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        PointerMove(*v, PointF(GET_X_LPARAM(l) / v->scale, GET_Y_LPARAM(l) / v->scale));
        return 0;
    }
    case WM_LBUTTONUP:
        PointerUp(*v, PointF(GET_X_LPARAM(l) / v->scale, GET_Y_LPARAM(l) / v->scale));
        return 0;
    case WM_MOUSELEAVE:
        if (!GetCapture()) {
            v->hoverId = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        CancelPointer(*v);
        return 0;
    case WM_TIMER:
        if (w == 31 && v->dragging) {
            auto list = ListArea(*v);
            if (v->pointer.Y < list.Y + 28)
                SetOffset(*v, Offset(*v) - 14);
            else if (v->pointer.Y > list.GetBottom() - 28)
                SetOffset(*v, Offset(*v) + 14);
            DragTarget(*v, v->pointer);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        v->wheel += GET_WHEEL_DELTA_WPARAM(w);
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        int ticks = v->wheel / WHEEL_DELTA;
        v->wheel %= WHEEL_DELTA;
        float step = lines == WHEEL_PAGESCROLL ? ListArea(*v).Height : float(lines) * 22;
        SetOffset(*v, Offset(*v) - ticks * step);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_TAB) {
            FocusNext(*v, GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        if ((w == VK_RETURN || w == VK_SPACE) && v->focusId && !(l & (1LL << 30))) {
            v->pressedId = v->hoverId = v->focusId;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_PRIOR || w == VK_NEXT) {
            SetOffset(*v, Offset(*v) + ListArea(*v).Height * (w == VK_PRIOR ? -.85f : .85f));
            return 0;
        }
        if (w == VK_HOME || w == VK_END) {
            SetOffset(*v, w == VK_HOME ? 0 : MaxOffset(*v));
            return 0;
        }
        if (w == VK_UP || w == VK_DOWN) {
            SetOffset(*v, Offset(*v) + (w == VK_UP ? -40.f : 40.f));
            return 0;
        }
        if (w == VK_ESCAPE) {
            if (GetCapture() == h)
                CancelPointer(*v);
            else
                ShowWindow(h, SW_HIDE);
            return 0;
        }
        break;
    case WM_KEYUP:
        if ((w == VK_RETURN || w == VK_SPACE) && v->pressedId) {
            auto command = v->pressedId;
            v->pressedId = 0;
            InvalidateRect(h, nullptr, FALSE);
            Command(command);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        v->keyboardFocus = false;
        v->pressedId = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(h, &point);
            PointF p(point.x / v->scale, point.y / v->scale);
            SetCursor(LoadCursorW(nullptr, HitButton(*v, p)                      ? IDC_HAND
                                           : (v->dragging || HitRow(*v, p) >= 0) ? IDC_SIZEALL
                                                                                 : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        RECT rect{};
        GetClientRect(h, &rect);
        int edge = int(6 * v->scale);
        bool left = p.x < edge, right = p.x >= rect.right - edge;
        bool top = p.y < edge, bottom = p.y >= rect.bottom - edge;
        if (!IsZoomed(h)) {
            if (top && left)
                return HTTOPLEFT;
            if (top && right)
                return HTTOPRIGHT;
            if (bottom && left)
                return HTBOTTOMLEFT;
            if (bottom && right)
                return HTBOTTOMRIGHT;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
            if (top)
                return HTTOP;
            if (bottom)
                return HTBOTTOM;
        }
        if (p.y < int((v->mini ? 55 : 48) * v->scale) &&
            p.x < int((v->width - (v->mini ? 0 : 100)) * v->scale))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ACTIVATE:
        if (!v->mini && LOWORD(w) != WA_INACTIVE && !a->verifyLayout) {
            a->settings.startAtLogin = IsAutostartEnabled();
            Layout(*v);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (v->mini && LOWORD(w) == WA_INACTIVE && !a->hover)
            HidePopup();
        break;
    case WM_CLOSE:

        CancelPointer(*v);
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DPICHANGED: {
        auto rect = reinterpret_cast<RECT *>(l);
        SetWindowPos(h, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Resize(*v);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

bool VerifyLayout() {
    std::vector<std::string> checks;
    bool pass = true;
    auto check = [&](bool result, const char *name) {
        checks.push_back(std::string(result ? "PASS " : "FAIL ") + name);
        pass = pass && result;
    };
    auto originalSettings = a->settings;
    auto originalDevices = a->devices;
    a->settings.devices.clear();
    a->devices.clear();
    for (int i = 0; i < 12; ++i) {
        Device d;
        d.key = L"ui-fixture-" + std::to_wstring(i);
        d.name = L"测试设备 " + std::to_wstring(i + 1);
        d.subtitle = L"布局验证 · 非真实设备";
        d.kind = Kind(i % 4);
        d.battery.level = i == 0 || i % 4 == 3 ? 100 : 28 + i * 5;
        d.battery.charging = i == 0 || i % 4 == 3;
        d.connected = true;
        a->settings.devices[d.key] = {true, true, i};
        a->devices.push_back(std::move(d));
    }
    auto &keyboard = a->devices[3];
    keyboard.protocol = Protocol::MchoseK99V3;
    a->mchosePath = keyboard.path = L"test keyboard receiver";
    SendMessageW(a->host, MchoseMsg, WPARAM(MchoseLink::Down), 0);
    check(keyboard.dormant && !keyboard.connected && keyboard.battery.level == 100 &&
              a->workerSettings.paused.contains(keyboard.key),
          "keyboard link loss keeps cached charge and pauses queries");
    SendMessageW(a->host, MchoseMsg, WPARAM(MchoseLink::Up), 0);
    check(!a->workerSettings.paused.contains(keyboard.key) && a->request,
          "keyboard link restore queues battery refresh");
    keyboard.dormant = false;
    keyboard.connected = true;
    keyboard.path.clear();
    a->mchosePath.clear();
    a->busy = false;
    bool allDigitsFit = true;
    FontFamily family(L"Microsoft YaHei UI");
    StringFormat format;
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetAlignment(StringAlignmentFar);
    format.SetLineAlignment(StringAlignmentCenter);
    for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        Bitmap bitmap(256, 144, PixelFormat32bppPARGB);
        Graphics graphics(&bitmap);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.ScaleTransform(scale, scale);
        for (bool mini : {true, false}) {
            auto bounds = BatteryNumberBounds(mini ? 304.f : 608.f, 0, mini);
            Font digits(&family, mini ? 28.f : 44.f, FontStyleBold, UnitPixel);
            RectF measured;
            INT characters = 0;
            graphics.MeasureString(L"100", 3, &digits, bounds, &format, &measured, &characters, nullptr);
            allDigitsFit = allDigitsFit && characters == 3 && measured.Width <= bounds.Width;
        }
    }
    check(allDigitsFit, "100 fits popup and overview at 100/125/150/200 percent DPI");
    auto output = std::filesystem::path(a->directory).parent_path();
    std::filesystem::create_directories(output);
    Save();
    Main(false);
    auto &main = a->main;
    // Keep fixture geometry deterministic on hosted runners with small work areas.
    SetWindowPos(main.hwnd, nullptr, 0, 0, int(760 * main.scale), int(720 * main.scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    auto pointMessage = [&](View &v, UINT message, PointF p) {
        SendMessageW(v.hwnd, message, message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE ? MK_LBUTTON : 0,
                     MAKELPARAM(int(p.X * v.scale), int(p.Y * v.scale)));
    };
    int frames = 0;
    bool opaqueFrames = true, darkFrames = true;
    auto frameCheck = [&](View &v) {
        RECT rect{};
        GetClientRect(v.hwnd, &rect);
        Bitmap frame(rect.right, rect.bottom, PixelFormat32bppPARGB);
        {
            Graphics graphics(&frame);
            DrawFrame(graphics, v);
        }
        BitmapData pixels{};
        Rect bounds(0, 0, rect.right, rect.bottom);
        if (frame.LockBits(&bounds, ImageLockModeRead, PixelFormat32bppPARGB, &pixels) != Ok) {
            darkFrames = false;
            return;
        }
        for (int y = 0; y < rect.bottom; ++y) {
            auto row = static_cast<const BYTE *>(pixels.Scan0) + y * pixels.Stride;
            for (int x = 0; x < rect.right; ++x) {
                auto pixel = row + x * 4;
                opaqueFrames = opaqueFrames && pixel[3] == 255;
                if (pixel[0] >= 250 && pixel[1] >= 250 && pixel[2] >= 250)
                    darkFrames = false;
            }
        }
        frame.UnlockBits(&pixels);
        ++frames;
    };
    auto click = [&](View &v, int id, bool inspectFrames = false) {
        auto it = std::find_if(v.buttons.begin(), v.buttons.end(), [&](auto &b) { return b.id == id; });
        if (it == v.buttons.end()) {
            check(false, "button exists");
            return;
        }
        auto r = ButtonRect(v, *it);
        PointF p(r.X + r.Width / 2, r.Y + r.Height / 2);
        pointMessage(v, WM_LBUTTONDOWN, p);
        if (inspectFrames)
            frameCheck(v);
        pointMessage(v, WM_LBUTTONUP, p);
        if (inspectFrames)
            frameCheck(v);
    };
    for (int i = 0; i < 15; ++i) {
        click(main, SettingsTab, true);
        click(main, Overview, true);
    }
    check(frames == 60 && opaqueFrames && darkFrames,
          "60 pressed/page-change frames: fully opaque, no white fill");
    check(!GetWindow(main.hwnd, GW_CHILD), "no independently painted child controls");
    SendMessageW(main.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    check(Offset(main) > 0, "overview wheel scroll");
    SendMessageW(main.hwnd, WM_KEYDOWN, VK_END, 0);
    check(Offset(main) == MaxOffset(main), "overview scroll clamps at end");
    SendMessageW(main.hwnd, WM_KEYDOWN, VK_HOME, 0);
    auto thumb = ScrollThumb(main);
    PointF grip(thumb.X + 3, thumb.Y + 8);
    pointMessage(main, WM_LBUTTONDOWN, grip);
    pointMessage(main, WM_MOUSEMOVE, PointF(grip.X, grip.Y + 65));
    pointMessage(main, WM_LBUTTONUP, PointF(grip.X, grip.Y + 65));
    check(Offset(main) > 0 && !main.scrollDrag, "scrollbar thumb drag");
    SetOffset(main, 0);
    auto first = a->devices.front().key;
    PointF from(180, ListArea(main).Y + 30), to(180, ListArea(main).Y + 2 * RowStep(main) + 80);
    pointMessage(main, WM_LBUTTONDOWN, from);
    pointMessage(main, WM_MOUSEMOVE, to);
    pointMessage(main, WM_LBUTTONUP, to);
    check(a->devices[2].key == first, "overview drag reorders cards");
    check(LoadSettings(a->directory).devices[first].order == 2, "drag order persisted");

    click(main, SettingsTab);
    check(Offset(main) == 0, "settings keeps an independent scroll offset");
    auto bounds = [&](int id) {
        auto it = std::find_if(main.buttons.begin(), main.buttons.end(), [&](auto &b) { return b.id == id; });
        return it == main.buttons.end() ? RectF{} : ButtonRect(main, *it);
    };
    auto fixedInterval = bounds(Seconds300), fixedStartup = bounds(Autostart);
    SendMessageW(main.hwnd, WM_KEYDOWN, VK_END, 0);
    auto intervalAtEnd = bounds(Seconds300), startupAtEnd = bounds(Autostart);
    check(fixedInterval.Y == intervalAtEnd.Y && fixedStartup.Y == startupAtEnd.Y &&
              HitButton(main, PointF(intervalAtEnd.X + 10, intervalAtEnd.Y + 10)) == Seconds300 &&
              HitButton(main, PointF(startupAtEnd.X + 10, startupAtEnd.Y + 10)) == Autostart,
          "general controls stay fixed and clickable at the end of twelve-device list");
    float endOffset = Offset(main);
    click(main, Seconds300);
    check(LoadSettings(a->directory).interval == 300 && Offset(main) == endOffset,
          "refresh interval changes without moving device list");
    check(ListArea(main).GetBottom() < GeneralSettingsArea(main).Y &&
              HitRow(main, PointF(48, GeneralSettingsArea(main).Y + 10)) == -1,
          "general settings excluded from device scrolling and drag targets");
    check(Capture((output / L"many-settings-bottom.png").wstring(), main.hwnd),
          "capture fixed settings at list end");
    SendMessageW(main.hwnd, WM_KEYDOWN, VK_HOME, 0);
    check(Offset(main) == 0 && bounds(Autostart).Y == fixedStartup.Y,
          "general settings unchanged at list beginning");

    SendMessageW(main.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    check(Offset(main) > 0, "settings wheel scroll");
    float beforeFixedFocus = Offset(main);
    main.focusId = Seconds60;
    FocusNext(main, false);
    SendMessageW(main.hwnd, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(main.hwnd, WM_KEYUP, VK_SPACE, 0);
    check(LoadSettings(a->directory).interval == 120 && Offset(main) == beforeFixedFocus,
          "Tab and Space activate fixed controls without scrolling device list");
    SetOffset(main, 0);
    auto hidden = a->devices[0].key;
    int savedOrder = a->settings.devices[hidden].order;
    click(main, 301);
    check(!a->devices[0].visible && a->settings.devices[hidden].order == savedOrder,
          "visibility toggle preserves saved order");
    auto second = a->devices[1].key;
    from = PointF(180, ListArea(main).Y + 30);
    to = PointF(180, ListArea(main).Y + RowStep(main) + 60);
    pointMessage(main, WM_LBUTTONDOWN, from);
    pointMessage(main, WM_MOUSEMOVE, to);
    pointMessage(main, WM_LBUTTONUP, to);
    check(a->devices[0].key == second && a->devices[1].key == hidden && !a->devices[1].visible,
          "settings drag includes hidden devices without changing visibility");
    auto reversed = a->devices;
    std::reverse(reversed.begin(), reversed.end());
    auto reloaded = LoadSettings(a->directory);
    SortDevices(reversed, reloaded);
    bool same = reversed.size() == a->devices.size();
    for (size_t i = 0; i < reversed.size(); ++i)
        same = same && reversed[i].key == a->devices[i].key;
    check(same, "saved order restored after re-enumeration");

    auto iconPixels = [](const std::vector<Device> &devices) {
        HICON icon = MakeTrayIcon(devices, 32);
        ICONINFO info{};
        std::vector<BYTE> result(32 * 32 * 4);
        if (icon && GetIconInfo(icon, &info)) {
            GetBitmapBits(info.hbmColor, LONG(result.size()), result.data());
            DeleteObject(info.hbmColor);
            DeleteObject(info.hbmMask);
        } else
            result.clear();
        if (icon)
            DestroyIcon(icon);
        return result;
    };
    auto baseIcon = iconPixels(a->devices);
    auto modified = a->devices;
    int visibleIndex = 0;
    for (auto &d : modified)
        if (d.registered && d.visible && ++visibleIndex == 4) {
            d.battery.level = 1;
            break;
        }
    check(!baseIcon.empty() && baseIcon == iconPixels(modified),
          "tray ignores devices after the first three visible");
    modified = a->devices;
    auto visible =
        std::find_if(modified.begin(), modified.end(), [](auto &d) { return d.registered && d.visible; });
    visible->battery.level = 1;
    check(baseIcon != iconPixels(modified), "tray follows the first visible device");

    SetWindowPos(main.hwnd, nullptr, 0, 0, int(640 * main.scale), int(420 * main.scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    check(main.width == 640 && main.height == 420 && MaxOffset(main) > 0, "main resize updates viewport");
    float beforeFocus = Offset(main);
    main.focusId = Seconds300;
    FocusNext(main, false);
    auto startupRect = bounds(Autostart), panel = GeneralSettingsArea(main);
    check(main.focusId == Autostart && Offset(main) == beforeFocus && startupRect.Y >= panel.Y &&
              startupRect.GetBottom() <= panel.GetBottom() && panel.GetBottom() <= main.height - 68 &&
              ListArea(main).GetBottom() < panel.Y,
          "small window keeps full general panel fixed with no overlap");
    auto tab = bounds(SettingsTab);
    check(HitButton(main, PointF(tab.X + 10, tab.Y + 10)) == SettingsTab &&
              HitButton(main, PointF(startupRect.X + 20, startupRect.Y + 10)) == Autostart,
          "compact tabs and fixed general settings remain clickable");
    main.focusId = 300 + (int(a->devices.size()) - 2) * 2 + 1;
    FocusNext(main, false);
    auto deviceButton = bounds(300 + (int(a->devices.size()) - 1) * 2);
    check(deviceButton.Y >= ListArea(main).Y && deviceButton.GetBottom() <= ListArea(main).GetBottom() + 1 &&
              Offset(main) > beforeFocus,
          "keyboard focus scrolls only device controls into the reduced viewport");
    check(Capture((output / L"many-settings.png").wstring(), main.hwnd), "capture narrow scrolled settings");

    click(main, Overview);
    SetOffset(main, 0);
    auto orderBeforeCancel = a->devices[0].key;
    pointMessage(main, WM_LBUTTONDOWN, PointF(180, ListArea(main).Y + 30));
    pointMessage(main, WM_MOUSEMOVE, PointF(180, ListArea(main).GetBottom() - 4));
    SendMessageW(main.hwnd, WM_TIMER, 31, 0);
    check(Offset(main) > 0 && main.dragging, "drag near viewport edge auto-scrolls");
    SendMessageW(main.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!main.dragging && GetCapture() != main.hwnd && a->devices[0].key == orderBeforeCancel,
          "Escape cancels drag without persisting a move");
    check(Capture((output / L"many-overview.png").wstring(), main.hwnd), "capture resized overview");

    Popup(false);
    auto &popup = a->popup;
    SetWindowPos(popup.hwnd, nullptr, 0, 0, int(396 * popup.scale), int(320 * popup.scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    check(popup.height == 320 && MaxOffset(popup) > 0, "popup resize updates viewport");
    SendMessageW(popup.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    check(Offset(popup) > 0, "popup wheel scroll");
    SendMessageW(popup.hwnd, WM_KEYDOWN, VK_HOME, 0);
    auto oldFirstVisible = Rows(popup)[0]->key, oldSecondVisible = Rows(popup)[1]->key;
    from = PointF(150, ListArea(popup).Y + 30);
    to = PointF(150, ListArea(popup).Y + RowStep(popup) + 60);
    pointMessage(popup, WM_LBUTTONDOWN, from);
    pointMessage(popup, WM_MOUSEMOVE, to);
    pointMessage(popup, WM_LBUTTONUP, to);
    check(!a->hover, "interacting with hover popup pins it during drag and scrolling");
    auto popupRows = Rows(popup), mainRows = Rows(main);
    check(popupRows[0]->key == oldSecondVisible && popupRows[1]->key == oldFirstVisible &&
              popupRows[0]->key == mainRows[0]->key,
          "popup drag updates global order and main overview");
    check(Capture((output / L"many-popup.png").wstring(), popup.hwnd), "capture scrollable popup");
    Main(false);
    click(main, Hide);
    MSG closeMessage{};
    check(!IsWindowVisible(main.hwnd) && IsWindow(a->host) &&
              !PeekMessageW(&closeMessage, a->host, WM_CLOSE, WM_CLOSE, PM_NOREMOVE) &&
              !PeekMessageW(&closeMessage, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE),
          "X hides the window without requesting host exit");
    Main(false);
    SendMessageW(main.hwnd, WM_CLOSE, 0, 0);
    check(!IsWindowVisible(main.hwnd) && IsWindow(a->host) &&
              !PeekMessageW(&closeMessage, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE),
          "system window close keeps tray host alive");
    auto fixtureSettings = LoadSettings(a->directory);
    check(fixtureSettings.devices.size() == 12, "twelve device settings survive sorting and filtering");
    a->settings = originalSettings;
    a->devices = originalDevices;
    Save();
    std::ofstream log(output / L"ui-regression.txt");
    for (auto &line : checks)
        log << line << '\n';
    log << (pass ? "ALL PASS\n" : "FAILED\n");
    return pass;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (a && m == a->taskbarMsg) {
        Tray(true);
        return 0;
    }
    switch (m) {
    case NovaMsg: {
        std::unique_ptr<Reading> reading(reinterpret_cast<Reading *>(l));
        bool changed = false;
        for (auto &d : a->devices)
            if (d.protocol == Protocol::Nova && d.registered) {
                if (reading->level < 0) {
                    if (!d.dormant) {
                        d.dormant = true;
                        d.connected = false;
                        a->settings.paused.insert(d.key);
                        changed = true;
                    }
                } else if (d.dormant || d.battery.level != reading->level ||
                           d.battery.charging != reading->charging) {
                    d.battery = *reading;
                    d.connected = true;
                    d.dormant = false;
                    a->settings.paused.erase(d.key);
                    GetLocalTime(&d.updated);
                    changed = true;
                }
            }
        if (changed) {
            {
                std::lock_guard lock(a->mutex);
                a->workerSettings = a->settings;
            }
            Tray();
            if (a->main.hwnd)
                InvalidateRect(a->main.hwnd, nullptr, FALSE);
            if (a->popup.hwnd)
                InvalidateRect(a->popup.hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case NovaClosed:
        if (a->novaWatch.joinable())
            a->novaWatch.join();
        if (a->watchStop) {
            CloseHandle(a->watchStop);
            a->watchStop = nullptr;
        }
        a->novaPath.clear();
        return 0;
    case MchoseMsg: {
        auto state = MchoseLink(w);
        if (state == MchoseLink::Closed) {
            StopMchoseWatch();
            return 0;
        }
        for (auto &d : a->devices) {
            if (d.protocol != Protocol::MchoseK99V3 || !d.registered || d.path != a->mchosePath)
                continue;
            if (state == MchoseLink::Up) {
                a->settings.paused.erase(d.key);
                RefreshNow();
            } else {
                d.dormant = true;
                d.connected = false;
                a->settings.paused.insert(d.key);
                {
                    std::lock_guard lock(a->mutex);
                    a->workerSettings = a->settings;
                }
                Tray();
                if (a->main.hwnd)
                    InvalidateRect(a->main.hwnd, nullptr, FALSE);
                if (a->popup.hwnd)
                    InvalidateRect(a->popup.hwnd, nullptr, FALSE);
            }
            break;
        }
        return 0;
    }
    case WM_INPUT: {
        RAWINPUTHEADER header{};
        UINT size = sizeof(header);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_HEADER, &header, &size,
                            sizeof(RAWINPUTHEADER)) == sizeof(header)) {
            auto now = GetTickCount64();
            auto &previousTick = a->inputTicks[header.hDevice];
            if (now - previousTick < 100)
                return DefWindowProcW(h, m, w, l);
            previousTick = now;
            auto it = a->inputPaths.find(header.hDevice);
            if (it == a->inputPaths.end()) {
                UINT n = 0;
                GetRawInputDeviceInfoW(header.hDevice, RIDI_DEVICENAME, nullptr, &n);
                std::wstring path(n, L'\0');
                if (n)
                    GetRawInputDeviceInfoW(header.hDevice, RIDI_DEVICENAME, path.data(), &n);
                for (auto &ch : path)
                    ch = wchar_t(towlower(ch));
                it = a->inputPaths.emplace(header.hDevice, std::move(path)).first;
            }
            for (auto &d : a->devices) {
                if (d.protocol == Protocol::MchoseK99V3)
                    continue;
                wchar_t token[40];
                swprintf_s(token, L"vid_%04x&pid_%04x", d.vid, d.pid);
                if (it->second.find(token) == std::wstring::npos)
                    continue;
                a->activity[d.key] = GetTickCount64();
                bool wasPaused = a->settings.paused.erase(d.key) > 0 || d.dormant;
                if (wasPaused && d.registered) {
                    d.dormant = false;
                    SetTimer(h, 21, 500, nullptr);
                }
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
    case ResultMsg: {
        std::unique_ptr<ScanResult> r(reinterpret_cast<ScanResult *>(l));
        Apply(*r);
        return 0;
    }
    case OpenMsg:
        Main(w != 0);
        return 0;
    case QuitMsg:

        PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_COMMAND:
        Command(LOWORD(w));
        return 0;
    case TrayMsg: {
        UINT e = LOWORD(l);
        if (e == NIN_POPUPOPEN) {
            if (!a->main.hwnd || !IsWindowVisible(a->main.hwnd))
                Popup(false);
        } else if (e == NIN_POPUPCLOSE) {
            if (a->hover)
                SetTimer(h, 12, 350, nullptr);
        } else if (e == WM_LBUTTONDBLCLK) {
            a->suppressClick = GetTickCount64() + GetDoubleClickTime();
            KillTimer(h, 11);
            Main(false);
        } else if (e == NIN_SELECT || e == NIN_KEYSELECT) {
            if (GetTickCount64() >= a->suppressClick)
                SetTimer(h, 11, GetDoubleClickTime(), nullptr);
        } else if (e == WM_CONTEXTMENU)
            ContextMenu();
        return 0;
    }
    case WM_TIMER:
        if (w == 21) {
            KillTimer(h, 21);
            RefreshNow();
        } else if (w == 22) {
            UpdatePause();
            bool changed = false;
            for (auto &d : a->devices)
                if (a->settings.paused.contains(d.key) && !d.dormant) {
                    d.dormant = true;
                    changed = true;
                }
            {
                std::lock_guard lock(a->mutex);
                a->workerSettings = a->settings;
            }
            if (changed) {
                Tray();
                if (a->main.hwnd)
                    InvalidateRect(a->main.hwnd, nullptr, FALSE);
                if (a->popup.hwnd)
                    InvalidateRect(a->popup.hwnd, nullptr, FALSE);
            }
        } else if (w == 20) {
            KillTimer(h, 20);
            HWND target = a->capturePopup ? a->popup.hwnd : a->main.hwnd;
            RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            Capture(a->capture, target);
        } else if (w == 11) {
            KillTimer(h, 11);
            if (a->popup.hwnd && IsWindowVisible(a->popup.hwnd) && !a->hover)
                HidePopup();
            else
                Popup(true);
        } else if (w == 10) {
            KillTimer(h, 10);
            RefreshNow();
        } else if (w == 12) {
            POINT p;
            GetCursorPos(&p);
            RECT popup{}, icon{};
            GetWindowRect(a->popup.hwnd, &popup);
            NOTIFYICONIDENTIFIER id{sizeof(id)};
            id.hWnd = h;
            id.uID = 1;
            id.guidItem = trayGuid;
            Shell_NotifyIconGetRect(&id, &icon);
            if (a->hover && !PtInRect(&popup, p) && !PtInRect(&icon, p))
                HidePopup();
        }
        return 0;
    case WM_DEVICECHANGE:
        if (w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE || w == DBT_DEVNODES_CHANGED)
            SetTimer(h, 10, 1500, nullptr);
        return TRUE;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND)
            SetTimer(h, 10, 1500, nullptr);
        return TRUE;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:

        if (w)
            PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:

        if (a->quitting)
            return 0;
        a->quitting = true;
        StopMchoseWatch();
        if (a->watchStop)
            SetEvent(a->watchStop);
        if (a->novaWatch.joinable())
            a->novaWatch.join();
        if (a->watchStop) {
            CloseHandle(a->watchStop);
            a->watchStop = nullptr;
        }
        {
            std::lock_guard lock(a->mutex);
            a->stop = true;
        }
        a->cv.notify_one();
        if (a->worker.joinable())
            a->worker.join();
        Shell_NotifyIconW(NIM_DELETE, &a->nid);
        if (a->notification)
            UnregisterDeviceNotification(a->notification);
        if (a->main.hwnd)
            DestroyWindow(a->main.hwnd);
        if (a->popup.hwnd)
            DestroyWindow(a->popup.hwnd);
        if (a->tray)
            DestroyIcon(a->tray);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:

        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring dir = (std::filesystem::path(ExecutablePath()).parent_path() / L"data").wstring(), probe,
                 capture, autostart;
    bool tray = false, settings = false, test = false, quit = false, popup = false, verify = false,
         startupTest = false, layoutTest = false, explicitData = false;
    for (int i = 1; i < count; ++i) {
        std::wstring s = args[i];
        if (s == L"--tray")
            tray = true;
        else if (s == L"--settings")
            settings = true;
        else if (s == L"--self-test")
            test = true;
        else if (s == L"--quit")
            quit = true;
        else if (s == L"--popup")
            popup = true;
        else if (s == L"--test-startup")
            startupTest = true;
        else if (s == L"--autostart") {
            if (i + 1 >= count) {
                LocalFree(args);
                return 2;
            }
            autostart = args[++i];
        } else if (s == L"--verify-controls")
            verify = true;
        else if (s == L"--verify-layout")
            layoutTest = true;
        else if (s == L"--capture" && i + 1 < count)
            capture = args[++i];
        else if (s == L"--data-dir" && i + 1 < count) {
            dir = args[++i];
            explicitData = true;
        } else if (s == L"--probe" && i + 1 < count)
            probe = args[++i];
    }
    LocalFree(args);

    if ((layoutTest || startupTest) && !explicitData)
        return 5;
    if (!autostart.empty()) {
        if (autostart == L"status")
            return IsAutostartEnabled() ? 0 : 3;
        if (autostart != L"on" && autostart != L"off")
            return 2;
        std::wstring error;
        return SetAutostart(autostart == L"on", error) ? 0 : 1;
    }
    if (startupTest) {
        std::wstring error;
        return RunAutostartTests(dir, error) ? 0 : 1;
    }
    if (test) {
        std::wstring e;
        return RunParserTests(e) ? 0 : 1;
    }
    if (!probe.empty()) {
        Settings s;
        return WriteProbe(probe, ScanDevices(s, true)) ? 0 : 2;
    }
    if (quit) {
        auto h = FindWindowW(HostClass, nullptr);
        if (h)
            SendMessageW(h, QuitMsg, 0, 0);
        return 0;
    }
    auto singleton =
        layoutTest ? nullptr : CreateMutexW(nullptr, FALSE, L"Local\\PersonalDimension.PeripheralBattery.1");
    if (!layoutTest && GetLastError() == ERROR_ALREADY_EXISTS) {

        auto h = FindWindowW(HostClass, nullptr);
        if (h && !tray)
            SendMessageW(h, OpenMsg, settings, 0);
        if (singleton)
            CloseHandle(singleton);
        return 0;
    }
    App state;
    a = &state;
    a->instance = instance;
    a->directory = dir;
    if (!layoutTest && !verify) {
        std::wstring error;
        if (!MigrateAutostart(error))
            MessageBoxW(nullptr, error.c_str(), L"开机自启", MB_OK | MB_ICONERROR);
    }
    a->settings = LoadSettings(dir);
    a->capture = capture;
    a->capturePopup = popup;
    a->verifyControls = verify;
    a->verifyLayout = layoutTest;
    a->first = !std::filesystem::exists(std::filesystem::path(dir) / L"settings.ini");
    for (auto &[key, p] : a->settings.devices) {
        Device d;
        d.key = key;
        d.registered = p.registered;
        d.visible = p.visible;
        d.battery.error = L"等待连接";
        if (key.starts_with(L"logi")) {
            d.name = key.find(L"C543") != std::wstring::npos || key.find(L"C09A") != std::wstring::npos
                         ? L"PRO 2 LIGHTSPEED"
                         : L"G502 X PLUS";
            d.subtitle = L"Logitech · LIGHTSPEED";
        } else if (key.starts_with(L"nova")) {
            d.kind = Kind::Headset;
            d.protocol = Protocol::Nova;
            d.name = L"Nova 7 Gen 2";
            d.subtitle = L"SteelSeries · 2.4 GHz";
        } else if (key == DeviceKey(Protocol::MchoseK99V3, 0x3033)) {
            d.kind = Kind::Keyboard;
            d.protocol = Protocol::MchoseK99V3;
            d.name = L"K99 V3";
            d.subtitle = L"MCHOSE · 2.4 GHz";
        } else {
            d.kind = Kind::Gamepad;
            d.protocol = Protocol::Pangu;
            d.name = L"北通盘古";
            d.subtitle = L"BEITONG · BTP-PG01A";
        }
        a->devices.push_back(d);
    }
    SortDevices(a->devices, a->settings);
    GdiplusStartupInput input;
    ULONG_PTR gdip;
    if (GdiplusStartup(&gdip, &input, nullptr) != Ok) {
        if (singleton)
            CloseHandle(singleton);
        return 3;
    }
    // This canvas has no text-entry fields. Do not load an IME into its UI thread.
    ImmDisableIME(0);
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW hc{};
    hc.hInstance = instance;
    hc.lpfnWndProc = HostProc;
    hc.lpszClassName = layoutTest ? L"PBC.TestHost.1" : HostClass;
    RegisterClassW(&hc);
    WNDCLASSW vc{};
    vc.hInstance = instance;
    vc.lpfnWndProc = ViewProc;
    vc.lpszClassName = ViewClass;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    vc.style = CS_DBLCLKS;
    RegisterClassW(&vc);
    a->host = CreateWindowExW(WS_EX_TOOLWINDOW, hc.lpszClassName, L"PeripheralBattery.Host", WS_OVERLAPPED, 0,
                              0, 0, 0, nullptr, nullptr, instance, nullptr);
    a->taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    a->nid.cbSize = sizeof(a->nid);
    a->nid.hWnd = a->host;
    a->nid.uID = 1;
    a->nid.guidItem = trayGuid;
    a->nid.uCallbackMessage = TrayMsg;
    if (a->verifyLayout) {
        bool pass = VerifyLayout();
        if (a->main.hwnd)
            DestroyWindow(a->main.hwnd);
        if (a->popup.hwnd)
            DestroyWindow(a->popup.hwnd);
        DestroyWindow(a->host);
        GdiplusShutdown(gdip);
        if (singleton)
            CloseHandle(singleton);
        return pass ? 0 : 1;
    }
    Tray(true);
    DEV_BROADCAST_DEVICEINTERFACE_W f{};
    f.dbcc_size = sizeof(f);
    f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    f.dbcc_classguid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0, 0x11, 0x11, 0, 0, 0x30}};
    a->notification = RegisterDeviceNotificationW(a->host, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    RAWINPUTDEVICE inputDevices[] = {{1, 2, RIDEV_INPUTSINK, a->host}, {0x0c, 1, RIDEV_INPUTSINK, a->host}};
    RegisterRawInputDevices(inputDevices, 2, sizeof(RAWINPUTDEVICE));
    SetTimer(a->host, 22, 5000, nullptr);
    if (!tray)
        Main(settings);
    Worker();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0) {
            if (msg.wParam == 'R') {
                RefreshNow();
                continue;
            }
            if (msg.wParam == VK_OEM_COMMA) {
                Main(true);
                continue;
            }
        }
        // Keyboard focus and activation are handled by the single canvas.
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    while (PeekMessageW(&msg, nullptr, ResultMsg, ResultMsg, PM_REMOVE))
        delete reinterpret_cast<ScanResult *>(msg.lParam);
    while (PeekMessageW(&msg, nullptr, NovaMsg, NovaMsg, PM_REMOVE))
        delete reinterpret_cast<Reading *>(msg.lParam);
    GdiplusShutdown(gdip);
    if (singleton)
        CloseHandle(singleton);

    return 0;
}

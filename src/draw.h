#pragma once
#include "core.h"
#include <objidl.h>
#include <gdiplus.h>
namespace pbc {
using Gdiplus::Color;
inline Color Bg() {
    return Color(255, 15, 20, 28);
}
inline Color Surface() {
    return Color(255, 24, 31, 42);
}
inline Color Edge() {
    return Color(255, 43, 54, 70);
}
inline Color Ink() {
    return Color(255, 235, 241, 248);
}
inline Color Muted() {
    return Color(255, 139, 155, 176);
}
inline Color Mint() {
    return Color(255, 102, 226, 190);
}
Color BatteryColor(int level);
void Round(Gdiplus::Graphics &, float, float, float, float, float, Color, Color border = Color(0, 0, 0, 0));
void Text(Gdiplus::Graphics &, const std::wstring &, float, float, float, float, float, Color,
          bool bold = false, int align = 0);
void Line(Gdiplus::Graphics &, float, float, float, float, Color, float width = 1.6f);
void DeviceIcon(Gdiplus::Graphics &, Kind, float, float, float, Color);
void Brand(Gdiplus::Graphics &, float, float, float);
void Bolt(Gdiplus::Graphics &, float, float, float, Color);
HICON MakeTrayIcon(const std::vector<Device> &, int size);
} // namespace pbc

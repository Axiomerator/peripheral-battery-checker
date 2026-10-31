#include "draw.h"
#include <algorithm>
namespace pbc {
using namespace Gdiplus;
Color BatteryColor(int n) {
    return n < 0     ? Muted()
           : n <= 20 ? Color(255, 255, 124, 131)
           : n <= 40 ? Color(255, 244, 194, 105)
                     : Mint();
}
static void Path(GraphicsPath &p, float x, float y, float w, float h, float r) {
    r = (std::min)({r, w / 2, h / 2});
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
void Round(Graphics &g, float x, float y, float w, float h, float r, Color color, Color border) {
    if (w <= 0 || h <= 0)
        return;
    GraphicsPath p;
    Path(p, x, y, w, h, r);
    SolidBrush b(color);
    g.FillPath(&b, &p);
    if (border.GetA()) {
        Pen pen(border, 1);
        g.DrawPath(&pen, &p);
    }
}
void Text(Graphics &g, const std::wstring &s, float x, float y, float w, float h, float size, Color c,
          bool bold, int align) {
    FontFamily family(L"Microsoft YaHei UI");
    Font font(&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush(c);
    StringFormat f;
    f.SetAlignment(align == 2   ? StringAlignmentFar
                   : align == 1 ? StringAlignmentCenter
                                : StringAlignmentNear);
    f.SetLineAlignment(StringAlignmentCenter);
    f.SetTrimming(StringTrimmingEllipsisCharacter);
    f.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), int(s.size()), &font, RectF(x, y, w, h), &f, &brush);
}
void Line(Graphics &g, float x, float y, float x2, float y2, Color color, float width) {
    Pen p(color, width);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    g.DrawLine(&p, x, y, x2, y2);
}
void Brand(Graphics &g, float x, float y, float size) {
    Round(g, x, y, size, size, size * .27f, Color(255, 34, 66, 62));
    for (int i = 0; i < 3; i++) {
        float h = size * (.25f + .14f * i);
        Round(g, x + size * (.23f + .19f * i), y + (size - h) / 2, size * .10f, h, size * .045f, Mint());
    }
}
void Bolt(Graphics &g, float x, float y, float size, Color color) {
    PointF p[]{
        {x + size * .58f, y},        {x + size * .12f, y + size * .58f}, {x + size * .47f, y + size * .58f},
        {x + size * .32f, y + size}, {x + size * .9f, y + size * .38f},  {x + size * .55f, y + size * .38f}};
    SolidBrush b(color);
    g.FillPolygon(&b, p, 6);
}
void DeviceIcon(Graphics &g, Kind kind, float x, float y, float size, Color color) {
    auto state = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 48, size / 48);
    Pen p(color, 2);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    if (kind == Kind::Mouse) {
        GraphicsPath path;
        Path(path, 13, 4, 23, 39, 11);
        g.DrawPath(&p, &path);
        Line(g, 24.5f, 5, 24.5f, 19, color, 1.7f);
        Round(g, 22.5f, 11, 4, 8, 2, color);
        Line(g, 14, 21, 35, 21, color, 1.5f);
    } else if (kind == Kind::Headset) {
        g.DrawArc(&p, 7, 6, 34, 35, 180, 180);
        Round(g, 5, 23, 9, 17, 4, Color(0, 0, 0, 0), color);
        Round(g, 34, 23, 9, 17, 4, Color(0, 0, 0, 0), color);
        Line(g, 40, 38, 34, 43, color);
        Line(g, 34, 43, 27, 43, color);
        Round(g, 24, 41, 6, 4, 2, color);
    } else {
        GraphicsPath path;
        path.AddBezier(11, 14, 5, 14, 4, 28, 4, 35);
        path.AddBezier(4, 35, 4, 43, 13, 42, 18, 33);
        path.AddLine(18, 33, 30, 33);
        path.AddBezier(30, 33, 35, 42, 44, 43, 44, 35);
        path.AddBezier(44, 35, 44, 28, 43, 14, 37, 14);
        path.AddBezier(37, 14, 30, 10, 18, 10, 11, 14);
        path.CloseFigure();
        g.DrawPath(&p, &path);
        Line(g, 11, 23, 20, 23, color);
        Line(g, 15.5f, 18.5f, 15.5f, 27.5f, color);
        SolidBrush b(color);
        g.FillEllipse(&b, 31.f, 18.f, 3.f, 3.f);
        g.FillEllipse(&b, 36.f, 23.f, 3.f, 3.f);
        g.FillEllipse(&b, 28.f, 26.f, 3.f, 3.f);
        g.FillEllipse(&b, 33.f, 31.f, 3.f, 3.f);
    }
    g.Restore(state);
}
HICON MakeTrayIcon(const std::vector<Device> &devices, int size) {
    Bitmap b(size, size, PixelFormat32bppARGB);
    Graphics g(&b);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    g.ScaleTransform(float(size) / 32, float(size) / 32);
    Round(g, 1, 1, 30, 30, 8, Color(255, 22, 34, 43), Color(255, 64, 88, 95));
    std::vector<const Device *> visible;
    for (auto &d : devices)
        if (d.registered && d.visible)
            visible.push_back(&d);
    for (int i = 0; i < 3; ++i) {
        auto *d = i < int(visible.size()) ? visible[i] : nullptr;
        int level = d ? d->battery.level : -1;
        float h = level < 0 ? 5.f : 6.f + float(level) * .13f;
        Color c = d ? BatteryColor(level) : Color(255, 70, 85, 100);
        if (visible.empty())
            c = Mint();
        Round(g, 7.f + i * 7, 23 - h, 4, h, 2, c);
    }
    HICON icon = nullptr;
    b.GetHICON(&icon);
    return icon;
}
} // namespace pbc

#include "ui.h"

#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>

namespace pk::ui {
namespace {
using namespace Gdiplus;

Color color(COLORREF rgb) { return Color(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)); }

HFONT font(int size, int weight, UINT dpi) {
    return CreateFontW(-MulDiv(size, static_cast<int>(dpi), 96), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}
}

Fonts::~Fonts() {
    for (HFONT current : {caption, body, strong, card, section, value, title}) if (current) DeleteObject(current);
}

void Fonts::reset(UINT dpi) {
    for (HFONT current : {caption, body, strong, card, section, value, title}) if (current) DeleteObject(current);
    caption = font(12, FW_NORMAL, dpi);
    body = font(14, FW_NORMAL, dpi);
    strong = font(14, FW_SEMIBOLD, dpi);
    card = font(17, FW_SEMIBOLD, dpi);
    section = font(18, FW_SEMIBOLD, dpi);
    value = font(26, FW_SEMIBOLD, dpi);
    title = font(39, FW_SEMIBOLD, dpi);
}

RECT box(int x, int y, int width, int height) { return {x, y, x + width, y + height}; }

std::wstring window_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(control, value.data(), length + 1)));
    return value;
}

void dark_caption(HWND window) {
    constexpr BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &rail, sizeof(rail));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

void fill(HDC dc, RECT rect, COLORREF rgb) {
    SetDCBrushColor(dc, rgb);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void rounded(HDC dc, RECT rect, int radius, COLORREF rgb, COLORREF border) {
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const float x = static_cast<float>(rect.left) + 0.5f;
    const float y = static_cast<float>(rect.top) + 0.5f;
    const float width = static_cast<float>(rect.right - rect.left) - 1;
    const float height = static_cast<float>(rect.bottom - rect.top) - 1;
    if (width <= 0 || height <= 0) return;
    const float diameter = std::min({static_cast<float>(2 * radius), width, height});
    GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    SolidBrush brush(color(rgb));
    graphics.FillPath(&brush, &path);
    if (border != CLR_INVALID) {
        Pen pen(color(border), 1);
        graphics.DrawPath(&pen, &path);
    }
}

void label(HDC dc, RECT rect, std::wstring_view value, HFONT type, COLORREF rgb, UINT flags) {
    if (value.empty()) return;
    const auto previous = SelectObject(dc, type);
    SetTextColor(dc, rgb);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect, flags);
    SelectObject(dc, previous);
}

void icon(HDC dc, Icon glyph, RECT rect, COLORREF rgb) {
    if (glyph == Icon::none) return;
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<float>(rect.left), static_cast<float>(rect.top));
    graphics.ScaleTransform(static_cast<float>(rect.right - rect.left) / 24.f,
                            static_cast<float>(rect.bottom - rect.top) / 24.f);
    Pen pen(color(rgb), 1.65f);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(color(rgb));
    const auto path = [&](std::initializer_list<PointF> points) {
        graphics.DrawLines(&pen, points.begin(), static_cast<INT>(points.size()));
    };
    const auto circle = [&](float x, float y, float size) { graphics.DrawEllipse(&pen, x, y, size, size); };
    switch (glyph) {
    case Icon::brand:
        pen.SetWidth(2.f);
        path({{3, 9}, {3, 6}, {10, 6}, {12, 8}, {21, 8}, {21, 19}, {3, 19}, {3, 9}});
        path({{8, 13.5f}, {10.5f, 16}, {16.5f, 11}});
        break;
    case Icon::folder:
        path({{3, 18}, {3, 6}, {10, 6}, {12, 8}, {21, 8}, {21, 18}, {3, 18}});
        break;
    case Icon::backup:
        path({{4, 9}, {4, 20}, {20, 20}, {20, 9}});
        graphics.DrawRectangle(&pen, 3.f, 4.f, 18.f, 5.f);
        graphics.DrawLine(&pen, 10, 13, 14, 13);
        break;
    case Icon::trash:
        path({{5, 7}, {6, 21}, {18, 21}, {19, 7}});
        path({{3, 7}, {21, 7}});
        path({{9, 7}, {9, 3}, {15, 3}, {15, 7}});
        path({{10, 11}, {10, 17}});
        path({{14, 11}, {14, 17}});
        break;
    case Icon::plus:
        path({{5, 12}, {19, 12}});
        path({{12, 5}, {12, 19}});
        break;
    case Icon::import:
        path({{4, 14}, {4, 20}, {20, 20}, {20, 14}});
        path({{12, 3}, {12, 15}});
        path({{8, 11}, {12, 15}, {16, 11}});
        break;
    case Icon::search:
        circle(4, 4, 11.5f);
        path({{14, 14}, {20, 20}});
        break;
    case Icon::sort:
        path({{4, 6}, {20, 6}});
        path({{7, 12}, {17, 12}});
        path({{10, 18}, {14, 18}});
        break;
    case Icon::settings:
        path({{10, 3}, {14, 3}, {15, 6}, {18, 6}, {20, 9}, {19, 12}, {20, 15}, {18, 18},
              {15, 18}, {14, 21}, {10, 21}, {9, 18}, {6, 18}, {4, 15}, {5, 12}, {4, 9},
              {6, 6}, {9, 6}, {10, 3}});
        circle(9.5f, 9.5f, 5);
        break;
    case Icon::arrow:
        path({{5, 12}, {18, 12}});
        path({{13, 7}, {18, 12}, {13, 17}});
        break;
    case Icon::disk:
        path({{3, 13}, {6, 5}, {18, 5}, {21, 13}, {21, 19}, {3, 19}, {3, 13}, {21, 13}});
        graphics.FillEllipse(&brush, 6.f, 15.2f, 2.f, 2.f);
        graphics.FillEllipse(&brush, 10.f, 15.2f, 2.f, 2.f);
        break;
    case Icon::issues:
        circle(3, 3, 18);
        path({{12, 7}, {12, 13}});
        graphics.FillEllipse(&brush, 10.9f, 15.f, 2.2f, 2.2f);
        break;
    case Icon::github:
        circle(3, 3, 5);
        circle(3, 16, 5);
        circle(16, 3, 5);
        path({{5.5f, 8}, {5.5f, 16}});
        path({{18.5f, 8}, {18.5f, 11}, {15, 14}, {9, 14}, {5.5f, 17}});
        break;
    case Icon::file:
        path({{5, 3}, {14, 3}, {19, 8}, {19, 21}, {5, 21}, {5, 3}});
        path({{14, 3}, {14, 8}, {19, 8}});
        path({{8, 12}, {16, 12}});
        path({{8, 16}, {14, 16}});
        break;
    case Icon::more:
        for (float x : {5.f, 10.5f, 16.f}) graphics.FillEllipse(&brush, x, 10.5f, 3.f, 3.f);
        break;
    case Icon::lock:
        graphics.DrawArc(&pen, 7.f, 3.f, 10.f, 11.f, 180, 180);
        path({{7, 8.5f}, {7, 11}, {4.5f, 11}, {4.5f, 21}, {19.5f, 21},
              {19.5f, 11}, {17, 11}, {17, 8.5f}});
        path({{7, 11}, {17, 11}});
        path({{12, 15}, {12, 17}});
        break;
    case Icon::eye:
        path({{2, 12}, {7, 7}, {12, 5}, {17, 7}, {22, 12}, {17, 17}, {12, 19}, {7, 17}, {2, 12}});
        circle(9, 9, 6);
        break;
    case Icon::scan:
        graphics.DrawArc(&pen, 4.f, 4.f, 16.f, 16.f, 222, 315);
        path({{14, 12}, {20, 12}, {20, 6}});
        break;
    case Icon::restore:
        path({{3, 11}, {7, 7}, {11, 11}});
        path({{7, 7}, {7, 13}, {10, 17}, {15, 18}, {20, 15}});
        break;
    case Icon::close:
        path({{6, 6}, {18, 18}});
        path({{18, 6}, {6, 18}});
        break;
    case Icon::check:
        path({{5, 12}, {10, 17}, {19, 7}});
        break;
    case Icon::warning:
        path({{12, 3}, {22, 20}, {2, 20}, {12, 3}});
        path({{12, 9}, {12, 13}});
        graphics.FillEllipse(&brush, 11.f, 15.f, 2.f, 2.f);
        break;
    case Icon::pin:
        path({{8, 3}, {16, 3}, {15, 7}, {15, 10}, {18, 13}, {18, 15},
              {6, 15}, {6, 13}, {9, 10}, {9, 7}, {8, 3}});
        path({{12, 15}, {12, 22}});
        break;
    case Icon::info:
        circle(3, 3, 18);
        graphics.FillEllipse(&brush, 10.8f, 6.5f, 2.4f, 2.4f);
        path({{12, 11}, {12, 17}});
        break;
    case Icon::link:
        path({{7, 8}, {10, 5}, {16, 5}, {19, 8}, {16, 11}, {13, 11}});
        path({{17, 16}, {14, 19}, {8, 19}, {5, 16}, {8, 13}, {11, 13}});
        path({{9, 15}, {15, 9}});
        break;
    case Icon::none: break;
    }
}

void button(const DRAWITEMSTRUCT& item, const Fonts& fonts, UINT dpi, std::wstring_view caption,
            Icon glyph, Role role, bool active, bool hovered) {
    const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
    const auto rect = item.rcItem;
    const auto dc = item.hDC;
    const int height = rect.bottom - rect.top;
    COLORREF ink = disabled ? muted : text;
    COLORREF bg = surface, stroke = line;
    switch (role) {
    case Role::primary:
        bg = disabled ? soft : pressed || hovered ? accent_hover : accent;
        ink = disabled ? muted : rail;
        stroke = CLR_INVALID;
        break;
    case Role::secondary:
        bg = pressed ? mint : hovered ? surface_high : surface;
        break;
    case Role::quiet:
        bg = pressed || hovered ? mint : soft;
        ink = disabled ? muted : accent;
        stroke = CLR_INVALID;
        break;
    case Role::navigation:
    case Role::rail_button:
        bg = active ? mint : pressed || hovered ? rail_panel : rail;
        ink = active ? rail_accent : disabled || role == Role::navigation ? rail_muted : text;
        stroke = role == Role::rail_button ? rail_line : CLR_INVALID;
        break;
    }
    if (focused) stroke = role == Role::navigation || role == Role::rail_button ? rail_accent : accent;
    rounded(dc, rect, px(9), bg, stroke);
    if (active && role == Role::navigation)
        rounded(dc, {rect.left + px(1), rect.top + px(11), rect.left + px(4), rect.bottom - px(11)}, px(2), accent);
    const bool aligned_left = role == Role::navigation || role == Role::rail_button;
    const int icon_size = px(18);
    int x = rect.left + px(13);
    const auto old_font = SelectObject(dc, fonts.strong);
    SIZE size{};
    if (!caption.empty()) GetTextExtentPoint32W(dc, caption.data(), static_cast<int>(caption.size()), &size);
    SelectObject(dc, old_font);
    if (!aligned_left) {
        const int content = size.cx + (glyph != Icon::none ? icon_size + (caption.empty() ? 0 : px(8)) : 0);
        x = std::max(static_cast<int>(rect.left) + px(4), static_cast<int>(rect.left + (rect.right - rect.left - content) / 2));
    }
    if (glyph != Icon::none) {
        icon(dc, glyph, box(x, rect.top + (height - icon_size) / 2, icon_size, icon_size), ink);
        x += icon_size + px(8);
    }
    label(dc, {x, rect.top, rect.right - px(3), rect.bottom}, caption, fonts.strong, ink);
}
}

#pragma once

#include <windows.h>
#include <initializer_list>
#include <string>
#include <string_view>

namespace pk::ui {
inline constexpr COLORREF background = RGB(16, 18, 27);
inline constexpr COLORREF surface = RGB(26, 30, 43);
inline constexpr COLORREF surface_high = RGB(34, 39, 54);
inline constexpr COLORREF surface_low = RGB(20, 24, 35);
inline constexpr COLORREF line = RGB(51, 58, 76);
inline constexpr COLORREF text = RGB(242, 245, 251);
inline constexpr COLORREF muted = RGB(166, 176, 197);
inline constexpr COLORREF accent = RGB(128, 225, 188);
inline constexpr COLORREF accent_hover = RGB(164, 242, 210);
inline constexpr COLORREF mint = RGB(37, 66, 62);
inline constexpr COLORREF mint_line = RGB(72, 146, 123);
inline constexpr COLORREF soft = RGB(31, 36, 50);
inline constexpr COLORREF rail = RGB(20, 23, 34);
inline constexpr COLORREF rail_panel = RGB(28, 33, 47);
inline constexpr COLORREF rail_line = RGB(46, 52, 69);
inline constexpr COLORREF rail_muted = RGB(158, 171, 194);
inline constexpr COLORREF rail_accent = accent;
inline constexpr COLORREF amber = RGB(248, 192, 109);

enum class Icon {
    none, brand, folder, backup, trash, plus, import, search, sort, settings, arrow,
    disk, issues, github, file, more, lock, eye, scan, restore, close, check, warning, pin, info, link
};

enum class Role { primary, secondary, quiet, navigation, rail_button };

struct Fonts {
    HFONT caption{}, body{}, strong{}, card{}, section{}, value{}, title{};
    ~Fonts();
    void reset(UINT dpi);
};

RECT box(int x, int y, int width, int height);
std::wstring window_text(HWND control);
void dark_caption(HWND window);
void fill(HDC dc, RECT rect, COLORREF color);
void rounded(HDC dc, RECT rect, int radius, COLORREF color, COLORREF border = CLR_INVALID);
void label(HDC dc, RECT rect, std::wstring_view value, HFONT font, COLORREF color,
           UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
void icon(HDC dc, Icon glyph, RECT rect, COLORREF color);
void button(const DRAWITEMSTRUCT& item, const Fonts& fonts, UINT dpi, std::wstring_view caption,
            Icon glyph, Role role, bool active, bool hovered);
}

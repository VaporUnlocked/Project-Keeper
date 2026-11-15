#include "dialogs.h"
#include "ui.h"

#include <algorithm>

namespace pk {
namespace {
constexpr int input = 10, description = 11, validation = 12;
constexpr int margin = 18, field_width = 306;

struct Prompt {
    Text caption, hint, value;
    Text action = L"Save", second, source;
    bool secret = false, viewer = false, project = false, accepted = false;
    bool notice = false, confirmation = false;
    UINT dpi = 96;
    ui::Fonts fonts;

    Prompt(Text heading, Text label, Text initial)
        : caption(std::move(heading)), hint(std::move(label)), value(std::move(initial)) {}

    int start() const { return source.empty() ? 43 : 64; }
    int field_y() const { return start() + (notice ? 0 : project ? 19 : 28); }
    int field_height() const { return notice ? 49 : viewer ? 146 : 15; }
    int actions() const {
        if (project) return field_y() + 95;
        return field_y() + field_height() + (viewer ? 14 : 19);
    }
};

INT_PTR CALLBACK prompt_proc(HWND dialog, UINT msg, WPARAM w, LPARAM l) {
    auto* data = reinterpret_cast<Prompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        data = reinterpret_cast<Prompt*>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SetWindowTextW(dialog, data->caption.c_str());
        ui::dark_caption(dialog);
        data->dpi = std::max(96u, GetDpiForWindow(dialog));
        data->fonts.reset(data->dpi);
        const auto create = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                                int x, int y, int width, int height, HFONT font) {
            RECT area{x, y, x + width, y + height};
            MapDialogRect(dialog, &area);
            const auto control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, area.left, area.top,
                area.right - area.left, area.bottom - area.top, dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return control;
        };
        auto& font = data->fonts;
        create(L"STATIC", data->caption.c_str(), SS_ENDELLIPSIS | SS_NOPREFIX, -1,
               margin, 11, field_width, 23, font.value);
        if (!data->source.empty()) {
            create(L"STATIC", data->source.c_str(), SS_PATHELLIPSIS | SS_NOPREFIX, 13,
                   margin, 43, field_width, 15, font.caption);
        }
        if (!data->hint.empty())
            create(L"STATIC", data->hint.c_str(), SS_NOPREFIX, data->project ? -1 : 13,
                   margin, data->start(), field_width, data->project ? 14 : 23, font.body);
        const int field_y = data->field_y();
        auto edit = create(L"EDIT", data->value.c_str(), WS_TABSTOP |
            (data->viewer ? ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL) |
            (data->secret ? ES_PASSWORD : 0), input, margin + 6, field_y + 3, field_width - 12, data->field_height(), font.body);
        if (data->viewer) {
            RECT area{};
            GetClientRect(edit, &area);
            const auto dc = GetDC(edit);
            const auto old = SelectObject(dc, font.body);
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            SelectObject(dc, old);
            ReleaseDC(edit, dc);
            if (SendMessageW(edit, EM_GETLINECOUNT, 0, 0) * metrics.tmHeight <= area.bottom)
                ShowScrollBar(edit, SB_VERT, FALSE);
        }
        if (!data->viewer) SendMessageW(edit, EM_SETLIMITTEXT, data->secret ? 512 : data->project ? 255 : 2048, 0);
        if (data->project) {
            create(L"STATIC", L"Description (optional)", 0, -1, margin, field_y + 34, field_width, 15, font.body);
            const auto second = create(L"EDIT", data->second.c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                                       description, margin + 6, field_y + 55, field_width - 12, 15, font.body);
            SendMessageW(second, EM_SETLIMITTEXT, 2048, 0);
            create(L"STATIC", L"", SS_NOPREFIX, validation, margin, field_y + 77, field_width, 15, font.caption);
        }
        if (!data->viewer || data->confirmation) create(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW,
                                  IDCANCEL, 189, data->actions(), 62, 23, font.strong);
        create(L"BUTTON", data->action.c_str(), WS_TABSTOP | BS_OWNERDRAW,
               IDOK, 260, data->actions(), 64, 23, font.strong);
        const int default_action = data->confirmation ? IDCANCEL : IDOK;
        SendMessageW(dialog, DM_SETDEFID, default_action, 0);
        SetFocus(data->notice ? GetDlgItem(dialog, default_action) : edit);
        SendMessageW(edit, EM_SETSEL, 0, data->viewer ? 0 : -1);
        return FALSE;
    }
    if (!data) return FALSE;
    if (msg == WM_ERASEBKGND) {
        RECT r{};
        GetClientRect(dialog, &r);
        const HDC dc = reinterpret_cast<HDC>(w);
        ui::fill(dc, r, ui::surface);
        RECT divider{margin, 35, margin + field_width, 36};
        MapDialogRect(dialog, &divider);
        ui::fill(dc, divider, ui::line);
        if (!data->notice) {
            for (const int field : {input, description}) {
                const auto control = GetDlgItem(dialog, field);
                RECT frame{};
                if (!control || !GetWindowRect(control, &frame)) continue;
                MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&frame), 2);
                InflateRect(&frame, MulDiv(10, static_cast<int>(data->dpi), 96), MulDiv(6, static_cast<int>(data->dpi), 96));
                const auto border = GetFocus() == control ? ui::mint_line : ui::line;
                ui::rounded(dc, frame, MulDiv(9, static_cast<int>(data->dpi), 96), ui::surface_low, border);
            }
        }
        return TRUE;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
        const auto control = reinterpret_cast<HWND>(l);
        const auto dc = reinterpret_cast<HDC>(w);
        const auto id = GetDlgCtrlID(control);
        SetTextColor(dc, id == validation ? ui::amber : id == 13 ? ui::muted : ui::text);
        const bool field = id == input || id == description;
        const auto bg = field && !data->notice ? ui::surface_low : ui::surface;
        SetBkColor(dc, bg);
        SetDCBrushColor(dc, bg);
        return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
    }
    if (msg == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(l);
        ui::fill(item.hDC, item.rcItem, ui::surface);
        ui::button(item, data->fonts, data->dpi, item.CtlID == IDOK ? data->action : L"Cancel",
                   ui::Icon::none, item.CtlID == IDOK ? ui::Role::primary : ui::Role::secondary, false, false);
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(w) == input || LOWORD(w) == description) &&
        (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) {
        InvalidateRect(dialog, nullptr, TRUE);
        return FALSE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(w) == IDCANCEL)) {
        if (data->secret) SetWindowTextW(GetDlgItem(dialog, input), L"");
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(w) == IDOK) {
        if (!data->viewer) data->value = ui::window_text(GetDlgItem(dialog, input));
        if (data->project && !valid_name(data->value)) {
            SetDlgItemTextW(dialog, validation, L"Enter a valid Windows folder name.");
            SetFocus(GetDlgItem(dialog, input));
            return TRUE;
        }
        if (data->project) data->second = ui::window_text(GetDlgItem(dialog, description));
        if (data->secret) SetWindowTextW(GetDlgItem(dialog, input), L"");
        data->accepted = true;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}

struct alignas(DWORD) DialogTemplate {
    DLGTEMPLATE dialog{};
    WORD menu{}, cls{}, name{}, points = 10;
    wchar_t typeface[9] = L"Segoe UI";
};

DialogTemplate dialog_template(const Prompt& data) {
    DialogTemplate tmpl;
    tmpl.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    tmpl.dialog.cx = 342;
    tmpl.dialog.cy = static_cast<short>(data.actions() + 36);
    return tmpl;
}

bool show_prompt(HWND owner, Prompt& data) {
    const auto tmpl = dialog_template(data);
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.dialog, owner, prompt_proc,
                                reinterpret_cast<LPARAM>(&data)) == -1)
        throw std::runtime_error("Windows could not open a Project Keeper dialog.");
    return data.accepted;
}

}

std::optional<Text> prompt(HWND owner, Text caption, Text label, Text value, bool secret, bool viewer) {
    Prompt data(std::move(caption), std::move(label), std::move(value));
    data.secret = secret;
    data.viewer = viewer;
    if (viewer) data.action = L"Done";
    else if (data.caption == L"Unlock project") data.action = L"Unlock";
    return show_prompt(owner, data) ? std::optional<Text>(std::move(data.value)) : std::nullopt;
}

bool notice(HWND owner, Text caption, Text message, bool confirmation) {
    Prompt data(std::move(caption), {}, std::move(message));
    data.viewer = data.notice = true;
    data.confirmation = confirmation;
    data.action = confirmation ? L"Continue" : L"OK";
    return show_prompt(owner, data);
}
std::optional<Draft> project_prompt(HWND owner, const fs::path& source) {
    Prompt data(source.empty() ? L"New project" : L"Import folder", L"Project name", source.filename().native());
    data.project = true;
    data.action = source.empty() ? L"Create" : L"Import";
    data.source = source.native();
    if (!show_prompt(owner, data)) return std::nullopt;
    return Draft{std::move(data.value), std::move(data.second)};
}

void check_project_prompt(HWND owner) {
    Prompt data(L"New project", L"Project name", L"");
    data.project = true;
    data.action = L"Create";
    const auto tmpl = dialog_template(data);
    const auto dialog = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), &tmpl.dialog, owner,
                                                    prompt_proc, reinterpret_cast<LPARAM>(&data));
    if (!dialog) throw std::runtime_error("The project form could not be created.");
    struct DialogLifetime { HWND hwnd; ~DialogLifetime() { DestroyWindow(hwnd); } } lifetime{dialog};
    if (ui::window_text(GetDlgItem(dialog, IDOK)) != L"Create" || !GetDlgItem(dialog, 11))
        throw std::runtime_error("The project form controls did not initialize.");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    if (data.accepted || ui::window_text(GetDlgItem(dialog, 12)).empty())
        throw std::runtime_error("The project form did not reject an empty name.");
}

}

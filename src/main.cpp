#include "keeper.h"
#include "dialogs.h"
#include "ui.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <tuple>

namespace pk {
namespace {
constexpr auto title = L"Project Keeper";
#ifdef PK_TEST_DRIVER
constexpr auto window_class = L"ProjectKeeper.Tests";
#else
constexpr auto window_class = L"ProjectKeeper.Main.05";
#endif
constexpr auto grid_class = L"ProjectKeeper.Tiles";
constexpr auto registry = L"Software\\ProjectKeeper";
constexpr auto issue_file = L".projectkeeper_issues";
constexpr auto gh_interval = 5u * 60u * 1000u;
constexpr UINT setup_message = WM_APP + 1;
constexpr UINT tray_message = WM_APP + 2;
constexpr UINT restore_message = WM_APP + 3;
constexpr UINT tray_icon_id = 1;
constexpr UINT_PTR tray_retry_timer = 3;
constexpr UINT tray_retry_interval = 2000;

enum Command {
    Choose = 100, Refresh, New, Import, Sync, Token, ShowHidden,
    Projects, Trash, Backups, Open, Rename, Describe, Delete, Restore, Purge,
    Protect, Hide, Context, GitHub, Issues, Review, Backup, Recover, History,
    OpenArea, UpdateContext, OpenGitHub, RemovePasscode, OpenNotes, OpenVault,
    Search, ClearSearch, Sort, More, Settings, Welcome, SortName, SortModified, SortSize,
    Pin, Info, Mcp, TrayMenu, TrayOpen, TrayExit
};
enum class Page { projects, trash, backups };
enum class Order { name, modified, size };

void open_target(HWND window, const Text& target) {
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Windows could not open the requested file, folder, or URL.");
}

void alert(HWND window, const Text& value) {
    try { notice(window, title, value); }
    catch (const std::exception&) { MessageBoxW(window, value.c_str(), title, MB_OK | MB_ICONWARNING); }
}

bool confirm(HWND window, const wchar_t* text) {
    return notice(window, L"Confirm action", text, true);
}

#ifndef PK_TEST_DRIVER
bool elevated() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION status{};
    DWORD bytes{};
    const auto ok = GetTokenInformation(token, TokenElevation, &status, sizeof(status), &bytes);
    CloseHandle(token);
    return ok && status.TokenIsElevated;
}
#endif

struct LaunchOptions {
    fs::path vault;
    bool tray = false;
};

std::optional<LaunchOptions> launch_options(int argc, const wchar_t* const* args) {
    LaunchOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument = args[i];
        if (argument == L"--tray" && !options.tray) {
            options.tray = true;
        } else if (argument == L"--vault" && options.vault.empty() && i + 1 < argc && args[i + 1][0]) {
            options.vault = args[++i];
        } else {
            return std::nullopt;
        }
    }
    return options;
}

bool remember(const fs::path& path) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, registry, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const auto result = RegSetValueExW(key, L"VaultRoot", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.native().size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::optional<fs::path> browse(HWND owner, const wchar_t* caption) {
    IFileOpenDialog* raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw))))
        throw std::runtime_error("Windows could not open the folder chooser.");
    const auto release = [](auto* p) { p->Release(); };
    std::unique_ptr<IFileOpenDialog, decltype(release)> dialog(raw, release);
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(caption);
    const auto shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(shown)) throw std::runtime_error("The folder chooser could not finish.");
    IShellItem* item_raw = nullptr;
    if (FAILED(dialog->GetResult(&item_raw))) return std::nullopt;
    std::unique_ptr<IShellItem, decltype(release)> item(item_raw, release);
    PWSTR value = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) return std::nullopt;
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(value, CoTaskMemFree);
    return fs::path(path.get());
}

Text count(const Project& project, const wchar_t* key) {
    auto value = project.get(key);
    return value.empty() ? L"—" : value;
}

Text repo_key(const Text& value) {
    try { return normalize_repo(value); } catch (const std::exception&) { return value; }
}

std::pair<uintmax_t, uintmax_t> drive(const fs::path& path) {
    ULARGE_INTEGER free{}, total{};
    GetDiskFreeSpaceExW(path.c_str(), nullptr, &total, &free);
    return {free.QuadPart, total.QuadPart};
}

bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return fs::equivalent(a, b, error);
}

std::set<Text> listed_numbers(const Text& value) {
    std::set<Text> result;
    Text number;
    for (auto ch : value) {
        if (ch >= L'0' && ch <= L'9') number += ch;
        else if (!number.empty()) { result.insert(std::move(number)); number.clear(); }
    }
    if (!number.empty()) result.insert(std::move(number));
    return result;
}

struct SyncResult {
    std::map<Text, RepoStats> values;
    std::map<Text, Text> errors;
    Text failure;
};

using ui::Icon;
using ui::Role;
struct Button {
    Command id;
    const wchar_t* caption;
    Icon icon;
    Role role;
    bool icon_only = false;
};

constexpr Button buttons[]{
    {Projects, L"Projects", Icon::folder, Role::navigation},
    {Backups, L"Backups", Icon::backup, Role::navigation},
    {Trash, L"Trash", Icon::trash, Role::navigation},
    {Settings, L"Vault settings", Icon::settings, Role::rail_button},
    {Mcp, L"MCP connection", Icon::link, Role::navigation},
    {Import, L"Import folder", Icon::import, Role::secondary},
    {New, L"New project", Icon::plus, Role::primary},
    {OpenArea, L"Open location", Icon::folder, Role::secondary},
    {Welcome, L"Choose vault folder", Icon::folder, Role::primary},
    {Sort, L"Sort", Icon::sort, Role::secondary},
    {ClearSearch, L"Clear search", Icon::close, Role::quiet, true},
    {More, L"More", Icon::more, Role::secondary},
    {Info, L"Details", Icon::info, Role::secondary},
    {Pin, L"Pin", Icon::pin, Role::secondary},
    {Open, L"Open folder", Icon::arrow, Role::primary},
    {Protect, L"Unlock", Icon::lock, Role::primary},
    {Context, L"Context", Icon::file, Role::secondary},
    {Backup, L"Back up", Icon::backup, Role::secondary},
    {Restore, L"Restore project", Icon::restore, Role::primary},
    {Recover, L"Recover copy", Icon::restore, Role::primary},
};

Text display_name(const Project& row, Page page) {
    if (page == Page::projects) return row.name();
    const auto original = row.get(L"keeper_original");
    if (!original.empty()) return original;
    auto name = row.name();
    const auto split = name.rfind(page == Page::backups ? L"__backup_" : L"__deleted_");
    return split == Text::npos || split == 0 ? name : name.substr(0, split);
}

Text number(uintmax_t value) {
    auto display = std::to_wstring(value);
    for (int at = static_cast<int>(display.size()) - 3; at > 0; at -= 3)
        display.insert(static_cast<size_t>(at), 1, L',');
    return display;
}

std::optional<unsigned long> tally(const Text& value) {
    if (value.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    auto result = wcstoul(value.c_str(), &end, 10);
    return end == value.c_str() + value.size() ? std::optional(result) : std::nullopt;
}

struct JobResult {
    Text message;
    Text selection;
    std::map<fs::path, Stats> measurements;
    bool replace_measurements = false;
    bool warning = false;
    fs::path open_after;

    JobResult(Text status) : message(std::move(status)) {}
};

class App {
    static constexpr int footer_height = 40;
    static constexpr int min_width = 864, min_height = 558;
    HINSTANCE instance;
    HWND window{}, grid{}, search{}, hovered{};
    ui::Fonts fonts;
    std::map<Command, HWND> controls;
    std::unique_ptr<VaultView> vault;
    Page page = Page::projects;
    Order order = Order::name, project_order = Order::name;
    std::vector<Project> rows;
    std::vector<size_t> shown;
    struct Tile { HWND button; size_t index = SIZE_MAX; };
    std::vector<Tile> tile_buttons;
    size_t selection = SIZE_MAX, pinned_count{};
    std::map<fs::path, Stats> measured;
    std::set<fs::path> unlocked;
    std::future<JobResult> job;
    std::shared_ptr<ImportProgress> import_progress;
    std::future<SyncResult> sync;
    std::map<Text, Text> sync_errors;
    Text selected_name, status_text, query;
    Text mcp_revision;
    ULONGLONG next_mcp_check = 0;
    uintmax_t drive_free{}, drive_total{};
    size_t eligible{};
    bool show_hidden = false, pending_sync = false, closing = false, smoke = false, in_command = false;
    bool status_error = false, replacing_search = false;
    bool tray_ready = false, tray_modern = false;
    UINT taskbar_created = 0;
    bool start_in_tray = false;
    UINT dpi = 96;
    fs::path launch_vault;
    int width{}, height{}, library_x{}, library_width{}, action_left{};
    int rail_width = 218, pad = 30;
    int grid_height{}, scroll_offset{}, wheel_delta{}, drag_origin{}, drag_offset{};
    bool dragging_scroll = false;
    static constexpr int grid_top = 177, tile_gap = 15, group_header = 39, tile_id = 4000;

    int px(int n) const { return MulDiv(n, static_cast<int>(dpi), 96); }
    int logical(int n) const { return MulDiv(n, 96, static_cast<int>(dpi)); }
    RECT box(int x, int y, int w, int h) const {
        return {px(x), px(y), px(x + w), px(y + h)};
    }
    HWND child(const wchar_t* cls, const wchar_t* caption, DWORD style, int id = 0) {
        const auto result = CreateWindowExW(0, cls, caption, WS_CHILD | style, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!result) throw std::runtime_error("A window control could not be created.");
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.body), FALSE);
        return result;
    }
    void place(HWND control, int x, int y, int w, int h, bool visible = true) const {
        const bool was_visible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
        if (!visible) {
            if (was_visible && GetFocus() == control) SetFocus(vault ? search : window);
            if (was_visible) ShowWindow(control, SW_HIDE);
            return;
        }
        SetWindowPos(control, nullptr, px(x), px(y), px(w), px(h),
                     SWP_NOACTIVATE | SWP_NOZORDER | (was_visible ? 0 : SWP_SHOWWINDOW));
    }
    void place(Command id, int x, int y, int w, int h, bool visible = true) const {
        place(controls.at(id), x, y, w, h, visible);
    }
    void label(HWND control, const Text& value) {
        if (ui::window_text(control) != value) SetWindowTextW(control, value.c_str());
    }
    bool busy() const { return job.valid(); }
    void invalidate() const { InvalidateRect(window, nullptr, FALSE); }
    void log(Text value, bool error = false) {
        status_text = std::move(value);
        status_error = error;
        invalidate();
    }

    NOTIFYICONDATAW tray_data() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = tray_icon_id;
        return data;
    }
    bool add_tray() {
        if (tray_ready) return true;
        auto data = tray_data();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        wcscpy_s(data.szTip, title);
        tray_ready = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (tray_ready) {
            data.uVersion = NOTIFYICON_VERSION_4;
            tray_modern = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        }
        return tray_ready;
    }
    void remove_tray() {
        if (!tray_ready) return;
        auto data = tray_data();
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_ready = false;
    }
    void refresh_tray() {
        if (closing) return;
        if (add_tray()) KillTimer(window, tray_retry_timer);
        else SetTimer(window, tray_retry_timer, tray_retry_interval, nullptr);
    }
    void open_window() const {
        if (IsIconic(window)) {
            ShowWindow(window, smoke ? SW_SHOWNOACTIVATE : SW_RESTORE);
        } else {
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (!smoke) SetForegroundWindow(window);
    }
    void close_window() {
        if (closing) return;
        if (add_tray()) {
            ShowWindow(window, SW_HIDE);
        } else if (IsWindowVisible(window)) {
            ShowWindow(window, SW_MINIMIZE);
            log(L"The system tray is unavailable. Project Keeper is minimized.", true);
        }
    }
    void exit_application() {
        if (closing) return;
        closing = true;
        KillTimer(window, tray_retry_timer);
        if (!busy() && !sync.valid()) {
            DestroyWindow(window);
            return;
        }
        log(L"Finishing the current operation before exiting…");
        if (tray_ready) {
            auto data = tray_data();
            data.uFlags = NIF_TIP | NIF_SHOWTIP;
            wcscpy_s(data.szTip, L"Project Keeper is finishing before exiting");
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        enable();
    }
    fs::path area() const {
        if (page == Page::trash) return vault->trash();
        if (page == Page::backups) return vault->backups();
        return vault->projects();
    }
    const wchar_t* page_name() const {
        if (page == Page::trash) return L"Trash";
        if (page == Page::backups) return L"Backups";
        return L"Projects";
    }
    Icon page_icon() const {
        if (page == Page::trash) return Icon::trash;
        if (page == Page::backups) return Icon::backup;
        return Icon::folder;
    }
    const Project* selected() const {
        return selection < shown.size() ? &rows[shown[selection]] : nullptr;
    }
    bool access(const Project& p) const { return !p.flag(L"locked") || unlocked.contains(p.path); }
    bool usable(const Project* p) const { return p && !p->flag(L"keeper_read_error") && access(*p); }
    bool has_repo(const Project* p) const { return usable(p) && !p->get(L"repo").empty(); }
    bool pinned(const Project& p) const { return page == Page::projects && p.flag(L"pinned"); }
    int tile_height() const { return page == Page::projects ? 190 : 144; }

    int group_bottom(size_t count) const {
        return count ? group_header + static_cast<int>((count + 2) / 3) * (tile_height() + tile_gap) : 0;
    }
    int content_height() const {
        return group_bottom(pinned_count) + group_bottom(shown.size() - pinned_count);
    }
    int max_scroll() const { return std::max(0, content_height() - grid_height); }
    int tile_width() const { return std::max(1, (library_width - 14 - 2 * tile_gap) / 3); }
    RECT tile_rect(size_t index) const {
        const bool in_pins = index < pinned_count;
        const size_t local = in_pins ? index : index - pinned_count;
        const int x = static_cast<int>(local % 3) * (tile_width() + tile_gap);
        const int y = (in_pins ? 0 : group_bottom(pinned_count)) + group_header +
                      static_cast<int>(local / 3) * (tile_height() + tile_gap);
        return {x, y, x + tile_width(), y + tile_height()};
    }
    RECT scroll_thumb() const {
        if (!max_scroll()) return {};
        const int track = grid_height - 8;
        const int thumb = std::max(30, MulDiv(track, grid_height, content_height()));
        const int y = 4 + MulDiv(track - thumb, scroll_offset, max_scroll());
        return box(library_width - 7, y, 5, thumb);
    }
    const wchar_t* sort_title() const {
        if (order == Order::size) return L"Largest";
        if (order == Order::modified) return page == Page::projects ? L"Last modified" : L"Newest first";
        return L"A–Z";
    }

    Text tile_name(const Project& row) const {
        Text name = display_name(row, page);
        if (row.flag(L"keeper_read_error")) name += L", unavailable";
        else if (!access(row)) name += L", locked";
        if (pinned(row)) name += L", pinned";
        if (row.flag(L"hidden")) name += L", hidden";
        return name;
    }

    void position_tiles(bool focus_selected = false) {
        if (!grid) return;
        scroll_offset = std::clamp(scroll_offset, 0, max_scroll());
        // The native buttons represent only the visible portion of the board.
        const auto indices = std::views::iota(size_t{0}, shown.size());
        const auto first = std::ranges::upper_bound(indices, scroll_offset, {}, [this](size_t index) {
            return tile_rect(index).bottom;
        });
        const size_t first_index = static_cast<size_t>(first - indices.begin());
        size_t end = first_index;
        while (end < shown.size() && tile_rect(end).top < scroll_offset + grid_height) ++end;
        const auto focus = GetFocus();
        const bool was_on_tile = focus && GetParent(focus) == grid;
        const size_t tab_index = selection >= first_index && selection < end ? selection : first_index;
        HWND tab_target{};
        for (size_t index = first_index; index < end; ++index) {
            const size_t slot = index - first_index;
            if (slot == tile_buttons.size()) {
                const auto button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW | BS_NOTIFY,
                    0, 0, 0, 0, grid, reinterpret_cast<HMENU>(static_cast<INT_PTR>(tile_id + slot)), instance, nullptr);
                if (!button) throw std::runtime_error("Could not create a project tile.");
                SetWindowSubclass(button, track_button, 1, reinterpret_cast<DWORD_PTR>(this));
                tile_buttons.push_back({button});
            }
            auto& tile = tile_buttons[slot];
            tile.index = index;
            label(tile.button, tile_name(rows[shown[index]]));
            const auto style = GetWindowLongPtrW(tile.button, GWL_STYLE);
            SetWindowLongPtrW(tile.button, GWL_STYLE, index == tab_index ? style | WS_TABSTOP : style & ~WS_TABSTOP);
            const auto rect = tile_rect(index);
            SetWindowPos(tile.button, nullptr, px(rect.left), px(rect.top - scroll_offset),
                         px(rect.right) - px(rect.left), px(rect.bottom) - px(rect.top),
                         SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER);
            InvalidateRect(tile.button, nullptr, FALSE);
            if (index == tab_index) tab_target = tile.button;
        }
        if ((focus_selected || was_on_tile) && vault) {
            const auto target = tab_target ? tab_target : search;
            if (GetFocus() != target) SetFocus(target);
        }
        for (size_t slot = end - first_index; slot < tile_buttons.size(); ++slot) {
            auto& tile = tile_buttons[slot];
            tile.index = SIZE_MAX;
            ShowWindow(tile.button, SW_HIDE);
        }
        InvalidateRect(grid, nullptr, FALSE);
    }

    void reveal_selection() {
        if (selection >= shown.size()) return;
        const auto rect = tile_rect(selection);
        const auto group_start = selection < pinned_count ? 0 : pinned_count;
        if (rect.top < scroll_offset)
            scroll_offset = std::max(0L, rect.top - (selection - group_start < 3 ? group_header : 0));
        else if (rect.bottom > scroll_offset + grid_height) scroll_offset = rect.bottom - grid_height;
    }
    void select_tile(size_t index, bool focus = false) {
        if (index >= shown.size()) return;
        selection = index;
        if (focus) reveal_selection();
        layout();
        if (focus) position_tiles(true);
        enable();
    }
    void scroll(int value) {
        const int next = std::clamp(value, 0, max_scroll());
        if (next == scroll_offset) return;
        scroll_offset = next;
        position_tiles();
    }

    int search_width() const { return std::min(448, library_width - 262); }
    void layout() {
        RECT rect{};
        GetClientRect(window, &rect);
        width = logical(rect.right);
        height = logical(rect.bottom);
        rail_width = width < 1000 ? 184 : 212;
        pad = width < 1000 ? 23 : 34;
        library_x = rail_width + pad;
        library_width = width - library_x - pad;
        grid_height = std::max(0, height - grid_top - 106);

        place(Projects, 14, 113, rail_width - 28, 43);
        place(Backups, 14, 162, rail_width - 28, 43);
        place(Trash, 14, 211, rail_width - 28, 43);
        place(Mcp, 14, height - 113, rail_width - 28, 43);
        place(Settings, 14, height - 59, rail_width - 28, 43);
        place(Welcome, library_x + 34, 300, 188, 42, !vault);
        place(New, width - pad - 136, 38, 136, 40, vault && page == Page::projects);
        place(Import, width - pad - 283, 38, 137, 40, vault && page == Page::projects);
        place(OpenArea, width - pad - 146, 38, 146, 40, vault && page != Page::projects);
        place(Sort, width - pad - 156, 111, 156, 42, !!vault);
        place(search, library_x + 40, 122, search_width() - 76, 23, !!vault);
        place(ClearSearch, library_x + search_width() - 32, 117, 26, 28, vault && !query.empty());
        place(grid, library_x, grid_top, library_width, grid_height, !!vault);

        const auto* row = selected();
        const bool allowed = usable(row);
        const bool locked = row && !row->flag(L"keeper_read_error") && !allowed;
        int x = width - pad - 10;
        const int y = height - 93;
        const auto action = [&](Command id, int size, bool visible) {
            if (visible) x -= size;
            place(id, x, y, size, 36, visible);
            if (visible) x -= 7;
        };
        action(Open, 120, allowed && page == Page::projects);
        action(Protect, 106, locked);
        action(Restore, 149, allowed && page == Page::trash);
        action(Recover, 145, allowed && page == Page::backups);
        action(More, 76, allowed);
        action(Info, 91, allowed);
        action(Pin, 82, allowed && page == Page::projects);
        action(Backup, 90, allowed && page == Page::projects && library_width >= 870);
        action(Context, 88, allowed && page == Page::projects && library_width >= 870);
        action_left = x;
        label(controls.at(Sort), sort_title());
        label(controls.at(Pin), row && pinned(*row) ? L"Unpin" : L"Pin");
        position_tiles();
        invalidate();
    }

    void enable() {
        const auto* row = selected();
        const bool ready = vault && !busy() && !closing;
        for (const auto& [id, hwnd] : controls) {
            bool allowed = ready;
            if (id == Settings || id == Mcp) allowed = !closing;
            else if (id == Welcome) allowed = !busy() && !closing;
            else if (id == Sort || id == ClearSearch) allowed = vault && !closing;
            else if (id == Protect) allowed = ready && row && !row->flag(L"keeper_read_error");
            else if (id == More || id == Open || id == Context || id == Backup || id == Restore ||
                     id == Recover || id == Pin || id == Info) allowed = ready && usable(row);
            if ((IsWindowEnabled(hwnd) != FALSE) != allowed) EnableWindow(hwnd, allowed);
        }
    }

    void filter() {
        const Text previous = selected_name.empty() && selected() ? selected()->name() : selected_name;
        selected_name.clear();
        eligible = pinned_count = 0;
        shown.clear();
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (page == Page::projects && row.flag(L"hidden") && !show_hidden) continue;
            ++eligible;
            if (contains_text(display_name(row, page), query) ||
                (usable(&row) && (contains_text(row.get(L"description"), query) || contains_text(row.get(L"repo"), query)))) {
                shown.push_back(i);
                if (pinned(row)) ++pinned_count;
            }
        }
        const auto stat = [&](const Project& row) {
            const auto it = measured.find(row.path);
            return usable(&row) && it != measured.end() ? it->second : Stats{};
        };
        std::stable_sort(shown.begin(), shown.end(), [&](size_t a, size_t b) {
            const auto& first = rows[a];
            const auto& second = rows[b];
            if (page == Page::projects) {
                if (pinned(first) != pinned(second)) return pinned(first);
                if (order == Order::size && stat(first).bytes != stat(second).bytes)
                    return stat(first).bytes > stat(second).bytes;
                if (order == Order::modified && stat(first).modified != stat(second).modified)
                    return stat(first).modified > stat(second).modified;
            } else if (order == Order::modified) {
                const auto date = [](const Project& p) {
                    const auto name = p.name();
                    const auto split = name.rfind(L"_");
                    if (split == Text::npos || split + 16 > name.size()) return Text{};
                    return name.substr(split + 1, 15);
                };
                if (date(first) != date(second)) return date(first) > date(second);
            }
            const auto first_name = display_name(first, page);
            const auto second_name = display_name(second, page);
            return CompareStringOrdinal(first_name.c_str(), -1, second_name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        const auto found = std::ranges::find_if(shown, [&](size_t index) { return rows[index].name() == previous; });
        selection = shown.empty() ? SIZE_MAX : found == shown.end() ? 0 : static_cast<size_t>(found - shown.begin());
        reveal_selection();
        layout();
        enable();
    }

    void reload() {
        if (selected_name.empty() && selected()) selected_name = selected()->name();
        shown.clear();
        rows = vault ? vault->list(area()) : std::vector<Project>{};
        if (vault) std::tie(drive_free, drive_total) = drive(vault->root());
        filter();
    }

    void set_page(Page next) {
        page = next;
        order = page == Page::projects ? project_order : Order::modified;
        selection = SIZE_MAX;
        selected_name.clear();
        shown.clear();
        replacing_search = true;
        label(search, L"");
        replacing_search = false;
        query.clear();
        scroll_offset = 0;
        const wchar_t* hint = page == Page::projects ? L"Search projects" :
                              page == Page::backups ? L"Search backups" : L"Search Trash";
        SendMessageW(search, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(hint));
        reload();
        for (const auto id : {Projects, Backups, Trash}) InvalidateRect(controls.at(id), nullptr, FALSE);
    }


    void paint(HDC target) {
        const auto canvas = box(0, 0, width, height);
        const HDC buffer = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, canvas.right, canvas.bottom);
        const auto previous = bitmap && buffer ? SelectObject(buffer, bitmap) : nullptr;
        draw_workspace(previous ? buffer : target);
        if (previous) {
            BitBlt(target, 0, 0, canvas.right, canvas.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
    }

    void draw_workspace(HDC dc) const {
        using namespace ui;
        const auto word = [&](int x, int y, int w, int h, std::wstring_view value, HFONT type, COLORREF ink = ui::text) {
            ui::label(dc, box(x, y, w, h), value, type, ink);
        };
        const auto card = [&](int x, int y, int w, int h, COLORREF bg = ui::surface, COLORREF stroke = ui::line) {
            rounded(dc, box(x, y, w, h), px(13), bg, stroke);
        };
        fill(dc, box(0, 0, width, height), background);
        fill(dc, box(0, 0, rail_width, height), rail);
        fill(dc, box(rail_width - 1, 0, 1, height), rail_line);
        rounded(dc, box(18, 23, 39, 39), px(11), accent);
        icon(dc, Icon::brand, box(25, 30, 25, 25), rail);
        word(68, 21, rail_width - 77, 44, title, width < 1000 ? fonts.strong : fonts.section);
        fill(dc, box(17, 78, rail_width - 34, 1), rail_line);
        word(20, 88, rail_width - 40, 18, L"Vault", fonts.caption, rail_muted);

        const int storage_y = height - 238;
        card(15, storage_y, rail_width - 30, 104, surface_low, rail_line);
        icon(dc, Icon::disk, box(27, storage_y + 12, 17, 17), accent);
        word(51, storage_y + 8, rail_width - 78, 25, L"Drive storage", fonts.strong);
        word(27, storage_y + 34, rail_width - 54, 21,
             drive_total ? format_size(drive_free) + L" free" : vault ? Text(L"Space unavailable") : Text(L"No vault selected"),
             fonts.body, rail_muted);
        if (drive_total) {
            const int bar_width = rail_width - 54;
            const auto used = drive_total - std::min(drive_free, drive_total);
            const int filled = static_cast<int>(static_cast<double>(bar_width) * static_cast<double>(used) / static_cast<double>(drive_total));
            rounded(dc, box(27, storage_y + 64, bar_width, 5), px(3), rail_line);
            if (filled > 0) rounded(dc, box(27, storage_y + 64, filled, 5), px(3), accent);
            word(27, storage_y + 73, bar_width, 17, format_size(drive_total) + L" total", fonts.caption, rail_muted);
        }

        fill(dc, box(rail_width, height - footer_height, width - rail_width, 1), line);
        const auto idle = vault ? Text(L"Vault ready") : Text(L"Local workspace");
        const auto status_icon = status_error ? Icon::warning : busy() || sync.valid() ? Icon::scan : Icon::check;
        icon(dc, status_icon, box(library_x, height - 28, 16, 16), status_error ? amber : accent);
        if (import_progress && !closing) {
            const auto stage = import_progress->stage.load();
            const auto bytes = import_progress->bytes_total.load();
            const auto files = import_progress->files_total.load();
            const auto copied = std::min(bytes, import_progress->bytes_copied.load());
            const auto finished = std::min(files, import_progress->files_copied.load());
            const auto fraction = stage == ImportStage::finishing ? 1.0 :
                bytes ? static_cast<double>(copied) / static_cast<double>(bytes) :
                files ? static_cast<double>(finished) / static_cast<double>(files) : 0.0;
            const auto summary = stage == ImportStage::preparing ? Text(L"Preparing import…") :
                                 stage == ImportStage::finishing ? Text(L"Finishing import…") :
                                 L"Importing   ·   " + number(finished) + L" / " + number(files) + L" files   ·   " +
                                 format_size(copied) + L" / " + format_size(bytes);
            word(library_x + 24, height - 36, library_width - 80, 23, summary, fonts.caption, ui::text);
            rounded(dc, box(library_x, height - 10, library_width, 5), px(3), line);
            if (stage == ImportStage::preparing) {
                const int bar = std::min(90, library_width / 4);
                const auto travel = static_cast<unsigned>(std::max(1, library_width - bar));
                const int position = static_cast<int>((GetTickCount64() / 12) % travel);
                if (bar > 0) rounded(dc, box(library_x + position, height - 10, bar, 5), px(3), accent);
            } else {
                const int bar = static_cast<int>(library_width * fraction);
                if (bar > 0) rounded(dc, box(library_x, height - 10, bar, 5), px(3), accent);
                ui::label(dc, box(width - pad - 56, height - 36, 56, 23),
                          std::to_wstring(static_cast<int>(fraction * 100)) + L"%", fonts.caption, accent,
                          DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            }
        } else {
            word(library_x + 24, height - 34, library_width - 24, 27, status_text.empty() ? idle : status_text,
                 fonts.caption, status_error ? amber : muted);
        }

        if (!vault) {
            word(library_x, 25, library_width, 53, L"Open your vault", fonts.title);
            card(library_x, 106, library_width, 281);
            rounded(dc, box(library_x + 34, 139, 54, 54), px(13), mint);
            icon(dc, Icon::folder, box(library_x + 47, 152, 28, 28), accent);
            word(library_x + 34, 203, library_width - 68, 41, L"Choose a folder", fonts.value);
            word(library_x + 34, 252, library_width - 68, 24,
                 L"Select a dedicated folder to store your projects, backups, and Trash.", fonts.body, muted);
            return;
        }

        word(library_x, 19, library_width - 288, 57, page_name(), fonts.title);
        icon(dc, Icon::folder, box(library_x, 82, 16, 16), accent);
        ui::label(dc, box(library_x + 23, 76, library_width - 23, 28), vault->root().native(), fonts.body, muted,
                  DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        rounded(dc, box(library_x, 111, search_width(), 42), px(10), soft, GetFocus() == search ? accent : line);
        icon(dc, Icon::search, box(library_x + 13, 122, 18, 18), muted);
        const int count_x = library_x + search_width() + 15;
        const int sort_label_x = width - pad - 204;
        word(sort_label_x, 111, 45, 42, L"Sort by", fonts.caption, muted);
        const int count_w = sort_label_x - count_x - 7;
        if (!query.empty() && count_w >= 65)
            word(count_x, 111, count_w, 42, number(shown.size()) + L" of " + number(eligible), fonts.caption, muted);

        const auto* row = selected();
        if (!row) return;
        card(library_x, height - 103, library_width, 56, surface);
        const bool allowed = usable(row);
        rounded(dc, box(library_x + 11, height - 94, 37, 37), px(10), mint);
        icon(dc, allowed ? page_icon() : Icon::lock, box(library_x + 20, height - 85, 19, 19), accent);
        const int label_x = library_x + 59;
        const int size = std::max(0, action_left - label_x - 8);
        word(label_x, height - 100, size, 27, display_name(*row, page), fonts.strong);
        const wchar_t* status = row->flag(L"keeper_read_error") ? L"Metadata unavailable" :
                                !allowed ? L"Passcode locked" : pinned(*row) ? L"Pinned project" :
                                page == Page::backups ? L"Backup" : page == Page::trash ? L"Deleted project" : L"Selected project";
        word(label_x, height - 78, size, 21, status, fonts.caption, muted);
    }

    void draw_grid(HDC dc) const {
        using namespace ui;
        fill(dc, box(0, 0, library_width, grid_height), background);
        const auto group = [&](int y, const wchar_t* heading, size_t count, bool pin) {
            y -= scroll_offset;
            if (y + group_header < 0 || y >= grid_height) return;
            const int left = pin ? 25 : 1;
            if (pin) icon(dc, Icon::pin, box(2, y + 6, 16, 16), accent);
            const auto font = SelectObject(dc, fonts.strong);
            SIZE label_size{};
            GetTextExtentPoint32W(dc, heading, static_cast<int>(wcslen(heading)), &label_size);
            SelectObject(dc, font);
            const int badge_x = left + logical(label_size.cx) + 10;
            ui::label(dc, box(left, y + 1, badge_x - left, 26), heading, fonts.strong, pin ? accent : ui::text);
            const auto value = number(count);
            const int badge_w = std::max(24, static_cast<int>(value.size()) * 8 + 12);
            rounded(dc, box(badge_x, y + 5, badge_w, 19), px(7), pin ? mint : surface_high);
            ui::label(dc, box(badge_x, y + 5, badge_w, 19), value, fonts.caption, pin ? accent : muted,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            const int line_x = badge_x + badge_w + 14;
            fill(dc, box(line_x, y + 15, library_width - line_x - 15, 1), line);
        };
        if (pinned_count) group(0, L"Pinned", pinned_count, true);
        if (shown.size() > pinned_count) {
            const auto label = page == Page::projects ? L"Projects" : page == Page::backups ? L"Snapshots" : L"Deleted projects";
            group(group_bottom(pinned_count), label, shown.size() - pinned_count, false);
        }
        if (shown.empty()) {
            const int y = std::max(22, grid_height / 2 - 65);
            rounded(dc, box((library_width - 52) / 2, y, 52, 52), px(12), soft, line);
            icon(dc, query.empty() ? page_icon() : Icon::search, box((library_width - 24) / 2, y + 14, 24, 24), accent);
            constexpr UINT centered = DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            const auto empty_heading = !query.empty() ? L"No matches" : page == Page::projects ?
                L"No projects" : page == Page::backups ? L"No backups yet" : L"Trash is empty";
            const auto hint = !query.empty() ? L"Try a different search." : page == Page::projects ?
                L"Create a project or import a folder to get started." : page == Page::backups ?
                L"Choose a project and select Back up to save a copy." : L"Projects moved to Trash appear here.";
            ui::label(dc, box(15, y + 56, library_width - 30, 35), empty_heading, fonts.section, ui::text, centered);
            ui::label(dc, box(15, y + 92, library_width - 30, 22), hint, fonts.body, muted, centered);
        }
        if (max_scroll()) {
            rounded(dc, box(library_width - 7, 4, 5, grid_height - 8), px(3), soft);
            rounded(dc, scroll_thumb(), px(3), dragging_scroll ? accent : mint_line);
        }
    }

    Text deleted_at(const Project& project) const {
        const auto name = project.name();
        const auto marker = name.rfind(L"__deleted_");
        if (marker == Text::npos || marker + 18 > name.size()) return {};
        const auto date = name.substr(marker + 10, 8);
        return date.substr(0, 4) + L"-" + date.substr(4, 2) + L"-" + date.substr(6);
    }

    Text tile_github_summary(const Project& row, bool sync_failed) const {
        if (row.get(L"issues").empty() && row.get(L"stars").empty() && row.get(L"forks").empty())
            return sync_failed ? L"GitHub sync failed" : L"Waiting for GitHub sync";
        return count(row, L"issues") + L" open  ·  " + count(row, L"stars") + L" stars  ·  " + count(row, L"forks") + L" forks";
    }

    void draw_tile(const DRAWITEMSTRUCT& item) const {
        using namespace ui;
        const auto slot = static_cast<size_t>(item.CtlID - tile_id);
        if (slot >= tile_buttons.size()) return;
        const size_t index = tile_buttons[slot].index;
        if (index >= shown.size()) return;
        const auto& row = rows[shown[index]];
        const bool active = index == selection;
        const bool pointed = hovered == item.hwndItem;
        const bool focused = GetFocus() == item.hwndItem;
        const bool valid = !row.flag(L"keeper_read_error");
        const bool allowed = usable(&row);
        const HDC dc = item.hDC;
        const int w = logical(item.rcItem.right), h = logical(item.rcItem.bottom);
        fill(dc, item.rcItem, background);
        rounded(dc, item.rcItem, px(14), active ? RGB(29, 43, 51) : pointed ? surface_high : surface,
                focused ? accent : active || pinned(row) ? mint_line : line);
        const auto word = [&](int x, int y, int ww, int hh, std::wstring_view value, HFONT font,
                              COLORREF ink = ui::text, UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) {
            ui::label(dc, box(x, y, ww, hh), value, font, ink, flags);
        };
        const auto tint = page == Page::backups ? RGB(180, 188, 249) : page == Page::trash ? amber : accent;
        const auto chip = page == Page::backups ? RGB(49, 51, 82) : page == Page::trash ? RGB(63, 49, 36) : mint;
        rounded(dc, box(15, 15, 39, 39), px(11), chip);
        icon(dc, !valid ? Icon::warning : !allowed ? Icon::lock : page_icon(),
             box(24, 24, 21, 21), !valid ? amber : tint);
        if (pinned(row)) {
            rounded(dc, box(w - 44, 22, 29, 25), px(8), mint);
            icon(dc, Icon::pin, box(w - 37, 27, 15, 15), accent);
        }
        const bool linked = page == Page::projects && has_repo(&row);
        const auto unread = linked ? tally(row.get(L"new_issues")) : std::nullopt;
        if (unread && *unread) {
            const int x = w - (pinned(row) ? 117 : 79);
            rounded(dc, box(x, 22, 64, 25), px(8), soft);
            word(x, 22, 64, 25, number(*unread) + L" new", fonts.caption, accent,
                 DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        word(15, 59, w - 30, 27, display_name(row, page), fonts.card);
        Text secondary, left, right;
        if (!valid) secondary = L"Metadata unavailable";
        else if (!allowed) secondary = L"Passcode locked";
        else if (page == Page::projects) {
            secondary = row.get(L"description");
            if (row.flag(L"hidden")) secondary = L"Hidden" + (secondary.empty() ? Text{} : L"  ·  " + secondary);
            const auto it = measured.find(row.path);
            if (it != measured.end()) {
                left = format_size(it->second.bytes);
                if (it->second.modified != fs::file_time_type::min()) right = format_time(it->second.modified).substr(0, 10);
            } else left = busy() ? L"Awaiting scan" : L"Size unavailable";
        } else {
            secondary = row.get(L"description");
            const auto time = page == Page::backups ? row.get(L"keeper_snapshot").substr(0, 10) : deleted_at(row);
            left = page == Page::backups ? L"Snapshot" : L"In Trash";
            right = time;
        }
        if (page == Page::projects && !linked) {
            word(15, 90, w - 30, 50, secondary, fonts.body, muted,
                 DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            word(15, 86, w - 30, 19, secondary, fonts.caption, muted);
        }
        if (linked) {
            const auto repo = row.get(L"repo");
            const bool sync_failed = sync_errors.contains(repo_key(repo));
            const int y = secondary.empty() ? 91 : 108;
            rounded(dc, box(13, y, w - 26, 43), px(9), surface_low);
            icon(dc, Icon::github, box(21, y + 5, 15, 15), accent);
            word(42, y + 2, w - (sync_failed ? 82 : 61), 21, repo, fonts.body);
            if (sync_failed) icon(dc, Icon::warning, box(w - 34, y + 6, 14, 14), amber);
            word(21, y + 23, w - 42, 17, tile_github_summary(row, sync_failed), fonts.caption, muted);
        }
        fill(dc, box(15, h - 33, w - 30, 1), active ? mint_line : line);
        word(15, h - 30, w - 30 - (right.empty() ? 0 : 77), 25, left, fonts.caption, muted);
        word(w - 93, h - 30, 79, 25, right, fonts.caption, muted,
             DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    void project_details(const Project& row) {
        const auto name = display_name(row, page);
        Text body = row.get(L"description");
        if (!body.empty()) body += L"\r\n\r\n";
        body += L"Folder\r\n" + row.path.native();
        if (const auto it = measured.find(row.path); it != measured.end()) {
            body += L"\r\n\r\nSize  " + format_size(it->second.bytes) + L"\r\nFiles  " + number(it->second.files) +
                    L"\r\nLast modified  " + format_time(it->second.modified);
            if (it->second.skipped) body += L"\r\nEntries skipped during scan  " + number(it->second.skipped);
        }
        if (!row.get(L"keeper_snapshot").empty()) body += L"\r\n\r\nBackup saved  " + row.get(L"keeper_snapshot");
        if (page == Page::projects && !row.get(L"repo").empty()) {
            const auto repo = row.get(L"repo");
            body += L"\r\n\r\nGitHub  " + repo + L"\r\nOpen issues  " + count(row, L"issues") +
                    L"\r\nUnread issues  " + count(row, L"new_issues") + L"\r\nStars  " + count(row, L"stars") +
                    L"   ·   Forks  " + count(row, L"forks") + L"\r\nLast sync  " + count(row, L"gh_updated");
            if (const auto error = sync_errors.find(repo_key(repo)); error != sync_errors.end()) body += L"\r\n" + error->second;
        }
        prompt(window, L"Project details", name, body, false, true);
    }


    void set_vault(const fs::path& path, bool persist) {
        if (vault && same_folder(vault->root(), path)) return;
        std::unique_ptr<VaultView> opened;
        try {
            opened = std::make_unique<VaultView>(path);
        } catch (const std::exception&) {
            const Vault setup(path);
            opened = std::make_unique<VaultView>(setup.root());
        }
        const bool remembered = !persist || remember(opened->root());
        vault = std::move(opened);
        mcp_revision.clear();
        next_mcp_check = 0;
        unlocked.clear();
        measured.clear();
        sync_errors.clear();
        show_hidden = false;
        set_page(Page::projects);
        log(remembered ? L"Vault opened." : L"Vault opened. Windows could not remember this folder.", !remembered);
    }

    void scan() {
        run(L"Scanning projects and updating context…", [](const Vault& target) {
            size_t errors = 0;
            JobResult result(L"Project sizes and context updated.");
            result.replace_measurements = true;
            for (const auto& row : target.list(target.projects())) {
                try {
                    const auto info = target.scan(row);
                    result.measurements[row.path] = info;
                    if (!row.flag(L"keeper_read_error")) target.context(row, info);
                } catch (const std::exception&) { ++errors; }
            }
            if (errors) {
                result.message = L"Scan finished with " + number(errors) + L" unreadable projects or context files.";
                result.warning = true;
            }
            return result;
        });
    }

    void run(const Text& message, auto fn) {
        log(message);
        job = std::async(std::launch::async, [root = vault->root(), fn = std::move(fn)]() -> JobResult {
            const Vault writer(root, Vault::Open::existing, 3000);
            return fn(writer);
        });
        enable();
    }

    auto edit_project(const Project& snapshot, auto fn) {
        const Vault writer(vault->root(), Vault::Open::existing);
        auto current = writer.load(snapshot.path);
        if (!access(current)) throw std::runtime_error("The project is locked.");
        return fn(writer, current);
    }

    void refresh_mcp_changes() {
        if (!vault || busy() || closing || GetTickCount64() < next_mcp_check) return;
        next_mcp_check = GetTickCount64() + 1000;
        try {
            const auto revision = read_text(vault->root() / L".keeper" / mcp_change_file, 128);
            if (revision == mcp_revision) return;
            reload();
            mcp_revision = revision;
        } catch (const std::exception&) {
            // Retry the next time the vault is available.
        }
    }

    void start_sync(bool requested = false) {
        if (!vault || closing || smoke) return;
        if (sync.valid() || busy()) { pending_sync = true; return; }
        pending_sync = false;
        std::set<Text> repos;
        for (const auto& p : vault->list(vault->projects())) {
            if (!p.get(L"repo").empty()) repos.insert(repo_key(p.get(L"repo")));
        }
        if (repos.empty()) {
            if (requested) log(L"Link a repository to enable GitHub sync.");
            return;
        }
        log(L"Synchronizing linked repositories…");
        sync = std::async(std::launch::async, [repos = std::move(repos)] {
            SyncResult result;
            try { result.values = github_sync(repos, github_token(), result.errors); }
            catch (const std::exception& error) { result.failure = error_text(error); }
            return result;
        });
        enable();
    }

    void apply_sync(const Vault& writer, SyncResult result) {
        sync_errors = std::move(result.errors);
        if (!result.failure.empty()) { log(result.failure, true); return; }
        for (auto project : writer.list(writer.projects())) {
            const auto repo = repo_key(project.get(L"repo"));
            if (auto found = result.values.find(repo); found != result.values.end()) {
                try {
                    auto cache = read_fields(project.path / issue_file);
                    if (cache.contains(L"repo") && repo_key(cache[L"repo"]) != repo) cache.clear();
                    auto update = issue_update(std::move(cache), found->second);
                    update.cache[L"repo"] = repo;
                    write_fields(project.path / issue_file, update.cache);
                    project.fields[L"stars"] = std::to_wstring(found->second.stars);
                    project.fields[L"forks"] = std::to_wstring(found->second.forks);
                    project.fields[L"issues"] = std::to_wstring(update.open);
                    project.fields[L"new_issues"] = std::to_wstring(update.unread);
                    project.fields[L"gh_updated"] = timestamp();
                    writer.save(project);
                } catch (const std::exception& error) { sync_errors[repo] = error_text(error); }
            }
        }
        reload();
        log(sync_errors.empty() ? L"GitHub synchronization completed."
                                : L"GitHub sync finished with errors. See the project's GitHub panel.", !sync_errors.empty());
    }

    void poll() {
        using namespace std::chrono_literals;
        if (in_command) return;
        if (job.valid() && job.wait_for(0ms) == std::future_status::ready) {
            import_progress.reset();
            try {
                auto result = job.get();
                if (result.replace_measurements) measured.clear();
                for (auto& [path, stats] : result.measurements) measured.insert_or_assign(path, stats);
                selected_name = std::move(result.selection);
                log(std::move(result.message), result.warning);
                if (!closing && !result.open_after.empty()) open_target(window, result.open_after.native());
            } catch (const std::exception& error) {
                log(error_text(error), true);
                if (!closing && IsWindowVisible(window)) alert(window, error_text(error));
            }
            if (!closing) reload();
        }
        if (import_progress && IsWindowVisible(window)) {
            const auto footer = box(rail_width, height - footer_height, width - rail_width, footer_height);
            InvalidateRect(window, &footer, FALSE);
        }
        if (!busy() && sync.valid() && sync.wait_for(0ms) == std::future_status::ready) {
            if (closing) {
                sync.get();
            } else {
                try {
                    const Vault writer(vault->root(), Vault::Open::existing);
                    apply_sync(writer, sync.get());
                } catch (const VaultBusy&) {
                    // Retain the completed sync result until the current writer finishes.
                } catch (const std::exception& error) {
                    if (sync.valid()) sync.get();
                    log(error_text(error), true);
                }
            }
        }
        if (closing && !busy() && !sync.valid()) { DestroyWindow(window); return; }
        if (pending_sync && !busy() && !sync.valid()) start_sync();
        refresh_mcp_changes();
        enable();
    }

    static JobResult refresh_context(const Vault& writer, const Project& p, Text message, bool select = false) {
        JobResult result(std::move(message));
        if (select) result.selection = p.name();
        try {
            const auto stats = writer.scan(p);
            result.measurements.emplace(p.path, stats);
            writer.context(p, stats);
        } catch (const std::exception& error) {
            if (result.message.empty()) throw;
            result.message += L" Context update failed. " + error_text(error);
            result.warning = true;
        }
        return result;
    }

    void change_passcode(Project& p) {
        if (p.flag(L"locked")) {
            if (access(p)) {
                unlocked.erase(p.path);
                filter();
                return;
            }
            auto pass = prompt(window, L"Unlock project", L"Enter the project passcode.", {}, true);
            if (!pass) return;
            bool verified = false;
            try { verified = Vault::verify(p, *pass); }
            catch (...) { SecureZeroMemory(pass->data(), pass->size() * sizeof(wchar_t)); throw; }
            SecureZeroMemory(pass->data(), pass->size() * sizeof(wchar_t));
            if (!verified) { alert(window, L"Incorrect passcode."); return; }
            unlocked.insert(p.path);
            filter();
            return;
        }
        auto pass = prompt(window, L"Set project passcode", L"Use 6 or more characters. This protects controls inside Project Keeper.", {}, true);
        if (!pass) return;
        auto again = prompt(window, L"Confirm passcode", L"Enter the passcode again.", {}, true);
        const bool match = again && *pass == *again;
        if (again) SecureZeroMemory(again->data(), again->size() * sizeof(wchar_t));
        try { if (match) Vault::lock_project(p, *pass); }
        catch (...) { SecureZeroMemory(pass->data(), pass->size() * sizeof(wchar_t)); throw; }
        SecureZeroMemory(pass->data(), pass->size() * sizeof(wchar_t));
        if (!again) return;
        if (!match) { alert(window, L"Passcodes did not match."); return; }
        edit_project(p, [&](const Vault& writer, Project& current) {
            for (const auto key : {L"locked", L"salt", L"hash"}) current.fields[key] = p.get(key);
            writer.save(current);
            writer.record(current, L"Enabled application passcode");
        });
        reload();
    }

    void repository(Project p) {
        auto input = prompt(window, L"Link GitHub repository", L"Enter a repository URL or owner/repository. Leave blank to unlink.", p.get(L"repo"));
        if (!input) return;
        auto repo = normalize_repo(*input);
        const bool changed = edit_project(p, [&](const Vault& writer, Project& current) {
            if (repo == current.get(L"repo")) return false;
            current.fields[L"repo"] = repo;
            for (auto key : {L"issues", L"new_issues", L"stars", L"forks", L"gh_updated"}) current.fields.erase(key);
            writer.save(current);
            write_fields(current.path / issue_file, {{L"repo", repo}});
            writer.record(current, repo.empty() ? L"Unlinked GitHub repository" : L"Linked GitHub repository " + repo);
            return true;
        });
        if (!changed) { start_sync(); return; }
        pending_sync = true;
        run(L"Updating repository link…", [path = p.path, linked = !repo.empty()](const Vault& writer) {
            return refresh_context(writer, writer.load(path), linked ? L"Repository linked." : L"Repository unlinked.");
        });
    }

    void issue_view(const Project& p) {
        const auto repo = normalize_repo(p.get(L"repo"));
        const auto cache = read_fields(p.path / issue_file);
        Text body;
        if (cache.contains(L"unread_trimmed")) body = L"Unread list capped at the most recent 10,000 issues.\r\n\r\n";
        const auto unread = cache.contains(L"new") ? listed_numbers(cache.at(L"new")) : std::set<Text>{};
        for (const auto& issue : cached_issues(cache)) {
            const auto number = std::to_wstring(issue.number);
            if (unread.contains(number)) body += L"[New] ";
            body += (issue.open ? L"[Open] #" : L"[Closed] #") + number + L"  " + issue.title + L"\r\n";
            if (body.size() >= 240000) { body += L"\r\nAdditional cached issues omitted.\r\n"; break; }
        }
        if (body.empty()) body = L"No issues cached.";
        prompt(window, L"GitHub issues", L"https://github.com/" + repo + L"/issues", body, false, true);
    }

    void review(const Project& p) {
        edit_project(p, [](const Vault& writer, Project& current) {
            auto fields = read_fields(current.path / issue_file);
            fields.erase(L"new");
            fields.erase(L"unread_trimmed");
            for (auto it = fields.begin(); it != fields.end();) {
                if (it->first.starts_with(L"issue.") && it->second.starts_with(L"0")) it = fields.erase(it);
                else ++it;
            }
            write_fields(current.path / issue_file, fields);
            current.fields[L"new_issues"] = L"0";
            writer.save(current);
            writer.record(current, L"Marked GitHub issues reviewed");
        });
        reload();
    }

    void clear_search() {
        label(search, L"");
        SetFocus(search);
    }

    Command popup(Command from, std::optional<POINT> at = std::nullopt) {
        auto raw = CreatePopupMenu();
        if (!raw) return static_cast<Command>(0);
        std::deque<Text> captions;
        struct MenuHandle {
            HMENU value;
            HBRUSH background;
            ~MenuHandle() { DestroyMenu(value); DeleteObject(background); }
        } menu{raw, CreateSolidBrush(ui::surface)};
        MENUINFO menu_info{sizeof(menu_info)};
        menu_info.fMask = MIM_BACKGROUND;
        menu_info.hbrBack = menu.background;
        SetMenuInfo(menu.value, &menu_info);
        const auto add = [&](Command id, const wchar_t* name, UINT flags = 0) {
            captions.emplace_back(name);
            MENUITEMINFOW entry{sizeof(entry)};
            entry.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_STATE | MIIM_DATA;
            entry.fType = MFT_OWNERDRAW;
            entry.fState = flags;
            entry.wID = id;
            entry.dwItemData = reinterpret_cast<ULONG_PTR>(&captions.back());
            entry.dwTypeData = captions.back().data();
            InsertMenuItemW(menu.value, static_cast<UINT>(GetMenuItemCount(menu.value)), TRUE, &entry);
        };
        const auto separator = [&] { AppendMenuW(menu.value, MF_SEPARATOR, 0, nullptr); };
        const auto* row = selected();
        if (from == TrayMenu) {
            add(TrayOpen, L"Open Project Keeper");
            separator();
            add(TrayExit, L"Exit");
            SetMenuDefaultItem(menu.value, TrayOpen, FALSE);
        } else if (from == Settings) {
            add(Choose, vault ? L"Switch vault…" : L"Choose vault…",
                busy() || sync.valid() ? MF_GRAYED : 0);
            if (vault) {
                add(OpenVault, L"Open vault folder");
                separator();
                add(Refresh, L"Scan projects\tF5", busy() ? MF_GRAYED : 0);
                add(ShowHidden, L"Show hidden projects", (show_hidden ? MF_CHECKED : 0) | (busy() ? MF_GRAYED : 0));
                separator();
                add(Sync, L"Sync GitHub now", busy() || sync.valid() ? MF_GRAYED : 0);
                add(Token, L"Manage GitHub token…", busy() || sync.valid() ? MF_GRAYED : 0);
            }
            if (!tray_ready) {
                separator();
                add(TrayExit, L"Exit");
            }
        } else if (from == Sort) {
            add(SortName, L"A–Z", order == Order::name ? MF_CHECKED : 0);
            add(SortModified, page == Page::projects ? L"Last modified" : L"Newest first",
                order == Order::modified ? MF_CHECKED : 0);
            if (page == Page::projects) add(SortSize, L"Largest", order == Order::size ? MF_CHECKED : 0);
        } else if (row && !row->flag(L"keeper_read_error") && !busy()) {
            if (!access(*row)) {
                add(Protect, L"Unlock…");
            } else {
                add(Open, L"Open folder");
                add(Info, L"Details");
                if (page == Page::projects) {
                    add(Pin, pinned(*row) ? L"Unpin project" : L"Pin project");
                    add(Rename, L"Rename…");
                    add(Describe, L"Edit description…");
                    separator();
                    add(Context, L"Open project context");
                    add(OpenNotes, L"Open project notes");
                    add(UpdateContext, L"Regenerate context");
                    add(History, L"View history");
                    separator();
                    add(Backup, L"Create backup");
                    add(GitHub, has_repo(row) ? L"Change GitHub link…" : L"Link GitHub…");
                    if (has_repo(row)) {
                        add(OpenGitHub, L"Open repository");
                        add(Issues, L"View cached issues");
                        add(Review, L"Mark issues reviewed");
                    }
                    separator();
                    add(Hide, row->flag(L"hidden") ? L"Show project" : L"Hide project");
                    add(Protect, row->flag(L"locked") ? L"Lock now" : L"Set passcode…");
                    if (row->flag(L"locked")) add(RemovePasscode, L"Remove passcode…");
                    separator();
                    add(Delete, L"Move to Trash…");
                } else {
                    add(History, L"View history");
                    add(page == Page::backups ? Recover : Restore,
                        page == Page::backups ? L"Recover copy…" : L"Restore project");
                    if (row->flag(L"locked")) add(Protect, L"Lock now");
                    if (page == Page::trash) {
                        separator();
                        add(Purge, L"Permanently delete…");
                    }
                }
            }
        }
        if (!GetMenuItemCount(menu.value)) return static_cast<Command>(0);
        UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON;
        if (!at) {
            RECT anchor{};
            GetWindowRect(controls.at(from), &anchor);
            at = from == Settings ? POINT{anchor.left, anchor.top} : POINT{anchor.right, anchor.bottom};
            flags |= from == Settings ? TPM_BOTTOMALIGN | TPM_LEFTALIGN : TPM_RIGHTALIGN;
        }
        if (from == TrayMenu) SetForegroundWindow(window);
        const auto chosen = static_cast<Command>(TrackPopupMenuEx(menu.value, flags, at->x, at->y, window, nullptr));
        if (from == TrayMenu) {
            PostMessageW(window, WM_NULL, 0, 0);
            if (!chosen) {
                auto data = tray_data();
                Shell_NotifyIconW(NIM_SETFOCUS, &data);
            }
        }
        return chosen;
    }

    void show_menu(Command from, std::optional<POINT> at = std::nullopt) {
        if (in_command || closing) return;
        in_command = true;
        Command id{};
        try { id = popup(from, at); }
        catch (...) { in_command = false; throw; }
        in_command = false;
        if (id) commands(id);
    }


    void commands(Command id) {
        if (id == TrayOpen) { if (!closing) open_window(); return; }
        if (id == TrayExit) { exit_application(); return; }
        if (id == ClearSearch) { clear_search(); return; }
        if (id == SortName || id == SortModified || id == SortSize) {
            order = id == SortSize ? Order::size : id == SortModified ? Order::modified : Order::name;
            if (page == Page::projects) project_order = order;
            filter();
            return;
        }
        if ((busy() && id != Mcp) || closing || in_command) return;
        struct Guard {
            bool& active;
            ~Guard() { active = false; }
        } guard{in_command};
        in_command = true;
        if (id == Mcp) {
            Text exe(32768, L'\0');
            const auto size = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            if (!size || size >= exe.size()) throw std::runtime_error("Could not locate the Project Keeper executable.");
            exe.resize(size);
            const auto helper = fs::path(exe).parent_path() / L"ProjectKeeperMcp.exe";
            Text body = L"In Codex, use Project Keeper to create or import projects, edit notes, and update descriptions or pins. "
                        L"You can also read project context, history, and cached GitHub issues.\r\n\r\n"
                        L"If Project Keeper is missing, open MCP servers in Codex settings and add a STDIO server named project-keeper. "
                        L"Use this program as the command:\r\n\r\n" + helper.native() +
                        L"\r\n\r\nLeave arguments blank. Restart Codex and ask: What projects are pinned in my Project Keeper vault?";
            prompt(window, L"MCP connection", L"Connect Project Keeper to your chat client.", body, false, true);
            return;
        }
        if (id == Choose || id == Welcome) {
            if (sync.valid()) return;
            if (auto path = browse(window, L"Choose an empty folder or an existing Project Keeper vault")) {
                set_vault(*path, !smoke);
                SetFocus(search);
                pending_sync = !smoke;
                scan();
            }
            return;
        }
        if (!vault) return;
        switch (id) {
        case Projects: set_page(Page::projects); return;
        case Trash: set_page(Page::trash); return;
        case Backups: set_page(Page::backups); return;
        case Refresh: reload(); scan(); return;
        case ShowHidden: show_hidden = !show_hidden; reload(); return;
        case OpenArea: open_target(window, area().native()); return;
        case OpenVault: open_target(window, vault->root().native()); return;
        case Sync: start_sync(true); return;
        case Token: {
            auto token = prompt(window, L"GitHub token", L"Saved in Windows Credential Manager. Leave blank to remove the saved token.", {}, true);
            if (token) {
                try { set_github_token(*token); }
                catch (...) { SecureZeroMemory(token->data(), token->size() * sizeof(wchar_t)); throw; }
                SecureZeroMemory(token->data(), token->size() * sizeof(wchar_t));
                start_sync(true);
            }
            return;
        }
        case New: case Import: {
            fs::path source;
            if (id == Import) {
                auto dir = browse(window, L"Choose a project folder to copy into this vault");
                if (!dir) return;
                source = *dir;
            }
            auto draft = project_prompt(window, source);
            if (!draft) return;
            if (page != Page::projects || !query.empty()) set_page(Page::projects);
            const auto progress = id == Import ? std::make_shared<ImportProgress>() : nullptr;
            run(id == Import ? L"Preparing import…" : L"Creating project…",
                [draft = std::move(*draft), source, progress](const Vault& writer) {
                const auto created = writer.create(draft.name, draft.description, source, progress.get());
                return refresh_context(writer, created, source.empty() ? L"Project created." : L"Project imported.", true);
            });
            import_progress = progress;
            return;
        }
        default: break;
        }
        if (!selected() || selected()->flag(L"keeper_read_error")) return;
        auto project = vault->load(selected()->path);
        if (id == Protect) { change_passcode(project); return; }
        if (!access(project)) return;
        switch (id) {
        case Open: open_target(window, project.path.native()); break;
        case Info: project_details(project); break;
        case Pin:
            if (page == Page::projects) {
                const bool was_pinned = edit_project(project, [](const Vault& writer, Project& current) {
                    const bool pinned = current.flag(L"pinned");
                    if (pinned) current.fields.erase(L"pinned");
                    else current.fields[L"pinned"] = L"1";
                    writer.save(current);
                    return pinned;
                });
                reload();
                log(was_pinned ? L"Project unpinned." : L"Project pinned.");
            }
            break;
        case Rename: {
            auto name = prompt(window, L"Rename project", L"New project folder name", project.name());
            if (name) {
                auto previous = project.path;
                const auto renamed = edit_project(project, [&](const Vault& writer, const Project& current) {
                    return writer.rename(current, *name);
                });
                if (unlocked.erase(previous)) unlocked.insert(renamed.path);
                measured.erase(previous);
                selected_name = renamed.name();
                reload();
                run(L"Updating renamed project…", [renamed](const Vault& writer) {
                    return refresh_context(writer, writer.load(renamed.path), L"Project renamed.");
                });
            }
            break;
        }
        case Describe: {
            auto value = prompt(window, L"Project description", L"Description for this project and its handoff summary.", project.get(L"description"));
            if (value) {
                edit_project(project, [&](const Vault& writer, Project& current) {
                    current.fields[L"description"] = *value;
                    writer.save(current);
                    writer.record(current, L"Updated description");
                });
                run(L"Updating description…", [project](const Vault& writer) {
                    return refresh_context(writer, writer.load(project.path), L"Description updated.");
                });
            }
            break;
        }
        case Delete:
            if (confirm(window, L"Move this project to the vault’s Trash? You can restore it from Trash.")) {
                edit_project(project, [](const Vault& writer, const Project& current) { writer.erase(current); });
                unlocked.erase(project.path);
                measured.erase(project.path);
                reload();
                log(L"Project moved to Trash.");
            }
            break;
        case Restore:
            edit_project(project, [](const Vault& writer, const Project& current) { writer.restore_trash(current); });
            unlocked.erase(project.path);
            reload();
            scan();
            break;
        case Purge:
            if (confirm(window, L"Permanently delete the selected Trash folder?") &&
                confirm(window, L"Final confirmation. This deletion cannot be reversed."))
                run(L"Deleting Trash folder…", [project](const Vault& writer) {
                    writer.purge(writer.load(project.path));
                    return Text(L"Trash folder deleted.");
                });
            break;
        case Hide:
            edit_project(project, [](const Vault& writer, Project& current) {
                current.fields[L"hidden"] = current.flag(L"hidden") ? L"0" : L"1";
                writer.save(current);
                writer.record(current, current.flag(L"hidden") ? L"Hid project" : L"Showed project");
            });
            reload();
            break;
        case Context:
            run(L"Preparing project context…", [project](const Vault& writer) {
                auto result = refresh_context(writer, writer.load(project.path), {});
                result.message = L"Project context updated.";
                result.open_after = project.path / L"PROJECT_CONTEXT.md";
                return result;
            });
            break;
        case OpenNotes:
            open_target(window, (project.path / project_notes).native());
            break;
        case UpdateContext:
            run(L"Updating project context…", [project](const Vault& writer) {
                auto result = refresh_context(writer, writer.load(project.path), {});
                result.message = L"Project context updated.";
                return result;
            });
            break;
        case OpenGitHub:
            if (!project.get(L"repo").empty()) open_target(window, L"https://github.com/" + normalize_repo(project.get(L"repo")));
            break;
        case RemovePasscode:
            if (project.flag(L"locked") && confirm(window, L"Remove this project's application passcode?")) {
                edit_project(project, [](const Vault& writer, Project& current) {
                    Vault::lock_project(current, {});
                    writer.save(current);
                    writer.record(current, L"Removed application passcode");
                });
                unlocked.erase(project.path);
                reload();
            }
            break;
        case GitHub: repository(project); break;
        case Issues: issue_view(project); break;
        case Review: review(project); break;
        case Backup:
            run(L"Copying a backup…", [project](const Vault& writer) {
                writer.backup(writer.load(project.path));
                return Text(L"Backup completed.");
            });
            break;
        case Recover: {
            auto suggestion = project.get(L"keeper_original");
            if (suggestion.empty()) {
                suggestion = project.name();
                if (auto split = suggestion.rfind(L"__backup_"); split != Text::npos) suggestion.resize(split);
            }
            auto name = prompt(window, L"Recover backup", L"Enter a name for the recovered project. Existing projects are preserved.",
                               suggestion.substr(0, 240) + L" - recovered");
            if (name) {
                set_page(Page::projects);
                if (project.flag(L"hidden")) show_hidden = true;
                run(L"Recovering backup…", [project, name = *name](const Vault& writer) {
                    const auto recovered = writer.recover(writer.load(project.path), name);
                    return refresh_context(writer, recovered, L"Backup recovered as a separate project.", true);
                });
            }
            break;
        }
        case History: {
            auto value = vault->history(project);
            prompt(window, L"Project history", project.name(), value.empty() ? L"No recorded events." : value, false, true);
            break;
        }
        default: break;
        }
    }

    size_t tile_index(HWND handle) const {
        if (!handle || GetParent(handle) != grid) return SIZE_MAX;
        const int slot = GetDlgCtrlID(handle) - tile_id;
        return slot >= 0 && static_cast<size_t>(slot) < tile_buttons.size() ?
            tile_buttons[static_cast<size_t>(slot)].index : SIZE_MAX;
    }

    size_t move_tile(size_t index, bool down) const {
        const bool in_pins = index < pinned_count;
        const size_t start = in_pins ? 0 : pinned_count;
        const size_t end = in_pins ? pinned_count : shown.size();
        const size_t column = (index - start) % 3;
        const size_t row_start = index - column;
        if (down) {
            if (row_start + 3 < end) return std::min(index + 3, end - 1);
            return in_pins && end < shown.size() ? std::min(end + column, shown.size() - 1) : index;
        }
        if (row_start > start) return index - 3;
        if (!in_pins && pinned_count) return std::min((pinned_count - 1) / 3 * 3 + column, pinned_count - 1);
        return index;
    }

public:
    explicit App(HINSTANCE module, bool test = false, LaunchOptions options = {})
        : instance(module), smoke(test), start_in_tray(options.tray), launch_vault(std::move(options.vault)) {}

    void present(int show) const {
        if (start_in_tray) return;
        ShowWindow(window, show);
        UpdateWindow(window);
    }

    bool shortcut(const MSG& msg) {
        if (msg.message != WM_KEYDOWN || in_command || closing) return false;
        if (GetKeyState(VK_CONTROL) < 0 && vault) {
            if (msg.wParam == 'F') {
                SetFocus(search);
                SendMessageW(search, EM_SETSEL, 0, -1);
                return true;
            }
            if (msg.wParam == 'N' || msg.wParam == 'I' || msg.wParam == 'P') {
                commands(msg.wParam == 'P' ? Pin : msg.wParam == 'N' ? New : Import);
                return true;
            }
        }
        if (msg.wParam == VK_F5 && vault) { commands(Refresh); return true; }
        if (msg.wParam == VK_ESCAPE && msg.hwnd == search && !query.empty()) {
            clear_search();
            return true;
        }
        if (msg.wParam == VK_RETURN && msg.hwnd == search && selected()) {
            select_tile(selection, true);
            return true;
        }
        const size_t index = tile_index(msg.hwnd);
        if (index >= shown.size()) return false;
        switch (msg.wParam) {
        case VK_RETURN: select_tile(index); activate(); break;
        case VK_LEFT: select_tile(index ? index - 1 : index, true); break;
        case VK_RIGHT: select_tile(std::min(index + 1, shown.size() - 1), true); break;
        case VK_UP: select_tile(move_tile(index, false), true); break;
        case VK_DOWN: select_tile(move_tile(index, true), true); break;
        case VK_HOME: select_tile(0, true); break;
        case VK_END: select_tile(shown.size() - 1, true); break;
        case VK_PRIOR: case VK_NEXT: {
            size_t next = index;
            for (int i = 0; i < std::max(1, grid_height / (tile_height() + tile_gap)); ++i)
                next = move_tile(next, msg.wParam == VK_NEXT);
            select_tile(next, true);
            break;
        }
        default: return false;
        }
        return true;
    }

    static LRESULT CALLBACK grid_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_NCCREATE) {
            const auto app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!app) return DefWindowProcW(hwnd, msg, w, l);
        try { return app->grid_message(hwnd, msg, w, l); }
        catch (const std::exception& error) {
            app->log(error_text(error), true);
            if (!app->smoke && !app->closing) alert(app->window, error_text(error));
            return 0;
        }
    }

    LRESULT grid_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_ERASEBKGND: return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(hwnd, &ps);
            draw_grid(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM:
            draw_tile(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
            return TRUE;
        case WM_COMMAND: {
            const auto index = tile_index(reinterpret_cast<HWND>(l));
            if (index >= shown.size()) return 0;
            if (HIWORD(w) == BN_CLICKED || HIWORD(w) == BN_DOUBLECLICKED) {
                select_tile(index);
                if (HIWORD(w) == BN_DOUBLECLICKED) activate();
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            const auto button = reinterpret_cast<HWND>(w);
            const auto index = tile_index(button);
            if (index >= shown.size()) return 0;
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (p.x == -1 && p.y == -1) {
                RECT rect{};
                GetWindowRect(button, &rect);
                p = {rect.left + px(16), std::min(rect.bottom, rect.top + px(110))};
            }
            select_tile(index);
            show_menu(More, p);
            return 0;
        }
        case WM_MOUSEWHEEL:
            wheel_delta += GET_WHEEL_DELTA_WPARAM(w);
            scroll(scroll_offset - wheel_delta / WHEEL_DELTA * 105);
            wheel_delta %= WHEEL_DELTA;
            return 0;
        case WM_LBUTTONDOWN: {
            if (logical(GET_X_LPARAM(l)) < library_width - 14 || !max_scroll()) break;
            const auto thumb = scroll_thumb();
            const int y = GET_Y_LPARAM(l);
            if (y >= thumb.top && y < thumb.bottom) {
                dragging_scroll = true;
                drag_origin = y;
                drag_offset = scroll_offset;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else scroll(scroll_offset + (y < thumb.top ? -grid_height : grid_height));
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_scroll) {
                const auto thumb = scroll_thumb();
                const int travel = px(grid_height - 8) - (thumb.bottom - thumb.top);
                if (travel > 0) scroll(drag_offset + MulDiv(GET_Y_LPARAM(l) - drag_origin, max_scroll(), travel));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (dragging_scroll) ReleaseCapture();
            break;
        case WM_CAPTURECHANGED:
            dragging_scroll = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }


    void activate() {
        const auto* row = selected();
        if (!row || row->flag(L"keeper_read_error")) return;
        if (!access(*row)) commands(Protect);
        else commands(Open);
    }

    static LRESULT CALLBACK track_button(HWND hwnd, UINT message, WPARAM w, LPARAM l,
                                         UINT_PTR, DWORD_PTR data) {
        auto& app = *reinterpret_cast<App*>(data);
        if (message == WM_MOUSEMOVE && app.hovered != hwnd) {
            if (app.hovered) InvalidateRect(app.hovered, nullptr, FALSE);
            app.hovered = hwnd;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (message == WM_MOUSELEAVE && app.hovered == hwnd) {
            app.hovered = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return DefSubclassProc(hwnd, message, w, l);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            const auto app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            app->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!app) return DefWindowProcW(hwnd, message, w, l);
        try { return app->message(message, w, l); }
        catch (const std::exception& error) {
            if (message == WM_CREATE) return -1;
            auto text = error_text(error);
            app->log(text, true);
            if (!app->smoke && !app->closing && IsWindowVisible(hwnd)) alert(hwnd, text);
            return 0;
        }
    }

    LRESULT message(UINT msg, WPARAM w, LPARAM l) {
        if (taskbar_created && msg == taskbar_created) {
            remove_tray();
            refresh_tray();
            return 0;
        }
        switch (msg) {
        case WM_CREATE: {
            dpi = std::max(96u, GetDpiForWindow(window));
            fonts.reset(dpi);
            const auto add = [&](const Button& spec) {
                const auto hwnd = child(L"BUTTON", spec.caption, WS_TABSTOP | BS_OWNERDRAW, spec.id);
                controls.emplace(spec.id, hwnd);
                SetWindowSubclass(hwnd, track_button, 1, reinterpret_cast<DWORD_PTR>(this));
            };
            for (const auto& spec : buttons) if (spec.id < Open || spec.id == Settings || spec.id == Mcp ||
                spec.id == OpenArea || spec.id == Welcome) add(spec);
            search = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Search);
            SendMessageW(search, EM_SETLIMITTEXT, 512, 0);
            SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            for (const auto& spec : buttons) if (spec.id == Sort || spec.id == ClearSearch) add(spec);
            grid = CreateWindowExW(WS_EX_CONTROLPARENT, grid_class, L"Project tiles",
                WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, window, nullptr, instance, this);
            if (!grid) return -1;
            for (const auto& spec : buttons) if (!controls.contains(spec.id)) add(spec);
            ui::dark_caption(window);
            reload();
            if (!smoke) {
                taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
                // Permit Explorer's icon callbacks to reach the elevated window.
                ChangeWindowMessageFilterEx(window, tray_message, MSGFLT_ALLOW, nullptr);
                if (taskbar_created) ChangeWindowMessageFilterEx(window, taskbar_created, MSGFLT_ALLOW, nullptr);
                refresh_tray();
                SetTimer(window, 1, 200, nullptr);
                SetTimer(window, 2, gh_interval, nullptr);
                PostMessageW(window, setup_message, 0, 0);
            }
            return 0;
        }
        case setup_message: {
            const auto path = launch_vault.empty() ? last_vault() : launch_vault;
            if (!path.empty()) {
                try {
                    set_vault(path, false);
                    if (IsWindowVisible(window)) SetFocus(search);
                    pending_sync = true;
                    scan();
                    return 0;
                }
                catch (const std::exception& error) { log(error_text(error), true); }
            }
            if (IsWindowVisible(window)) SetFocus(controls.at(Welcome));
            return 0;
        }
        case WM_SIZE:
            if (grid && w != SIZE_MINIMIZED) layout();
            return 0;
        case WM_DPICHANGED: {
            dpi = HIWORD(w);
            fonts.reset(dpi);
            for (const auto& [id, hwnd] : controls)
                SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.body), TRUE);
            SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.body), TRUE);
            const auto* rect = reinterpret_cast<RECT*>(l);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOACTIVATE | SWP_NOZORDER);
            layout();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            const UINT scale = std::max(96u, GetDpiForWindow(window));
            RECT rect{0, 0, MulDiv(min_width, static_cast<int>(scale), 96), MulDiv(min_height, static_cast<int>(scale), 96)};
            AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, scale);
            reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(window, &ps);
            paint(dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_MEASUREITEM: {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if (item.CtlType != ODT_MENU || !item.itemData) break;
            const auto& caption = *reinterpret_cast<const Text*>(item.itemData);
            const auto dc = GetDC(window);
            const auto old = SelectObject(dc, fonts.body);
            SIZE text{};
            const auto split = caption.find(L'\t');
            GetTextExtentPoint32W(dc, caption.data(), static_cast<int>(split == Text::npos ? caption.size() : split), &text);
            SelectObject(dc, old);
            ReleaseDC(window, dc);
            item.itemWidth = text.cx + px(split == Text::npos ? 40 : 82);
            item.itemHeight = px(30);
            return TRUE;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (item.CtlType == ODT_MENU && item.itemData) {
                const auto& caption = *reinterpret_cast<const Text*>(item.itemData);
                const auto r = item.rcItem;
                const auto color = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) ? ui::muted : ui::text;
                ui::fill(item.hDC, r, (item.itemState & ODS_SELECTED) ? ui::mint : ui::surface);
                if (item.itemState & ODS_CHECKED)
                    ui::icon(item.hDC, Icon::check, ui::box(r.left + px(8), r.top + px(7), px(16), px(16)), ui::accent);
                const auto split = caption.find(L'\t');
                ui::label(item.hDC, {r.left + px(30), r.top, r.right - px(12), r.bottom},
                          std::wstring_view(caption).substr(0, split), fonts.body, color);
                if (split != Text::npos)
                    ui::label(item.hDC, {r.left + px(30), r.top, r.right - px(12), r.bottom},
                              std::wstring_view(caption).substr(split + 1), fonts.caption, ui::muted,
                              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
                return TRUE;
            }
            for (const auto& spec : buttons) if (static_cast<UINT>(spec.id) == item.CtlID) {
                const bool active = (spec.id == Projects && page == Page::projects) ||
                    (spec.id == Backups && page == Page::backups) || (spec.id == Trash && page == Page::trash);
                const COLORREF backdrop = spec.role == Role::navigation || spec.role == Role::rail_button ? ui::rail :
                    spec.id == ClearSearch ? ui::soft : spec.id == New || spec.id == Import ||
                    spec.id == OpenArea || spec.id == Sort ? ui::background : ui::surface;
                ui::fill(item.hDC, item.rcItem, backdrop);
                const auto caption = spec.icon_only ? Text{} : ui::window_text(item.hwndItem);
                ui::button(item, fonts, dpi, caption, spec.icon, spec.role,
                           active && !!vault, hovered == item.hwndItem);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            const auto control = reinterpret_cast<HWND>(l);
            const auto dc = reinterpret_cast<HDC>(w);
            const auto bg = control == search ? ui::soft : ui::surface;
            SetTextColor(dc, ui::text);
            SetBkColor(dc, bg);
            SetDCBrushColor(dc, bg);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT rect{};
            if (grid && vault && GetWindowRect(grid, &rect) && PtInRect(&rect, point))
                return SendMessageW(grid, msg, w, l);
            break;
        }
        case WM_COMMAND: {
            const auto id = static_cast<Command>(LOWORD(w));
            if (id == Search) {
                if (HIWORD(w) == EN_CHANGE && !replacing_search) { query = ui::window_text(search); filter(); }
                else if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) invalidate();
            } else if (id == Settings || id == Sort || id == More) show_menu(id);
            else if (HIWORD(w) == BN_CLICKED) commands(id);
            return 0;
        }
        case WM_TIMER:
            if (w == 1) poll();
            if (w == 2 && vault && !closing) { if (!busy() && !in_command) scan(); pending_sync = true; }
            if (w == tray_retry_timer) refresh_tray();
            return 0;
        case tray_message: {
            if (!tray_ready || closing) return 0;
            const UINT event = tray_modern ? LOWORD(l) : static_cast<UINT>(l);
            const UINT source = tray_modern ? HIWORD(l) : static_cast<UINT>(w);
            if (source != tray_icon_id) return 0;
            if (event == NIN_SELECT || event == NIN_KEYSELECT || (!tray_modern && event == WM_LBUTTONUP)) {
                commands(TrayOpen);
            } else if (event == WM_CONTEXTMENU || (!tray_modern && event == WM_RBUTTONUP)) {
                POINT at{GET_X_LPARAM(w), GET_Y_LPARAM(w)};
                if (!tray_modern || (at.x == -1 && at.y == -1)) GetCursorPos(&at);
                show_menu(TrayMenu, at);
            }
            return 0;
        }
        case restore_message:
            commands(TrayOpen);
            return 0;
        case WM_CLOSE:
            close_window();
            return 0;
        case WM_ENDSESSION:
            if (w) remove_tray();
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_DESTROY:
            remove_tray();
            KillTimer(window, 1);
            KillTimer(window, 2);
            KillTimer(window, tray_retry_timer);
            if (!smoke) PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, msg, w, l);
    }

    HWND create() {
        WNDCLASSW cls{};
        cls.hInstance = instance;
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = window_class;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        cls.lpfnWndProc = grid_proc;
        cls.lpszClassName = grid_class;
        cls.hIcon = nullptr;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        const auto scale = GetDpiForSystem();
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int w = std::clamp(static_cast<int>(work.right - work.left) - 36,
                                 MulDiv(min_width + 16, scale, 96), MulDiv(1280, scale, 96));
        const int h = std::clamp(static_cast<int>(work.bottom - work.top) - 28,
                                 MulDiv(min_height + 39, scale, 96), MulDiv(820, scale, 96));
        return CreateWindowExW(0, window_class, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, instance, this);
    }

    int window_smoke(const fs::path& parent) {
        const auto temp = parent / (L"keeper-window-" + unique_id());
        fs::create_directory(temp);
        Text result = L"PASS";
        bool tray_registered = false;
        try {
            if (!create() || ui::window_text(window) != title || !IsWindow(grid) || !IsWindow(search))
                throw std::runtime_error("Native window or controls failed to initialize.");
            const wchar_t* args[]{L"ProjectKeeper.exe", L"--tray", L"--vault", temp.c_str()};
            auto options = launch_options(static_cast<int>(std::size(args)), args);
            if (!options || !options->tray || options->vault != temp)
                throw std::runtime_error("The tray launch arguments were rejected.");
            start_in_tray = options->tray;
            present(SW_SHOWNORMAL);
            if (IsWindowVisible(window))
                throw std::runtime_error("A tray launch showed a desktop window.");
            IFileOpenDialog* dialog = nullptr;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
                throw std::runtime_error("Windows folder chooser could not be instantiated.");
            dialog->Release();
            if (!(GetWindowLongPtrW(controls.at(Welcome), GWL_STYLE) & WS_VISIBLE))
                throw std::runtime_error("First-run state failed.");
            check_project_prompt(window);
            set_vault(temp, false);
            const auto fixtures = [&] {
                const Vault writer(vault->root(), Vault::Open::existing);
                const auto created = writer.create(L"Smoke project", L"Window fixture");
                auto linked = writer.create(L"Atlas", L"Second fixture");
                linked.fields.insert({{L"repo", L"example/atlas"}, {L"issues", L"4"}, {L"stars", L"12"}, {L"forks", L"2"}});
                writer.save(linked);
                const auto b = writer.create(L"Beacon", L"Third fixture");
                const auto z = writer.create(L"Zenith", L"Fourth fixture");
                auto hidden = writer.create(L"Hidden fixture", L"Hidden description");
                hidden.fields[L"hidden"] = L"1";
                writer.save(hidden);
                return std::array{created, linked, b, z};
            }();
            const auto& [smoke_project, a, b, z] = fixtures;
            if (!has_repo(&a) || tile_github_summary(a, false) != L"4 open  ·  12 stars  ·  2 forks")
                throw std::runtime_error("The tile did not summarize the linked project.");
            SendMessageW(window, WM_COMMAND, Projects, 0);
            if (shown.size() != 4)
                throw std::runtime_error("The project grid included hidden projects.");
            SendMessageW(window, WM_COMMAND, ShowHidden, 0);
            if (shown.size() != 5)
                throw std::runtime_error("The hidden-project toggle did not update the grid.");
            SendMessageW(window, WM_COMMAND, ShowHidden, 0);
            select_tile(2);
            SendMessageW(window, WM_COMMAND, Pin, 0);
            select_tile(2);
            SendMessageW(window, WM_COMMAND, Pin, 0);
            const auto names_are = [&](std::array<std::wstring_view, 4> names) {
                if (shown.size() != 4 || pinned_count != 2) return false;
                for (size_t i = 0; i < names.size(); ++i) if (rows[shown[i]].name() != names[i]) return false;
                return true;
            };
            if (!names_are({L"Beacon", L"Smoke project", L"Atlas", L"Zenith"}) ||
                !vault->load(smoke_project.path).flag(L"pinned") || !vault->load(b.path).flag(L"pinned"))
                throw std::runtime_error("Pins did not persist or sort separately by name.");
            measured[a.path].bytes = 10;
            measured[b.path].bytes = 2;
            measured[smoke_project.path].bytes = 5;
            measured[z.path].bytes = 20;
            SendMessageW(window, WM_COMMAND, SortSize, 0);
            if (!names_are({L"Smoke project", L"Beacon", L"Zenith", L"Atlas"}))
                throw std::runtime_error("Largest did not sort separately inside both groups.");
            const auto time = fs::file_time_type::clock::now();
            measured[a.path].modified = time;
            measured[b.path].modified = time;
            measured[smoke_project.path].modified = time - std::chrono::hours(1);
            measured[z.path].modified = time - std::chrono::hours(2);
            SendMessageW(window, WM_COMMAND, SortModified, 0);
            if (!names_are({L"Beacon", L"Smoke project", L"Atlas", L"Zenith"}))
                throw std::runtime_error("Last modified did not sort separately inside both groups.");
            if (tile_rect(2).left != 0 || tile_rect(2).top <= tile_rect(0).bottom)
                throw std::runtime_error("Ordinary projects did not start a new section.");
            label(search, L"SMOKE");
            if (shown.size() != 1 || !selected() || selected()->name() != L"Smoke project" || pinned_count != 1)
                throw std::runtime_error("Case-insensitive filtering or selection did not populate.");
            label(search, L"no matching project");
            if (!shown.empty())
                throw std::runtime_error("The empty search result did not display.");
            label(search, L"window fixture");
            if (shown.size() != 1)
                throw std::runtime_error("Description search did not match.");
            RECT bounds{0, 0, px(min_width), px(min_height)};
            AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (tile_rect(2).right > library_width - 14 || tile_width() < 175 || grid_height < tile_height())
                throw std::runtime_error("The compact workspace layout does not fit.");
            SendMessageW(window, WM_COMMAND, Backup, 0);
            job.wait();
            poll();
            SendMessageW(window, WM_COMMAND, Backups, 0);
            if (shown.size() != 1 || !selected() ||
                display_name(*selected(), Page::backups) != L"Smoke project" || selected()->get(L"keeper_snapshot").empty())
                throw std::runtime_error("Backup action or native Backups view failed.");
            SendMessageW(window, WM_COMMAND, Trash, 0);
            if (!shown.empty())
                throw std::runtime_error("The empty Trash view did not display.");
            set_page(Page::projects);
            {
                const Vault outside(vault->root(), Vault::Open::existing);
                outside.create(L"MCP fixture", L"Added while the window was open.");
                write_text(outside.root() / L".keeper" / mcp_change_file, unique_id());
            }
            next_mcp_check = 0;
            refresh_mcp_changes();
            if (shown.size() != 5)
                throw std::runtime_error("The window did not reload after an external vault write.");
            tray_registered = add_tray();
            const auto handle = window;
            if (tray_registered) {
                // Exercise the real visibility transitions without activating a test window on the desktop.
                const auto styles = GetWindowLongPtrW(handle, GWL_EXSTYLE);
                SetWindowLongPtrW(handle, GWL_EXSTYLE, styles | WS_EX_TOOLWINDOW);
                SetWindowPos(handle, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 16,
                             GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(handle, SW_SHOWNOACTIVATE);
                if (!IsWindowVisible(handle)) throw std::runtime_error("The tray test window did not show.");
            }
            std::promise<void> finished;
            run(L"Tray test operation", [gate = finished.get_future()](const Vault&) {
                gate.wait();
                return Text(L"Tray test completed.");
            });
            SendMessageW(handle, WM_CLOSE, 0, 0);
            if (!IsWindow(handle) || IsWindowVisible(handle) || closing || !busy())
                throw std::runtime_error("Closing the window ended its background operation.");
            if (tray_registered) {
                const WPARAM icon = tray_modern ? 0 : tray_icon_id;
                const LPARAM event = tray_modern ? MAKELPARAM(NIN_SELECT, tray_icon_id) : WM_LBUTTONUP;
                SendMessageW(handle, tray_message, icon, event);
                if (!IsWindowVisible(handle) || !busy())
                    throw std::runtime_error("The tray icon failed to reopen the active window.");
                SendMessageW(handle, WM_CLOSE, 0, 0);
            }
            SendMessageW(handle, WM_COMMAND, TrayExit, 0);
            if (!IsWindow(handle) || !closing)
                throw std::runtime_error("Tray Exit did not wait for the active operation.");
            finished.set_value();
            job.wait();
            poll();
            if (IsWindow(handle) || tray_ready)
                throw std::runtime_error("Tray Exit did not close the window and remove the icon.");
        } catch (const std::exception& error) { result = L"FAIL: " + error_text(error); }
        if (IsWindow(window)) DestroyWindow(window);
        vault.reset();
        std::error_code ignored;
        fs::remove_all(temp, ignored);
        write_text(parent / L"window-smoke-result.txt", result + L"\r\n" +
            (tray_registered ? L"Explorer registered the tray icon.\r\n" : L"Explorer tray registration unavailable.\r\n"));
        return result == L"PASS" ? 0 : 1;
    }

};
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, [[maybe_unused]] int show) {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct Com { HRESULT result; ~Com() { if (SUCCEEDED(result)) CoUninitialize(); } } guard{com};
    int argc{};
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> free_args(args, LocalFree);
    if (!args) return 1;
    try {
#ifdef PK_TEST_DRIVER
        if (argc == 3 && std::wstring_view(args[1]) == L"--self-test") return pk::self_test(args[2]);
#else
        if (!pk::elevated()) {
            MessageBoxW(nullptr, L"Project Keeper requires administrator privileges.", pk::title, MB_OK | MB_ICONWARNING);
            return ERROR_ELEVATION_REQUIRED;
        }
        auto options = pk::launch_options(argc, args);
        if (!options) return ERROR_INVALID_PARAMETER;
        if (const auto previous = FindWindowW(pk::window_class, nullptr)) {
            if (options->tray) return 0;
            DWORD process{};
            GetWindowThreadProcessId(previous, &process);
            if (process) AllowSetForegroundWindow(process);
            if (PostMessageW(previous, pk::restore_message, 0, 0)) return 0;
        }
#endif
        Gdiplus::GdiplusStartupInput graphics;
        ULONG_PTR graphics_token{};
        if (Gdiplus::GdiplusStartup(&graphics_token, &graphics, nullptr) != Gdiplus::Ok) return 1;
        struct GraphicsLifetime { ULONG_PTR token; ~GraphicsLifetime() { Gdiplus::GdiplusShutdown(token); } } painting{graphics_token};
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&common);
#ifdef PK_TEST_DRIVER
        if (argc == 3 && std::wstring_view(args[1]) == L"--window-smoke") return pk::App(instance, true).window_smoke(args[2]);
        if ((argc == 3 || argc == 4) && std::wstring_view(args[1]) == L"--github-smoke") {
            std::map<pk::Text, pk::Text> errors;
            const pk::Text repo = argc == 4 ? pk::normalize_repo(args[3]) : L"microsoft/winget-cli";
            auto result = pk::github_sync({repo}, {}, errors);
            const auto count = result.contains(repo) ? result.at(repo).issues.size() : 0;
            pk::write_text(args[2], result.contains(repo)
                ? L"PASS: " + repo + L" returned " + std::to_wstring(count) + L" open and recently closed issues.\r\n"
                : L"FAIL: " + errors[repo] + L"\r\n");
            return result.contains(repo) ? 0 : 1;
        }
        return 1;
#else
        pk::App app(instance, false, std::move(*options));
        const auto window = app.create();
        if (!window) return 1;
        app.present(show);
        MSG msg{};
        int status{};
        while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            if (!app.shortcut(msg) && !IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        return status < 0 ? 1 : static_cast<int>(msg.wParam);
#endif
    } catch (const std::exception&) { return 1; }
}

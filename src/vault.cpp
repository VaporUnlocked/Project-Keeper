#include "keeper.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <limits>
#include <objbase.h>
#include <sstream>

namespace pk {
namespace {
constexpr auto meta = L".projectkeeper";
constexpr auto context_name = L"PROJECT_CONTEXT.md";
constexpr auto events = L".projectkeeper_history";
constexpr auto staging = L".keeper-staging-";

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }

DWORD attrs(const fs::path& path) { return GetFileAttributesW(path.c_str()); }

bool exists_entry(const fs::path& path) {
    const auto a = attrs(path);
    if (a != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw std::system_error(static_cast<int>(error), std::system_category(), "Reading file attributes");
}

fs::path canonical_directory(const fs::path& path) {
    if (!plain_directory(path)) fail("Choose a normal directory. Links and junctions cannot be used here.");
    return fs::canonical(path);
}

bool equal(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool within(const fs::path& child, const fs::path& parent) {
    auto p = child.begin();
    for (const auto& part : parent) {
        if (p == child.end() || !equal(p->native(), part.native())) return false;
        ++p;
    }
    return true;
}

void verify_entry(const Project& p, const fs::path& area) {
    if (!equal(p.path.parent_path().native(), area.native()) || !plain_directory(area) ||
        !plain_directory(p.path) || !valid_name(p.name())) fail("This folder is no longer available.");
}

void require_destination(const fs::path& dest) {
    if (exists_entry(dest)) fail("A folder or file with that name already exists.");
}

DWORD copy_attributes(const fs::path& path) {
    const auto a = attrs(path);
    if (a == INVALID_FILE_ATTRIBUTES) fail("A source entry became unavailable during the copy.");
    if (a & FILE_ATTRIBUTE_REPARSE_POINT)
        fail("The source contains a symbolic link or junction. Copy stopped to preserve the original.");
    return a;
}

struct FileCopyProgress {
    ImportProgress& progress;
    uintmax_t prior_bytes, file_size;
};

DWORD CALLBACK report_file_copy(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER stream_bytes,
                               DWORD stream, DWORD, HANDLE, HANDLE, LPVOID context) {
    const auto& current = *static_cast<const FileCopyProgress*>(context);
    if (stream == 1 && stream_bytes.QuadPart >= 0) {
        const auto copied = std::min(current.file_size, static_cast<uintmax_t>(stream_bytes.QuadPart));
        current.progress.bytes_copied = current.prior_bytes + copied;
    }
    return PROGRESS_CONTINUE;
}

void copy_tree(const fs::path& source, const fs::path& target, ImportProgress* progress) {
    uintmax_t total_bytes = 0, total_files = 0;
    if (progress) {
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            if (copy_attributes(entry.path()) & FILE_ATTRIBUTE_DIRECTORY) continue;
            total_bytes += entry.file_size();
            ++total_files;
        }
        progress->bytes_total = total_bytes;
        progress->files_total = total_files;
        progress->stage = ImportStage::copying;
    }
    uintmax_t copied_bytes = 0, copied_files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        const DWORD a = copy_attributes(entry.path());
        const auto dest = target / entry.path().lexically_relative(source);
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            fs::create_directory(dest);
        } else {
            const auto size = fs::file_size(entry.path());
            const auto time = fs::last_write_time(entry.path());
            if (progress) {
                FileCopyProgress current{*progress, copied_bytes, size};
                if (!CopyFileExW(entry.path().c_str(), dest.c_str(), report_file_copy, &current,
                                 nullptr, COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK)) {
                    const auto error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
                    throw fs::filesystem_error("Copying import file", entry.path(), dest, error);
                }
                copy_attributes(dest);
                copied_bytes += size;
                ++copied_files;
                progress->bytes_copied = copied_bytes;
                progress->files_copied = copied_files;
            } else {
                fs::copy_file(entry.path(), dest);
            }
            if (fs::file_size(entry.path()) != size || fs::last_write_time(entry.path()) != time)
                fail("A file changed during the copy. Close programs writing to the source and retry.");
        }
    }
    if (progress) {
        if (copied_bytes != total_bytes || copied_files != total_files)
            fail("The source folder changed during import. Retry when the folder is ready.");
        progress->stage = ImportStage::finishing;
    }
}

void staged_project(const fs::path& dest, const fs::path& source, const auto& prepare,
                    ImportProgress* progress = nullptr) {
    require_destination(dest);
    const auto temp = dest.parent_path() / (Text(staging) + unique_id());
    if (!fs::create_directory(temp)) fail("Could not create a staging folder.");
    try {
        if (!source.empty()) copy_tree(source, temp, progress);
        prepare(temp);
        fs::rename(temp, dest);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temp, ignored);
        throw;
    }
}

Text line(Text value) {
    std::ranges::replace(value, L'\r', L' ');
    std::ranges::replace(value, L'\n', L' ');
    return value;
}

Text hex(const auto& bytes) {
    constexpr auto digits = L"0123456789abcdef";
    Text result;
    for (auto b : bytes) {
        result += digits[b >> 4];
        result += digits[b & 15];
    }
    return result;
}

bool unhex(std::wstring_view input, auto& bytes) {
    if (input.size() != bytes.size() * 2) return false;
    auto digit = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        c = static_cast<wchar_t>(std::towlower(c));
        return c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
    };
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto a = digit(input[i * 2]), b = digit(input[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        bytes[i] = static_cast<unsigned char>(a * 16 + b);
    }
    return true;
}

using Salt = std::array<unsigned char, 16>;
using Hash = std::array<unsigned char, 32>;

Hash derive(const Text& passcode, Salt salt) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        fail("Windows could not initialize passcode protection.");
    Hash result{};
    const auto code = BCryptDeriveKeyPBKDF2(alg,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(passcode.data())),
        static_cast<ULONG>(passcode.size() * sizeof(wchar_t)), salt.data(),
        static_cast<ULONG>(salt.size()), 180000, result.data(), static_cast<ULONG>(result.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (code < 0) fail("Windows could not derive the passcode hash.");
    return result;
}

Text original_name(const Project& project, const Text& marker) {
    auto name = project.get(L"keeper_original");
    if (valid_name(name)) return name;
    const auto folder = project.name();
    const auto suffix = folder.rfind(marker);
    if (suffix != Text::npos && suffix + marker.size() < folder.size()) {
        const auto tail = folder.substr(suffix + marker.size());
        if (std::ranges::all_of(tail, [](wchar_t ch) { return (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_'; })) {
            name = folder.substr(0, suffix);
            if (valid_name(name)) return name;
        }
    }
    return {};
}

void best_effort_record(const Vault& vault, const Project& project, const Text& event) {
    try { vault.record(project, event); } catch (const std::exception&) {}
}
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto len = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), len, nullptr, 0, nullptr, nullptr);
    if (!size) fail("Text is not valid Unicode.");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), len, result.data(), size, nullptr, nullptr);
    return result;
}

Text wide(std::string_view text) {
    if (text.empty()) return {};
    const auto len = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), len, nullptr, 0);
    if (!size) fail("Text is not valid UTF-8.");
    Text result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), len, result.data(), size);
    return result;
}

Text error_text(const std::exception& error) {
    try { return wide(error.what()); }
    catch (const std::exception&) {
        const std::string_view text(error.what());
        const auto size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (!size) return L"An unexpected Windows or filesystem error occurred.";
        Text value(size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), value.data(), size);
        return value;
    }
}

Text unique_id() {
    GUID guid{};
    wchar_t output[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, output, 40))
        fail("Could not create a unique identifier.");
    return Text(output).substr(1, 36);
}

Text timestamp(bool filename) {
    SYSTEMTIME date{};
    if (filename) GetLocalTime(&date);
    else GetSystemTime(&date);
    wchar_t buffer[48]{};
    swprintf_s(buffer, filename ? L"%04d%02d%02d-%02d%02d%02d" : L"%04d-%02d-%02d %02d:%02d:%02d UTC",
               date.wYear, date.wMonth, date.wDay, date.wHour, date.wMinute, date.wSecond);
    return buffer;
}

Text format_time(fs::file_time_type time) {
    if (time == fs::file_time_type::min()) return L"No source files";
    const auto local = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(time));
    std::tm tm{};
    if (localtime_s(&tm, &local)) return L"Unavailable";
    wchar_t text[32]{};
    wcsftime(text, std::size(text), L"%Y-%m-%d %H:%M:%S", &tm);
    return text;
}

Text format_size(uintmax_t bytes) {
    const wchar_t* units[]{L"bytes", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(units)) { value /= 1024; ++unit; }
    if (!unit) return std::to_wstring(bytes) + L" bytes";
    wchar_t text[64]{};
    swprintf_s(text, L"%.2f %ls", value, units[unit]);
    return text;
}

Text read_text(const fs::path& path, size_t limit) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        fail("Could not read a text file.");
    }
    struct File { HANDLE handle; ~File() { CloseHandle(handle); } } file{handle};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        fail("A metadata or text file is not a regular file.");
    const auto size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > limit) fail("A text file exceeds the size limit.");
    std::string bytes;
    bytes.reserve(static_cast<size_t>(size));
    std::array<char, 8192> chunk;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr))
            fail("Could not read a text file.");
        if (!read) break;
        if (read > limit - bytes.size()) fail("A text file exceeds the size limit.");
        bytes.append(chunk.data(), read);
    }
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    return wide(bytes);
}

void write_text(const fs::path& path, std::wstring_view text) {
    const auto bytes = utf8(text);
    if (exists_entry(path) && (attrs(path) & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        fail("Cannot overwrite a directory or linked file.");
    const auto temp = path.parent_path() / (Text(staging) + unique_id());
    const auto handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("Could not create a temporary file for saving.");
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                    && written == bytes.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        fail("Could not save the file. Check write access and available disk space.");
    }
}

Fields read_fields(const fs::path& path) {
    std::wistringstream stream(read_text(path, 32 * 1024 * 1024));
    Fields fields;
    for (Text entry; std::getline(stream, entry);) {
        if (!entry.empty() && entry.back() == L'\r') entry.pop_back();
        const auto split = entry.find(L'=');
        if (split != Text::npos && split) fields[entry.substr(0, split)] = entry.substr(split + 1);
    }
    return fields;
}

void write_fields(const fs::path& path, const Fields& fields) {
    Text data;
    for (const auto& [key, value] : fields) data += key + L"=" + line(value) + L"\r\n";
    write_text(path, data);
}

bool plain_directory(const fs::path& path) {
    const auto a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool valid_name(std::wstring_view name) {
    if (name.empty() || name.size() > 255 || name.front() == L' ' || name.back() == L' ' ||
        name.back() == L'.' || name.find_first_of(L"<>:\"/\\|?*") != Text::npos ||
        std::ranges::any_of(name, [](wchar_t c) { return c < 32; })) return false;
    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.remove_suffix(1);
    for (auto reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (equal(base, reserved)) return false;
    if (base.size() == 4 && (equal(base.substr(0, 3), L"COM") || equal(base.substr(0, 3), L"LPT")) &&
        std::wstring_view(L"123456789¹²³").find(base.back()) != Text::npos) return false;
    return !equal(name.substr(0, std::wstring_view(staging).size()), staging);
}

bool internal_file(std::wstring_view name) {
    for (auto reserved : {meta, context_name, L".projectkeeper_issues", events})
        if (equal(name, reserved)) return true;
    return equal(name.substr(0, std::wstring_view(staging).size()), staging);
}

fs::path last_vault() {
    constexpr auto registry = L"Software\\ProjectKeeper";
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, registry, L"VaultRoot", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return {};
    Text value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, registry, L"VaultRoot", RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return {};
    value.resize(wcslen(value.c_str()));
    return fs::path(value);
}

bool contains_text(std::wstring_view value, std::wstring_view query) {
    return query.empty() || (!value.empty() &&
        FindStringOrdinal(FIND_FROMSTART, value.data(), static_cast<int>(value.size()),
                          query.data(), static_cast<int>(query.size()), TRUE) >= 0);
}

Text Project::get(const Text& key) const {
    const auto it = fields.find(key);
    return it == fields.end() ? Text{} : it->second;
}

VaultView::VaultView(fs::path root) : VaultView(std::move(root), true) {}

VaultView::VaultView(fs::path root, bool existing_only) : root_(canonical_directory(root)) {
    if (root_ == root_.root_path()) fail("Choose a dedicated folder inside the drive.");
    if (existing_only) {
        for (const auto& dir : {root_ / L".keeper", projects(), trash(), backups()})
            if (!plain_directory(dir)) fail("Choose an existing Project Keeper vault.");
    }
}

Vault::Vault(fs::path path, Open mode, unsigned wait_ms) : VaultView(std::move(path), mode == Open::existing) {
    const auto control = root() / L".keeper";
    const auto areas = {control, projects(), trash(), backups()};
    if (mode == Open::initialize && !std::ranges::all_of(areas, plain_directory)) {
        for (const auto& entry : fs::directory_iterator(root())) {
            if (!std::ranges::any_of(areas, [&](const auto& dir) {
                return equal(entry.path().native(), dir.native()) && plain_directory(entry.path());
            })) fail("Choose an empty folder or an existing Project Keeper vault.");
        }
        for (const auto& dir : areas) {
            fs::create_directory(dir);
            if (!plain_directory(dir)) fail("A vault directory is missing or is a symbolic link or junction.");
        }
    }
    const auto lock_path = control / L"write.lock";
    if (exists_entry(lock_path) && (attrs(lock_path) & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        fail("The vault lock is not a regular file.");
    const auto deadline = GetTickCount64() + wait_ms;
    for (;;) {
        lock_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lock_ != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)
            fail("The vault write lock could not be opened. Check write access.");
        if (GetTickCount64() >= deadline) throw VaultBusy{};
        Sleep(50);
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(lock_, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(lock_);
        fail("The vault lock is not a regular file.");
    }
}

Vault::~Vault() { CloseHandle(lock_); }

std::vector<Project> VaultView::list(const fs::path& area) const {
    if (!plain_directory(area)) fail("A vault folder is unavailable or is a symbolic link or junction.");
    std::vector<Project> projects;
    for (const auto& item : fs::directory_iterator(area)) {
        if (!plain_directory(item.path()) || !valid_name(item.path().filename().native())) continue;
        try {
            projects.push_back(load(item.path()));
        } catch (const std::exception&) {
            projects.push_back({item.path(), {{L"keeper_read_error", L"1"}, {L"locked", L"1"}}});
        }
    }
    std::ranges::sort(projects, [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.name().c_str(), -1, b.name().c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return projects;
}

Project VaultView::load(const fs::path& dir) const {
    const auto parent = dir.parent_path();
    if (parent != projects() && parent != trash() && parent != backups()) fail("Folder is outside the vault.");
    if (!plain_directory(parent) || !plain_directory(dir) || !valid_name(dir.filename().native()))
        fail("Project folder is unavailable.");
    return {dir, read_fields(dir / meta)};
}

void Vault::save(const Project& project) const {
    const auto parent = project.path.parent_path();
    if (parent != projects() && parent != trash() && parent != backups()) fail("Folder is outside the vault.");
    verify_entry(project, parent);
    write_fields(project.path / meta, project.fields);
}

Project Vault::create(const Text& name, const Text& description, const fs::path& input, ImportProgress* progress) const {
    if (!valid_name(name) || !plain_directory(projects())) fail("Use a valid Windows project folder name.");
    const auto dest = projects() / name;
    fs::path source;
    if (!input.empty()) {
        source = canonical_directory(input);
        if (within(root(), source) || within(source, root())) fail("Choose an import folder outside this vault.");
    }
    staged_project(dest, source, [&](const fs::path& temp) {
        auto fields = read_fields(temp / meta);
        fields[L"format"] = L"1";
        if (!description.empty() || !fields.contains(L"description")) fields[L"description"] = line(description);
        write_fields(temp / meta, fields);
        if (!exists_entry(temp / project_notes))
            write_text(temp / project_notes, L"# Project notes\n\n## Requirements and decisions\n\n## Current status\n\n## Next development steps\n");
    }, progress);
    auto project = load(dest);
    best_effort_record(*this, project, input.empty() ? L"Created project" : L"Imported a copy of a folder");
    return project;
}

Project Vault::rename(Project project, const Text& name) const {
    verify_entry(project, projects());
    if (!valid_name(name)) fail("Use a valid Windows project folder name.");
    if (project.name() == name) return project;
    const auto dest = projects() / name;
    if (!equal(project.name(), name)) require_destination(dest);
    const auto previous = project.name();
    fs::rename(project.path, dest);
    project.path = dest;
    best_effort_record(*this, project, L"Renamed from " + previous);
    return project;
}

void Vault::erase(Project project) const {
    verify_entry(project, projects());
    if (!plain_directory(trash())) fail("Trash is unavailable.");
    const auto old = project.path;
    const auto original = project.name();
    const auto destination = trash() / (original.substr(0, 190) + L"__deleted_" + timestamp(true) + L"-" + unique_id());
    project.fields[L"keeper_original"] = original;
    save(project);
    fs::rename(old, destination);
    project.path = destination;
    best_effort_record(*this, project, L"Moved to Trash");
}

void Vault::restore_trash(Project project) const {
    verify_entry(project, trash());
    if (!plain_directory(projects()) || project.name().starts_with(L"failed_import_"))
        fail("The Trash item cannot be restored as a project.");
    const auto name = original_name(project, L"__deleted_");
    if (!valid_name(name)) fail("The original project name could not be determined.");
    const auto dest = projects() / name;
    require_destination(dest);
    fs::rename(project.path, dest);
    project.path = dest;
    best_effort_record(*this, project, L"Restored from Trash");
}

void Vault::purge(Project project) const {
    verify_entry(project, trash());
    for (const auto& entry : fs::recursive_directory_iterator(project.path)) {
        const auto a = attrs(entry.path());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT))
            fail("Trash folder has an unavailable entry or link. Permanent deletion stopped.");
    }
    fs::remove_all(project.path);
}

Project Vault::backup(Project project) const {
    verify_entry(project, projects());
    if (!plain_directory(backups())) fail("Backups folder is unavailable.");
    const auto name = project.name();
    const auto dest = backups() / (name.substr(0, 190) + L"__backup_" + timestamp(true) + L"-" + unique_id());
    staged_project(dest, project.path, [&](const fs::path& temp) {
        auto fields = project.fields;
        fields[L"keeper_original"] = name;
        fields[L"keeper_snapshot"] = timestamp();
        write_fields(temp / meta, fields);
        record({temp, fields}, L"Created backup of " + name);
    });
    best_effort_record(*this, project, L"Created backup " + dest.filename().native());
    return load(dest);
}

Project Vault::recover(Project snapshot, const Text& name) const {
    verify_entry(snapshot, backups());
    if (snapshot.name().ends_with(L"__INCOMPLETE")) fail("Incomplete backups cannot be recovered.");
    if (!valid_name(name) || !plain_directory(projects())) fail("Use a valid Windows project folder name.");
    const auto dest = projects() / name;
    auto fields = snapshot.fields;
    fields.erase(L"keeper_snapshot");
    fields.erase(L"keeper_original");
    staged_project(dest, snapshot.path, [&](const fs::path& temp) {
        write_fields(temp / meta, fields);
        record({temp, fields}, L"Recovered from backup " + snapshot.name());
    });
    return load(dest);
}

Stats VaultView::scan(const Project& project) const {
    Stats stats;
    if (!plain_directory(project.path.parent_path()) || !plain_directory(project.path))
        fail("Project folder is unavailable.");
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(project.path, fs::directory_options::none, error);
         it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) { ++stats.skipped; error.clear(); }
        const auto& item = *it;
        const auto a = attrs(item.path());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            it.disable_recursion_pending();
            ++stats.skipped;
            continue;
        }
        if (item.path().parent_path() == project.path && internal_file(item.path().filename().native())) {
            it.disable_recursion_pending();
            continue;
        }
        if (a & FILE_ATTRIBUTE_DIRECTORY) continue;
        const auto size = item.file_size(error);
        if (error) { ++stats.skipped; error.clear(); continue; }
        stats.bytes += size;
        ++stats.files;
        const auto time = item.last_write_time(error);
        if (error) { ++stats.skipped; error.clear(); continue; }
        stats.modified = std::max(stats.modified, time);
    }
    if (error) ++stats.skipped;
    return stats;
}

void Vault::context(const Project& project, const Stats& stats) const {
    verify_entry(project, projects());
    Text result = L"# Project context\n\n";
    result += L"- Project: " + project.name() + L"\n- Description: " + project.get(L"description");
    result += L"\n- Last source change: " + format_time(stats.modified) + L" (local time)";
    result += L"\n- Files: " + std::to_wstring(stats.files) + L"\n- Size: " + format_size(stats.bytes);
    if (stats.skipped) result += L"\n- Unavailable entries or links: " + std::to_wstring(stats.skipped);
    if (!project.get(L"repo").empty()) result += L"\n- GitHub: https://github.com/" + project.get(L"repo");
    result += L"\n\n## Top-level contents\n\n";
    std::vector<Text> entries;
    for (const auto& item : fs::directory_iterator(project.path)) {
        const auto name = item.path().filename().native();
        if (!internal_file(name)) entries.push_back(name + (plain_directory(item.path()) ? L"/" : L""));
    }
    std::ranges::sort(entries);
    for (size_t i = 0; i < std::min<size_t>(entries.size(), 60); ++i) result += L"- " + entries[i] + L"\n";
    if (entries.size() > 60) result += L"- Additional entries omitted\n";
    for (auto filename : {L"README.md", L"README.txt"}) {
        try {
            auto excerpt = read_text(project.path / filename, 256 * 1024);
            if (excerpt.empty()) continue;
            if (excerpt.size() > 4000) {
                const auto length = excerpt[3999] >= 0xD800 && excerpt[3999] <= 0xDBFF ? 3999u : 4000u;
                excerpt = excerpt.substr(0, length) + L"\n…";
            }
            result += L"\n## README excerpt\n\n" + excerpt + L"\n";
            break;
        } catch (const std::exception&) { continue; }
    }
    result += L"\n## Work continuity\n\nRead PROJECT_NOTES.md for requirements and decisions.\n";
    result += L"\n> Generated locally by Project Keeper from the project folder.\n";
    const auto target = project.path / context_name;
    if (read_text(target) != result) write_text(target, result);
}

void Vault::record(const Project& project, const Text& event) const {
    auto current = read_text(project.path / events, 2 * 1024 * 1024);
    if (current.size() > 240000) {
        const auto first = current.find(L'\n', current.size() - 200000);
        if (first != Text::npos) current.erase(0, first + 1);
    }
    write_text(project.path / events, current + timestamp() + L"  " + line(event) + L"\r\n");
}

Text VaultView::history(const Project& project) const {
    return read_text(project.path / events, 2 * 1024 * 1024);
}

void Vault::lock_project(Project& project, const Text& passcode) {
    if (passcode.empty()) {
        project.fields[L"locked"] = L"0";
        project.fields.erase(L"salt");
        project.fields.erase(L"hash");
        return;
    }
    if (passcode.size() < 6 || passcode.size() > 512) fail("Use a passcode of 6 to 512 characters.");
    Salt salt{};
    if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        fail("Windows could not generate a passcode salt.");
    const auto hash = derive(passcode, salt);
    project.fields[L"salt"] = hex(salt);
    project.fields[L"hash"] = hex(hash);
    project.fields[L"locked"] = L"1";
}

bool Vault::verify(const Project& project, const Text& passcode) {
    if (!project.flag(L"locked")) return true;
    Salt salt{};
    Hash expected{};
    if (passcode.size() > 512 || !unhex(project.get(L"salt"), salt) || !unhex(project.get(L"hash"), expected)) return false;
    const auto actual = derive(passcode, salt);
    unsigned diff = 0;
    for (size_t i = 0; i < actual.size(); ++i) diff |= actual[i] ^ expected[i];
    return diff == 0;
}

int self_test(const fs::path& parent) {
    const auto dir = parent / (L"keeper-test-" + unique_id());
    const auto log = parent / L"self-test-result.txt";
    fs::create_directories(dir / L"vault");
    auto check = [](bool test, const char* message) { if (!test) fail(message); };
    Text outcome = L"PASS";
    try {
        Vault vault(dir / L"vault");
        try { Vault second(dir / L"vault"); fail("Vault opened twice."); }
        catch (const VaultBusy&) {}
        fs::create_directories(dir / L"input" / L"subfolder");
        write_text(dir / L"input" / L"subfolder" / L"source.txt", L"original");
        write_text(dir / L"input" / L"README.md", L"Readme excerpt");
        ImportProgress progress;
        auto project = vault.create(L"Unicode \u6E2C\u8A66", L"Project description", dir / L"input", &progress);
        check(progress.stage == ImportStage::finishing && progress.files_total == 2 && progress.files_copied == 2 &&
              progress.bytes_total == 22 && progress.bytes_copied == 22, "Import progress totals were incorrect.");
        const auto first = vault.scan(project);
        vault.context(project, first);
        check(first.files == 3 && first.bytes > 8, "Project stats failed.");
        check(read_text(project.path / context_name).find(L"Readme excerpt") != Text::npos, "Context generation failed.");
        check(vault.scan(project).files == first.files, "Generated context affected file stats.");
        Vault::lock_project(project, L"safe passcode");
        vault.save(project);
        project = vault.load(project.path);
        check(Vault::verify(project, L"safe passcode") && !Vault::verify(project, L"wrong"), "Passcode verification failed.");
        Project old{project.path, {{L"locked", L"1"}, {L"salt", L"000102030405060708090a0b0c0d0e0f"},
            {L"hash", L"5758e568d91e4d93d19781aded24f19b11efa1a4d8654e7f9c2fbdabca650eec"}}};
        check(Vault::verify(old, L"legacy password"), "Legacy passcode verification failed.");
        const auto snapshot = vault.backup(project);
        write_text(project.path / L"subfolder" / L"source.txt", L"changed");
        project = vault.rename(project, L"Renamed");
        vault.erase(project);
        const auto deleted = vault.list(vault.trash());
        check(deleted.size() == 1 && vault.list(vault.projects()).empty(), "Soft deletion failed.");
        vault.restore_trash(deleted.front());
        check(read_text(vault.projects() / L"Renamed" / L"subfolder" / L"source.txt") == L"changed", "Trash restore lost data.");
        const auto restored = vault.recover(snapshot, L"Recovered");
        check(read_text(restored.path / L"subfolder" / L"source.txt") == L"original", "Backup recovery lost data.");
        check(read_text(dir / L"input" / L"subfolder" / L"source.txt") == L"original", "Import modified the source.");
        check(vault.history(restored).find(L"Recovered from backup") != Text::npos, "History failed.");
        try { vault.recover(snapshot, L"Renamed"); fail("Backup replaced an existing project."); }
        catch (const std::runtime_error& e) {
            check(std::string_view(e.what()).find("already exists") != std::string_view::npos, "Backup collision handling failed.");
        }
        fs::create_directory(vault.trash() / L"Legacy__deleted_123456789");
        vault.restore_trash(vault.load(vault.trash() / L"Legacy__deleted_123456789"));
        check(plain_directory(vault.projects() / L"Legacy"), "Legacy Trash recovery failed.");
        check(!valid_name(L"LPT¹.txt") && !valid_name(L"../foo") && !valid_name(L"CON .txt"), "Unsafe name accepted.");
        const auto plain = vault.create(L"New project", L"From scratch");
        check(plain.get(L"description") == L"From scratch", "Project creation failed.");
        vault.erase(plain);
        vault.purge(vault.list(vault.trash()).front());
        check(vault.list(vault.trash()).empty(), "Permanent deletion failed.");
        github_self_test();
    } catch (const std::exception& e) { outcome = L"FAIL: " + error_text(e); }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    write_text(log, outcome + L"\r\n");
    return outcome == L"PASS" ? 0 : 1;
}
}

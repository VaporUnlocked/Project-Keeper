#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pk {
namespace fs = std::filesystem;
using Text = std::wstring;
using Fields = std::map<Text, Text>;
inline constexpr auto mcp_change_file = L"mcp-change";
inline constexpr auto project_notes = L"PROJECT_NOTES.md";
inline constexpr size_t project_notes_limit = 512 * 1024;

std::string utf8(std::wstring_view text);
Text wide(std::string_view text);
Text error_text(const std::exception& error);
Text timestamp(bool filename = false);
Text format_time(fs::file_time_type time);
Text format_size(uintmax_t bytes);
Text unique_id();
Text read_text(const fs::path& path, size_t limit = 512 * 1024);
void write_text(const fs::path& path, std::wstring_view text);
Fields read_fields(const fs::path& path);
void write_fields(const fs::path& path, const Fields& fields);
bool plain_directory(const fs::path& path);
bool valid_name(std::wstring_view name);
bool internal_file(std::wstring_view name);
fs::path last_vault();
bool contains_text(std::wstring_view value, std::wstring_view query);

struct Stats {
    uintmax_t bytes = 0, files = 0;
    fs::file_time_type modified = fs::file_time_type::min();
    size_t skipped = 0;
};

enum class ImportStage { preparing, copying, finishing };

struct ImportProgress {
    std::atomic<ImportStage> stage{ImportStage::preparing};
    std::atomic<uintmax_t> bytes_total{}, bytes_copied{}, files_total{}, files_copied{};
};

struct Project {
    fs::path path;
    Fields fields;
    Text name() const { return path.filename().wstring(); }
    Text get(const Text& key) const;
    bool flag(const Text& key) const { return get(key) == L"1"; }
};

struct Issue {
    unsigned number;
    Text title;
    bool open = true;
};

struct RepoStats {
    unsigned stars = 0, forks = 0;
    std::vector<Issue> issues;
};

struct IssueUpdate {
    Fields cache;
    size_t open = 0, unread = 0;
};
IssueUpdate issue_update(Fields prior, const RepoStats& stats);
std::vector<Issue> cached_issues(const Fields& cache);
void github_self_test();
Text normalize_repo(Text input);
std::map<Text, RepoStats> github_sync(const std::set<Text>& repos, Text token,
                                     std::map<Text, Text>& errors);
Text github_token();
void set_github_token(const Text& token);

class VaultView {
    fs::path root_;

protected:
    VaultView(fs::path root, bool existing_only);

public:
    explicit VaultView(fs::path root);
    const fs::path& root() const { return root_; }
    fs::path projects() const { return root_ / L"Projects"; }
    fs::path trash() const { return root_ / L"Trash"; }
    fs::path backups() const { return root_ / L"Backups"; }

    std::vector<Project> list(const fs::path& area) const;
    Project load(const fs::path& dir) const;
    Stats scan(const Project& project) const;
    Text history(const Project& project) const;
};

class Vault : public VaultView {
    HANDLE lock_ = INVALID_HANDLE_VALUE;

public:
    enum class Open { initialize, existing };
    explicit Vault(fs::path root, Open mode = Open::initialize, unsigned wait_ms = 0);
    ~Vault();
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    void save(const Project& project) const;
    Project create(const Text& name, const Text& description, const fs::path& source = {},
                   ImportProgress* progress = nullptr) const;
    Project rename(Project project, const Text& name) const;
    void erase(Project project) const;
    void restore_trash(Project project) const;
    void purge(Project project) const;
    Project backup(Project project) const;
    Project recover(Project snapshot, const Text& name) const;

    void context(const Project& project, const Stats& stats) const;
    void record(const Project& project, const Text& action) const;
    static void lock_project(Project& project, const Text& passcode);
    static bool verify(const Project& project, const Text& passcode);
};

class VaultBusy : public std::runtime_error {
public:
    VaultBusy() : std::runtime_error("The vault is busy with another write. Try again when the current operation finishes.") {}
};

int self_test(const fs::path& parent);
int run_mcp(const fs::path& configured_vault);
}

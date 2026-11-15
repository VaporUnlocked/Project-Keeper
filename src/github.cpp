#include "json.h"

#include <wincred.h>
#include <winhttp.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <sstream>

namespace pk {
namespace {
constexpr auto token_name = L"ProjectKeeper.GitHub";

struct HttpClose {
    void operator()(void* handle) const { WinHttpCloseHandle(handle); }
};
using Http = std::unique_ptr<void, HttpClose>;

struct Response {
    DWORD status = 0;
    Text links;
    Text retry;
    Text remaining;
    Text reset;
    std::string body;
};

Text header(HINTERNET request, Text name) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name.c_str(), nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (!size || size > 64 * 1024) return {};
    Text value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name.c_str(), value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

std::runtime_error network_error() {
    return std::runtime_error("GitHub network request failed (Windows error " + std::to_string(GetLastError()) + ").");
}

Response get(HINTERNET connection, const Text& path, const Text& token) {
    Http request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) throw network_error();
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) throw network_error();
    Text headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + token + L"\r\n";
    const bool sent = WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr)) throw network_error();
    Response response;
    DWORD size = sizeof(response.status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &size, WINHTTP_NO_HEADER_INDEX))
        throw network_error();
    response.links = header(request.get(), L"Link");
    response.retry = header(request.get(), L"Retry-After");
    response.remaining = header(request.get(), L"X-RateLimit-Remaining");
    response.reset = header(request.get(), L"X-RateLimit-Reset");
    char buffer[8192];
    for (;;) {
        DWORD count = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &count)) throw network_error();
        if (!count) break;
        response.body.append(buffer, count);
        if (response.body.size() > 32 * 1024 * 1024) throw std::runtime_error("GitHub response exceeded the size limit.");
    }
    return response;
}

uint64_t decimal(const Text& value) {
    if (value.empty() || !std::ranges::all_of(value, [](auto c) { return c >= L'0' && c <= L'9'; })) return 0;
    try { return std::stoull(value); } catch (const std::exception&) { return 0; }
}

uint64_t utc_seconds() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER raw{};
    raw.HighPart = time.dwHighDateTime;
    raw.LowPart = time.dwLowDateTime;
    return raw.QuadPart / 10000000 - 11644473600ULL;
}

// Only the single sync worker accesses this timestamp.
uint64_t cooldown_until = 0;

Json response_json(const Response& response) {
    if (response.status == 200) return Parser(response.body).parse();
    if (response.status == 429 || (response.status == 403 &&
        (!response.retry.empty() || response.remaining == L"0" || response.body.find("rate limit") != std::string::npos))) {
        const auto now = utc_seconds();
        const auto seconds = decimal(response.retry);
        const auto reset = response.remaining == L"0" ? decimal(response.reset) : 0;
        cooldown_until = std::max(now + (seconds ? std::min<uint64_t>(seconds, 86400) : 60), reset);
        throw std::runtime_error("GitHub rate limited. The next cycle will retry after the cooldown.");
    }
    if (response.status == 301 || response.status == 302 || response.status == 307 || response.status == 308)
        throw std::runtime_error("GitHub moved this repository. Update the project link.");
    throw std::runtime_error("GitHub returned HTTP " + std::to_string(response.status) + ".");
}

void add_issues(const Json& page, std::map<unsigned, Issue>& issues) {
    for (const auto& row : std::get<Json::Array>(page.value)) {
        if (row.find("pull_request")) continue;
        const auto number = row.at("number").number();
        if (!number) throw std::runtime_error("GitHub returned an invalid issue number.");
        const auto state = row.at("state").text();
        if (state != L"open" && state != L"closed") throw std::runtime_error("GitHub returned an invalid issue state.");
        auto title = row.at("title").text();
        std::ranges::replace(title, L'\r', L' ');
        std::ranges::replace(title, L'\n', L' ');
        std::ranges::replace(title, L'\0', L' ');
        issues.emplace(number, Issue{number, std::move(title), state == L"open"});
    }
}

Text next_issue_query(std::wstring_view links) {
    // Use GitHub's cursor or page number as supplied in the Link header.
    const auto relation = links.find(L">; rel=\"next\"");
    if (relation == Text::npos) return {};
    const auto start = links.rfind(L'<', relation);
    if (start == Text::npos) throw std::runtime_error("GitHub returned an invalid pagination link.");
    const auto url = links.substr(start + 1, relation - start - 1);
    const auto query = url.find(L'?');
    if (!url.starts_with(L"https://api.github.com/") || query == Text::npos || query + 1 == url.size() ||
        !std::ranges::all_of(url, [](auto ch) { return ch > L' ' && ch < 127 && ch != L'#'; }))
        throw std::runtime_error("GitHub returned an invalid pagination link.");
    return Text(url.substr(query));
}

RepoStats get_repo(HINTERNET connection, const Text& repo, const Text& token) {
    const Text path = L"/repos/" + repo;
    const auto info = response_json(get(connection, path, token));
    RepoStats result{info.at("stargazers_count").number(), info.at("forks_count").number(), {}};
    std::map<unsigned, Issue> issues;
    // Open issues are fully paginated. Fetch recent closed issues too, so short-lived
    // issues can still be seen after they close between two syncs.
    const auto endpoint = path + L"/issues";
    constexpr auto parameters = L"?per_page=100&sort=created&direction=desc&state=";
    Text query = Text(parameters) + L"open";
    std::set<Text> visited;
    for (unsigned page = 1;; ++page) {
        if (!visited.insert(query).second) throw std::runtime_error("GitHub repeated an issue pagination link.");
        const auto response = get(connection, endpoint + query, token);
        add_issues(response_json(response), issues);
        query = next_issue_query(response.links);
        if (query.empty()) break;
        if (page >= 500) throw std::runtime_error("Repository exceeds the open issue pagination limit.");
    }
    add_issues(response_json(get(connection, endpoint + parameters + L"closed", token)), issues);
    for (auto it = issues.rbegin(); it != issues.rend(); ++it) result.issues.push_back(std::move(it->second));
    return result;
}

std::set<unsigned> old_numbers(const Fields& prior, const Text& field) {
    const auto it = prior.find(field);
    if (it == prior.end()) return {};
    auto value = it->second;
    for (auto& c : value) if (c < L'0' || c > L'9') c = L' ';
    std::wistringstream stream(value);
    std::set<unsigned> numbers;
    for (unsigned number = 0; stream >> number;) numbers.insert(number);
    return numbers;
}
}

Text normalize_repo(Text input) {
    const auto first = input.find_first_not_of(L" \t\r\n");
    if (first == Text::npos) return {};
    input = input.substr(first, input.find_last_not_of(L" \t\r\n") - first + 1);
    Text lower = input;
    std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (lower.starts_with(L"https://github.com/")) lower.erase(0, 19);
    if (lower.ends_with(L'/')) lower.pop_back();
    const auto slash = lower.find(L'/');
    const auto invalid = []() -> Text { throw std::runtime_error("Use a GitHub repository URL or owner/repository."); };
    if (slash == Text::npos || slash == 0 || slash > 39 || slash + 1 == lower.size() || lower.size() - slash > 101)
        return invalid();
    const auto owner = lower.substr(0, slash);
    const auto repo = lower.substr(slash + 1);
    const auto alpha_num = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); };
    if (owner.front() == L'-' || owner.back() == L'-' || repo == L"." || repo == L".." ||
        !std::ranges::all_of(owner, [&](auto c) { return alpha_num(c) || c == L'-'; }) ||
        !std::ranges::all_of(repo, [&](auto c) { return alpha_num(c) || c == L'-' || c == L'_' || c == L'.'; }))
        return invalid();
    return lower;
}

IssueUpdate issue_update(Fields prior, const RepoStats& stats) {
    IssueUpdate update;
    auto seen = old_numbers(prior, L"seen");
    auto new_issues = old_numbers(prior, L"new");
    const bool baseline = prior[L"baseline"] == L"1";
    auto high = decimal(prior[L"high"]);
    if (!seen.empty()) high = std::max<uint64_t>(high, *seen.rbegin());
    uint64_t next_high = high;
    Fields cache{{L"baseline", L"1"}};
    for (const auto& issue : stats.issues) {
        if (issue.open) ++update.open;
        const auto key = std::to_wstring(issue.number);
        next_high = std::max<uint64_t>(next_high, issue.number);
        if (baseline && issue.number > high) new_issues.insert(issue.number);
        if (issue.open || new_issues.contains(issue.number))
            cache[L"issue." + key] = (issue.open ? L"1" : L"0") + issue.title;
    }
    bool trimmed = false;
    while (new_issues.size() > 10000) {
        new_issues.erase(new_issues.begin());
        trimmed = true;
    }
    if (trimmed || prior[L"unread_trimmed"] == L"1") cache[L"unread_trimmed"] = L"1";
    for (auto issue : new_issues) {
        const auto key = std::to_wstring(issue);
        const auto field = L"issue." + key;
        if (!cache.contains(field)) {
            const auto it = prior.find(field);
            const auto title = it != prior.end() && !it->second.empty() ? it->second.substr(1) : L"(view on GitHub)";
            cache[field] = L"0" + title;
        }
        if (!cache[L"new"].empty()) cache[L"new"] += L",";
        cache[L"new"] += key;
    }
    cache[L"high"] = std::to_wstring(next_high);
    cache[L"seen"] = cache[L"high"];
    update.unread = new_issues.size();
    update.cache = std::move(cache);
    return update;
}

std::vector<Issue> cached_issues(const Fields& cache) {
    std::vector<Issue> issues;
    for (const auto& [key, value] : cache) {
        if (!key.starts_with(L"issue.") || value.empty()) continue;
        const auto number = decimal(key.substr(6));
        if (!number || number > UINT_MAX) continue;
        issues.push_back({static_cast<unsigned>(number), value.substr(1), value[0] == L'1'});
    }
    std::ranges::sort(issues, std::greater{}, &Issue::number);
    return issues;
}

std::map<Text, RepoStats> github_sync(const std::set<Text>& repos, Text token, std::map<Text, Text>& errors) {
    struct Erase { Text& value; ~Erase() { SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); } } erase{token};
    std::map<Text, RepoStats> results;
    if (repos.empty()) return results;
    Http session(WinHttpOpen(L"ProjectKeeper/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    Http connection;
    if (session) {
        WinHttpSetTimeouts(session.get(), 5000, 5000, 7000, 7000);
        connection.reset(WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    }
    for (const auto& repo : repos) {
        try {
            if (utc_seconds() < cooldown_until) throw std::runtime_error("GitHub rate limit cooldown is active.");
            if (!connection) throw network_error();
            results.emplace(repo, get_repo(connection.get(), normalize_repo(repo), token));
        } catch (const std::exception& error) { errors[repo] = error_text(error); }
    }
    return results;
}

Text github_token() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(token_name, CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() == ERROR_NOT_FOUND) return {};
        throw std::runtime_error("Could not read Windows Credential Manager.");
    }
    std::unique_ptr<CREDENTIALW, decltype(&CredFree)> guard(credential, CredFree);
    Text token;
    if (credential->CredentialBlobSize % sizeof(wchar_t) == 0) {
        for (DWORD offset = 0; offset < credential->CredentialBlobSize; offset += sizeof(wchar_t)) {
            wchar_t ch;
            memcpy(&ch, credential->CredentialBlob + offset, sizeof(ch));
            if (!ch) break;
            token += ch;
        }
    }
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    return token;
}

void set_github_token(const Text& token) {
    if (token.empty()) {
        if (!CredDeleteW(token_name, CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
            throw std::runtime_error("Could not remove the Windows credential.");
        return;
    }
    if (token.size() > 512 || !std::ranges::all_of(token, [](auto c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_';
    })) throw std::runtime_error("The token contains unsupported characters.");
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(token_name);
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(token.data()));
    credential.CredentialBlobSize = static_cast<DWORD>(token.size() * sizeof(wchar_t));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) throw std::runtime_error("Could not save the token in Windows Credential Manager.");
}

void github_self_test() {
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("GitHub fixture test failed."); };
    require(next_issue_query(LR"(<https://api.github.com/repositories/42/issues?state=open&after=Cursor%3D&page=2>; rel="next", <https://api.github.com/repositories/42/issues?page=1>; rel="prev")")
            == L"?state=open&after=Cursor%3D&page=2");
    require(next_issue_query(LR"(<https://api.github.com/repos/example/tool/issues?page=2>; rel="next")") == L"?page=2");
    require(next_issue_query(LR"(<https://api.github.com/repos/example/tool/issues?page=1>; rel="prev")").empty());
    try { next_issue_query(LR"(<https://example.com/issues?page=2>; rel="next")"); require(false); }
    catch (const std::runtime_error& e) { require(std::string_view(e.what()).starts_with("GitHub returned an invalid pagination link")); }
    require(normalize_repo(L" https://github.com/Microsoft/Terminal/ ") == L"microsoft/terminal");
    try { normalize_repo(L"https://example.com/a/b"); require(false); }
    catch (const std::runtime_error& e) { require(std::string_view(e.what()).starts_with("Use a GitHub")); }
    const auto response = Parser(R"([{"number":4,"title":"Escaped \uD83D\uDE00","state":"open"},{"number":5,"title":"PR","state":"open","pull_request":{}},{"number":6,"title":"Closed","state":"closed"}])").parse();
    std::map<unsigned, Issue> parsed;
    add_issues(response, parsed);
    require(parsed.size() == 2 && parsed.at(4).title.size() == 10 && !parsed.at(6).open);
    auto update = issue_update({}, {10, 2, {parsed.at(4), parsed.at(6)}});
    require(update.open == 1 && update.unread == 0);
    update = issue_update(std::move(update.cache), {11, 2, {{7, L"New", true}, {4, L"Existing", true}}});
    require(update.open == 2 && update.unread == 1);
    update = issue_update(std::move(update.cache), {11, 2, {{7, L"New", false}, {4, L"Existing", true}}});
    require(update.open == 1 && update.unread == 1 && cached_issues(update.cache).size() == 2);
    update.cache.erase(L"new");
    update = issue_update(std::move(update.cache), {11, 2, {{7, L"New", false}}});
    require(update.open == 0 && update.unread == 0);
}
}

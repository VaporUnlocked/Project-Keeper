#include "keeper.h"

#include <memory>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> free_args(args, LocalFree);
    if (!args) return 1;
    int first = 1;
    if (argc > first && std::wstring_view(args[first]) == L"--mcp") ++first;
    try {
        if (argc == first) return pk::run_mcp({});
        if (argc == first + 2 && std::wstring_view(args[first]) == L"--vault")
            return pk::run_mcp(args[first + 1]);
        return 1;
    } catch (const std::exception&) { return 1; }
}

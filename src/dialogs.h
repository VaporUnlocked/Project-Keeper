#pragma once

#include "keeper.h"

#include <optional>

namespace pk {
std::optional<Text> prompt(HWND owner, Text caption, Text label, Text value = {},
                           bool secret = false, bool viewer = false);
bool notice(HWND owner, Text caption, Text message, bool confirmation = false);

struct Draft { Text name, description; };
std::optional<Draft> project_prompt(HWND owner, const fs::path& source);
void check_project_prompt(HWND owner);
}

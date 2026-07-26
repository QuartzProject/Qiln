#pragma once

#include "build/Compile.hpp"
#include "model/Model.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace qiln {

/// Runs every `@test` of `program`, each in a fresh sandbox; true when all pass.
std::expected<bool, std::string> runTests(const Compiled& compiled, const Limits& limits, std::string_view filter);

/// Runs `entry` as a task, answering its waits like `quartz run` does; returns the exit code to report.
std::expected<int, std::string> runEntry(const Compiled& compiled, const Limits& limits, const std::string& entry);

} // namespace qiln

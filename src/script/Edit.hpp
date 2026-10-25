#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace qiln::edit {

/// A dependency as `qiln add` writes it: a package notation (`org.example.text:^1.2`) or a project path (`:core`).
struct Dependency {
    std::string notation;
    bool test = false; // `testImplementation` instead of `implementation`
};

/// What an edit did, for the message `qiln add` and `qiln remove` print.
enum class Change { Added, Updated, Unchanged, Removed };

struct Edited {
    std::string text;
    Change change = Change::Added;
};

/// `build.qz` with the dependency added to its `dependencies([...])` list, or its range or configuration changed in
/// place; the rest of the text stays byte for byte as it was.
std::expected<Edited, std::string> addDependency(std::string_view script, const Dependency& d);

/// `build.qz` without the dependency on `target`, a package name or a project path.
std::expected<Edited, std::string> removeDependency(std::string_view script, std::string_view target);

} // namespace qiln::edit

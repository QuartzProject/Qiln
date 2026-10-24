#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// What one task printed, kept until the tasks before it have printed theirs.
class TaskLog {
public:
    void write(bool error, std::string_view text);
    /// Writes every chunk to standard output or error, in the order they came.
    void replay() const;

private:
    std::vector<std::pair<bool, std::string>> chunks_;
};

/// Sends this thread's output to `log` while it lives; a null log prints straight to the terminal.
class LogScope {
public:
    explicit LogScope(TaskLog* log);
    ~LogScope();
    LogScope(const LogScope&) = delete;
    LogScope& operator=(const LogScope&) = delete;

private:
    TaskLog* previous_;
};

/// A line of task output: to the current task's log, or to standard output.
void say(std::string_view line);
/// Text for standard error (compiler diagnostics, warnings), through the current task's log.
void complain(std::string_view text);

} // namespace qiln

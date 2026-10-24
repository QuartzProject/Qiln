#include "build/Console.hpp"

#include <cstdio>

namespace qiln {

namespace {

thread_local TaskLog* current = nullptr;

void put(bool error, std::string_view text) {
    std::FILE* f = error ? stderr : stdout;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
}

} // namespace

void TaskLog::write(bool error, std::string_view text) {
    if (!chunks_.empty() && chunks_.back().first == error) {
        chunks_.back().second.append(text);
        return;
    }
    chunks_.emplace_back(error, std::string(text));
}

void TaskLog::replay() const {
    for (const auto& [error, text] : chunks_) {
        put(error, text);
    }
}

LogScope::LogScope(TaskLog* log) : previous_(current) { current = log; }

LogScope::~LogScope() { current = previous_; }

void say(std::string_view line) {
    std::string text(line);
    text += '\n';
    if (current != nullptr) {
        current->write(false, text);
    } else {
        put(false, text);
    }
}

void complain(std::string_view text) {
    if (text.empty()) {
        return;
    }
    if (current != nullptr) {
        current->write(true, text);
    } else {
        put(true, text);
    }
}

} // namespace qiln

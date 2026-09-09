#include "support/Files.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace qiln {

std::optional<std::string> readText(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::expected<std::optional<std::vector<std::uint8_t>>, std::string> readLimited(const fs::path& path,
                                                                                 std::size_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::optional<std::vector<std::uint8_t>>();
    }
    std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > limit) {
        return std::unexpected(std::format("{} is larger than {} bytes", path.string(), limit));
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return std::unexpected("cannot read " + path.string());
    }
    return std::optional(std::move(bytes));
}

std::expected<void, std::string> writeAtomically(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::random_device rd;
    fs::path tmp = path;
    tmp += std::format(".tmp{:08x}", rd());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return std::unexpected("cannot write " + path.string());
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        std::string why = ec.message();
        fs::remove(tmp, ec);
        return std::unexpected(std::format("cannot write {}: {}", path.string(), why));
    }
    return {};
}

std::expected<void, std::string> writeAtomically(const fs::path& path, std::string_view text) {
    return writeAtomically(path, bytesOf(text));
}

fs::path qilnHome() {
    if (const char* q = std::getenv("QILN_HOME"); q != nullptr && *q != '\0') {
        return q;
    }
#ifdef _WIN32
    if (const char* u = std::getenv("USERPROFILE"); u != nullptr && *u != '\0') {
        return fs::path(u) / ".qiln";
    }
#else
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') {
        return fs::path(h) / ".qiln";
    }
#endif
    return fs::temp_directory_path() / "qiln-home";
}

bool safeRelativePath(std::string_view path) {
    if (path.empty() || path.size() > 1024) {
        return false;
    }
    while (true) {
        std::size_t slash = path.find('/');
        std::string_view part = path.substr(0, slash);
        if (part.empty() || part.front() == '.' || part.find_first_of("\\:\0", 0, 3) != std::string_view::npos) {
            return false;
        }
        for (char c : part) {
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        path.remove_prefix(slash + 1);
    }
}

std::expected<std::vector<std::string>, std::string> listFiles(const fs::path& base, const fs::path& dir,
                                                               std::span<const std::string_view> extensions) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::string leaf = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (leaf.starts_with('.') || leaf == "build") {
                it.disable_recursion_pending();
            }
            continue;
        }
        std::string ext = it->path().extension().string();
        if (!it->is_regular_file(ec) ||
            std::find(extensions.begin(), extensions.end(), std::string_view(ext)) == extensions.end()) {
            continue;
        }
        out.push_back(fs::relative(it->path(), base, ec).generic_string());
    }
    if (ec) {
        return std::unexpected(std::format("cannot list {}: {}", dir.string(), ec.message()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::span<const std::uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string_view textOf(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace qiln

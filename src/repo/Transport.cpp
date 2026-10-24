#include "repo/Transport.hpp"

#include "support/Files.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <random>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace qiln {

namespace {

constexpr std::size_t kMaxResponse = 64u << 10;

// Every byte of a URL handed to curl is printable ASCII without quotes, spaces or backslashes.
bool safeUrl(std::string_view url) {
    for (char c : url) {
        if (c <= ' ' || c > '~' || c == '"' || c == '\'' || c == '\\' || c == '`') {
            return false;
        }
    }
    return true;
}

// The host of `scheme://host[:port]/...`; nullopt when the authority holds user information.
std::optional<std::string_view> hostOf(std::string_view url) {
    url.remove_prefix(url.find("://") + 3);
    std::string_view authority = url.substr(0, url.find('/'));
    if (authority.find('@') != std::string_view::npos || authority.empty()) {
        return std::nullopt;
    }
    if (authority.starts_with('[')) {
        return authority.substr(0, authority.find(']') + 1);
    }
    return authority.substr(0, authority.find(':'));
}

#ifdef _WIN32
// The C runtime splits the command line again, so each argument is quoted by its rules.
std::string quoteArgument(std::string_view arg) {
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    return out + "\"";
}
#endif

std::string_view curlReason(int status) {
    switch (status) {
    case 6:
        return " (host not found)";
    case 7:
        return " (connection refused)";
    case 28:
        return " (timed out)";
    case 35:
    case 60:
        return " (TLS handshake or certificate not trusted)";
    case 63:
        return " (larger than allowed)";
    default:
        return "";
    }
}

std::expected<int, std::string> runCurl(const std::vector<std::string>& args) {
#ifdef _WIN32
    std::vector<std::string> quoted;
    for (const std::string& a : args) {
        quoted.push_back(quoteArgument(a));
    }
    std::vector<const char*> argv;
    for (const std::string& a : quoted) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    intptr_t status = _spawnvp(_P_WAIT, "curl", argv.data());
    if (status < 0) {
        return std::unexpected("cannot run curl; it ships with Windows 10 and later");
    }
    return static_cast<int>(status);
#else
    std::vector<char*> argv;
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ) != 0) {
        return std::unexpected("cannot run curl; install it to use an https repository");
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected("lost track of curl");
        }
    }
    if (!WIFEXITED(status)) {
        return std::unexpected("curl was killed");
    }
    return WEXITSTATUS(status);
#endif
}

// Files under `~/.qiln/tmp` that live as long as one request.
class Scratch {
public:
    Scratch() {
        std::error_code ec;
        dir_ = qilnHome() / "tmp";
        fs::create_directories(dir_, ec);
        fs::permissions(dir_, fs::perms::owner_all, ec);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    ~Scratch() {
        std::error_code ec;
        for (const fs::path& p : files_) {
            fs::remove(p, ec);
        }
    }

    fs::path next(std::string_view stem) {
        std::random_device rd;
        files_.push_back(dir_ / std::format("{}-{:08x}{:08x}", stem, rd(), rd()));
        return files_.back();
    }

    // Created empty and made private before the secret is written into it.
    std::expected<fs::path, std::string> secret(std::string_view text) {
        fs::path p = next("auth");
        std::error_code ec;
        {
            std::ofstream create(p, std::ios::binary);
        }
        fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
        if (ec || !out) {
            return std::unexpected("cannot write a private file under " + dir_.string());
        }
        return p;
    }

private:
    fs::path dir_;
    std::vector<fs::path> files_;
};

// A double quoted string of a curl config file.
std::string curlQuoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

struct Response {
    int status = 0;
    std::string body; // the start of an error response, for the message
};

// One request through curl; credentials go in a private config file, never on its command line.
std::expected<Response, std::string> request(const Location& repo, const std::string& url, Scratch& scratch,
                                             const fs::path& output, const fs::path* upload, std::size_t limit,
                                             bool fresh) {
    std::string protocols = repo.insecure ? "=http,https" : "=https";
    fs::path headers = scratch.next("headers");
    std::vector<std::string> args = {"curl",           "--silent",   "--show-error", "--proto",
                                     protocols,        "--max-time", "300",          "--dump-header",
                                     headers.string(), "--output",   output.string()};
    if (upload != nullptr) {
        args.insert(args.end(), {"--upload-file", upload->string(), "--header", "Expect:"});
        if (fresh) {
            args.insert(args.end(), {"--header", "If-None-Match: *"});
        }
    } else {
        args.insert(args.end(), {"--location", "--proto-redir", "=https", "--max-filesize", std::to_string(limit)});
    }
    if (repo.credentials) {
        const Credentials& c = *repo.credentials;
        std::string config = c.token.empty() ? "user = " + curlQuoted(c.username + ":" + c.password) + "\n"
                                             : "header = " + curlQuoted("Authorization: Bearer " + c.token) + "\n";
        auto file = scratch.secret(config);
        if (!file) {
            return std::unexpected(file.error());
        }
        args.insert(args.end(), {"--config", file->string()});
    }
    args.push_back(url);
    auto status = runCurl(args);
    if (!status) {
        return std::unexpected(status.error());
    }
    if (*status != 0) {
        return std::unexpected(
            std::format("cannot reach {}: curl exit status {}{}", url, *status, curlReason(*status)));
    }
    Response r;
    std::string text = readText(headers).value_or("");
    // After redirects and `100 Continue` the last status line is the answer.
    std::size_t at = text.rfind("HTTP/");
    std::size_t space = at == std::string::npos ? at : text.find(' ', at);
    if (space != std::string::npos && space + 4 <= text.size()) {
        std::from_chars(text.data() + space + 1, text.data() + space + 4, r.status);
    }
    if (r.status < 100 || r.status > 599) {
        return std::unexpected(std::format("{} did not answer with an HTTP status", url));
    }
    if (r.status >= 300) {
        auto body = readLimited(output, kMaxResponse);
        std::string_view b = body && *body ? textOf(**body) : std::string_view{};
        for (char c : b.substr(0, 200)) {
            r.body += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? ' ' : c;
        }
    }
    return r;
}

std::string refusal(const Location& repo, const std::string& url, const Response& r) {
    std::string detail = r.body.empty() ? "" : ": " + r.body;
    if (r.status == 401 || r.status == 403) {
        return std::format("{} refused {} (HTTP {}){}; {} credentials for {} in ~/.qiln/credentials.toml{}",
                           repo.display, url, r.status, detail, repo.credentials ? "check the" : "add", repo.url,
                           " or $QILN_PUBLISH_USERNAME and $QILN_PUBLISH_PASSWORD (or $QILN_PUBLISH_TOKEN)");
    }
    return std::format("{} answered HTTP {} for {}{}", repo.display, r.status, url, detail);
}

} // namespace

std::expected<Location, std::string> locate(std::string_view spec, const fs::path& relativeTo,
                                            bool allowInsecureLocalhost) {
    Location loc;
    loc.display = std::string(spec);
    bool http = spec.starts_with("http://");
    if (spec.starts_with("https://") || http) {
        while (spec.ends_with('/')) {
            spec.remove_suffix(1);
        }
        auto host = hostOf(spec);
        if (!safeUrl(spec) || !host) {
            return std::unexpected(std::format("'{}' is not a usable repository URL", spec));
        }
        if (http) {
            bool loopback = *host == "127.0.0.1" || *host == "[::1]" || *host == "localhost";
            if (!loopback || !allowInsecureLocalhost) {
                return std::unexpected(
                    std::format("repository '{}': plain http is refused, use https://{}", spec,
                                loopback ? " (or --allow-insecure-localhost for a test server)" : ""));
            }
            loc.insecure = true;
        }
        loc.url = std::string(spec);
        loc.display = loc.url;
        return loc;
    }
    if (spec.find("://") != std::string_view::npos && !spec.starts_with("file://")) {
        return std::unexpected(
            std::format("repository '{}': only https://, file:// and directories are supported", spec));
    }
    if (spec.starts_with("file://")) {
        spec.remove_prefix(7);
#ifdef _WIN32
        if (spec.size() > 2 && spec[0] == '/' && spec[2] == ':') {
            spec.remove_prefix(1);
        }
#endif
    }
    if (spec.empty()) {
        return std::unexpected("a repository needs a directory or an https:// URL");
    }
    fs::path p(spec);
    loc.local = p.is_absolute() ? p : relativeTo / p;
    return loc;
}

std::expected<std::optional<std::vector<std::uint8_t>>, std::string>
download(const Location& repo, const std::string& path, std::size_t limit) {
    if (!repo.remote()) {
        return readLimited(repo.local / path, limit);
    }
    Scratch scratch;
    fs::path out = scratch.next("download");
    std::string url = repo.url + "/" + path;
    auto r = request(repo, url, scratch, out, nullptr, limit, false);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (r->status == 404 || r->status == 410) {
        return std::optional<std::vector<std::uint8_t>>();
    }
    if (r->status != 200) {
        return std::unexpected(refusal(repo, url, *r));
    }
    auto bytes = readLimited(out, limit);
    if (bytes && !*bytes) {
        return std::unexpected("curl reported success but wrote nothing for " + url);
    }
    return bytes;
}

std::expected<void, std::string> store(const Location& repo, const std::string& path,
                                       std::span<const std::uint8_t> bytes, bool fresh) {
    if (!repo.remote()) {
        std::error_code ec;
        if (fresh && fs::exists(repo.local / path, ec)) {
            return std::unexpected(std::format("{} already exists in {}", path, repo.display));
        }
        return writeAtomically(repo.local / path, bytes);
    }
    Scratch scratch;
    fs::path body = scratch.next("upload");
    if (auto w = writeAtomically(body, bytes); !w) {
        return w;
    }
    fs::path out = scratch.next("response");
    std::string url = repo.url + "/" + path;
    auto r = request(repo, url, scratch, out, &body, kMaxResponse, fresh);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (r->status == 412 || (fresh && r->status == 409)) {
        return std::unexpected(std::format("{} already exists in {} (HTTP {})", path, repo.display, r->status));
    }
    if (r->status < 200 || r->status >= 300) {
        return std::unexpected(refusal(repo, url, *r));
    }
    return {};
}

} // namespace qiln

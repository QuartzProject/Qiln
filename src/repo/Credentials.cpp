#include "repo/Credentials.hpp"

#include "support/Files.hpp"
#include "support/Toml.hpp"

#include <cstdlib>
#include <format>

namespace qiln {

namespace {

constexpr std::size_t kMaxFile = 1u << 20;
constexpr std::size_t kMaxValue = 4096;

bool printable(std::string_view s) {
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

std::expected<Credentials, std::string> checked(Credentials c, std::string_view where) {
    for (const std::string* v : {&c.username, &c.password, &c.token}) {
        if (v->size() > kMaxValue || !printable(*v)) {
            return std::unexpected(std::format("{}: credentials hold at most {} printable bytes", where, kMaxValue));
        }
    }
    bool basic = !c.username.empty() && !c.password.empty();
    bool bearer = !c.token.empty();
    if (basic == bearer || (!basic && (!c.username.empty() || !c.password.empty()))) {
        return std::unexpected(std::format("{}: give a username and a password, or a token", where));
    }
    if (c.username.find(':') != std::string::npos) {
        return std::unexpected(std::format("{}: a username cannot hold ':'", where));
    }
    return c;
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

} // namespace

std::expected<std::vector<CredentialEntry>, std::string> parseCredentials(std::string_view text) {
    auto sections = toml::parse(text, "credentials.toml", true);
    if (!sections) {
        return std::unexpected(sections.error());
    }
    std::vector<CredentialEntry> out;
    for (const toml::Section& s : *sections) {
        std::string where = std::format("credentials.toml:{}", s.line);
        if (s.name.empty()) {
            if (!s.entries.empty()) {
                return std::unexpected(
                    std::format("credentials.toml:{}: keys go inside a [repository] section", s.entries.front().line));
            }
            continue;
        }
        if (s.array || s.name.find('.') != std::string::npos) {
            return std::unexpected(std::format("{}: sections are plain names like [example]", where));
        }
        CredentialEntry e;
        e.name = s.name;
        for (const toml::Entry& kv : s.entries) {
            std::string* field = kv.key == "url"        ? &e.url
                                 : kv.key == "username" ? &e.credentials.username
                                 : kv.key == "password" ? &e.credentials.password
                                 : kv.key == "token"    ? &e.credentials.token
                                                        : nullptr;
            if (field == nullptr || kv.kind != toml::Entry::Kind::String) {
                return std::unexpected(std::format("credentials.toml:{}: '{}' is not url, username, password or token "
                                                   "with a string value",
                                                   kv.line, kv.key));
            }
            *field = kv.text;
        }
        if (!(e.url.starts_with("https://") || e.url.starts_with("http://")) || e.url.size() > 1024 ||
            !printable(e.url)) {
            return std::unexpected(std::format("{}: [{}] needs the url of the repository it is for", where, s.name));
        }
        while (e.url.ends_with('/')) {
            e.url.pop_back();
        }
        auto c = checked(std::move(e.credentials), where);
        if (!c) {
            return std::unexpected(c.error());
        }
        e.credentials = std::move(*c);
        out.push_back(std::move(e));
    }
    return out;
}

const CredentialEntry* matchCredentials(const std::vector<CredentialEntry>& entries, std::string_view url) {
    const CredentialEntry* best = nullptr;
    for (const CredentialEntry& e : entries) {
        bool prefix = url.starts_with(e.url) && (url.size() == e.url.size() || url[e.url.size()] == '/');
        if (prefix && (best == nullptr || e.url.size() > best->url.size())) {
            best = &e;
        }
    }
    return best;
}

std::expected<std::optional<Credentials>, std::string> publishCredentialsFromEnvironment() {
    Credentials c{env("QILN_PUBLISH_USERNAME"), env("QILN_PUBLISH_PASSWORD"), env("QILN_PUBLISH_TOKEN")};
    if (c.username.empty() && c.password.empty() && c.token.empty()) {
        return std::optional<Credentials>();
    }
    auto ok = checked(std::move(c), "QILN_PUBLISH_*");
    if (!ok) {
        return std::unexpected(ok.error());
    }
    return std::optional(std::move(*ok));
}

std::expected<std::optional<Credentials>, std::string> credentialsFor(std::string_view url) {
    fs::path file = qilnHome() / "credentials.toml";
    auto bytes = readLimited(file, kMaxFile);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (!*bytes) {
        return std::optional<Credentials>();
    }
    auto entries = parseCredentials(textOf(**bytes));
    if (!entries) {
        return std::unexpected(std::format("{}: {}", file.string(), entries.error()));
    }
    const CredentialEntry* e = matchCredentials(*entries, url);
    return e == nullptr ? std::optional<Credentials>() : std::optional(e->credentials);
}

} // namespace qiln

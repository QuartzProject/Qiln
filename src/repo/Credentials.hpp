#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// What authenticates Qiln to a repository: a user name and password (HTTP Basic) or a token (Bearer).
struct Credentials {
    std::string username;
    std::string password;
    std::string token;
};

/// One section of `credentials.toml`: the URL prefix it applies to and what to send there.
struct CredentialEntry {
    std::string name;
    std::string url;
    Credentials credentials;
};

/// Reads `~/.qiln/credentials.toml`; every section needs a `url` and a username with a password, or a token.
std::expected<std::vector<CredentialEntry>, std::string> parseCredentials(std::string_view text);

/// The entry with the longest `url` that is a prefix of `url` at a path boundary.
const CredentialEntry* matchCredentials(const std::vector<CredentialEntry>& entries, std::string_view url);

/// `$QILN_PUBLISH_USERNAME` and `_PASSWORD`, or `$QILN_PUBLISH_TOKEN`; nullopt when none is set.
std::expected<std::optional<Credentials>, std::string> publishCredentialsFromEnvironment();

/// The credentials for a remote repository URL from `credentials.toml` under the Qiln home, if any.
std::expected<std::optional<Credentials>, std::string> credentialsFor(std::string_view url);

} // namespace qiln

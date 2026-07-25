#pragma once

#include "repo/Meta.hpp"
#include "repo/Transport.hpp"

#include <expected>
#include <map>
#include <string>
#include <vector>

namespace qiln {

struct Locked;

/// The repositories a graph resolves from, searched in order; the first that lists a package serves all of it.
class Repositories {
public:
    explicit Repositories(std::vector<Location> locations) : locations_(std::move(locations)) {}

    /// Published versions, sorted; empty when no repository has the package.
    std::expected<std::vector<quartz::Version>, std::string> versions(const std::string& name);
    /// The verified `.qzmeta` of one version, from the cache under `~/.qiln` or the repository.
    std::expected<Meta, std::string> meta(const std::string& name, const quartz::Version& version);
    /// The artifact of a locked package, checked against the lock's checksum.
    std::expected<std::vector<std::uint8_t>, std::string> artifact(const Locked& p);

    [[nodiscard]] bool empty() const { return locations_.empty(); }

private:
    std::vector<Location> locations_;
    std::map<std::string, std::size_t> home_; // package -> index of the repository that lists it
    std::map<std::string, std::vector<quartz::Version>> versions_;
};

} // namespace qiln

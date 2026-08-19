#include "resolve/Resolver.hpp"

#include "repo/Meta.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace qiln {

namespace {

constexpr int kMaxRounds = 64;
// Each retry rules out one chosen version; the bound keeps a hopeless graph from searching forever.
constexpr int kMaxRetries = 256;

struct Want {
    Requirement req;
    std::string fromName; // the package whose chosen version asks, empty for a root requirement
    quartz::Version fromVersion{};
};

struct Failure {
    std::string message;
    std::vector<std::pair<std::string, quartz::Version>> culprits; // chosen versions whose ranges conflict
};

std::string describe(const std::vector<Want>& wants) {
    std::string out;
    for (const Want& w : wants) {
        out += std::format("{}{} (from {})", out.empty() ? "" : ", ", w.req.range, w.req.by);
    }
    return out;
}

std::string label(const Meta& m) { return std::format("{} {}", m.name, quartz::formatVersion(m.version)); }

class Resolver {
public:
    Resolver(const std::vector<Requirement>& roots, const PackageSource& source, const ResolveOptions& options)
        : roots_(roots), source_(source), options_(options) {}

    std::expected<LockGraph, std::string> run() {
        for (int retry = 0;; ++retry) {
            auto r = attempt();
            if (r) {
                return order(*r);
            }
            auto culprit = std::find_if(r.error().culprits.begin(), r.error().culprits.end(),
                                        [&](const auto& c) { return !excluded_.contains(c); });
            if (culprit == r.error().culprits.end() || retry == kMaxRetries) {
                return std::unexpected(r.error().message);
            }
            excluded_.insert(*culprit);
        }
    }

    std::expected<std::map<std::string, Meta>, Failure> attempt() {
        std::map<std::string, Meta> chosen;
        for (int round = 0; round < kMaxRounds; ++round) {
            std::map<std::string, std::vector<Want>> wanted;
            for (const Requirement& r : roots_) {
                wanted[r.name].push_back({r, "", {}});
            }
            for (const auto& [name, m] : chosen) {
                for (const Dependency& d : m.dependencies) {
                    wanted[d.name].push_back({{d.name, d.range, label(m)}, m.name, m.version});
                }
            }
            std::map<std::string, Meta> next;
            for (const auto& [name, wants] : wanted) {
                auto pick = choose(name, wants);
                if (!pick) {
                    return std::unexpected(pick.error());
                }
                next.emplace(name, std::move(*pick));
            }
            if (sameVersions(next, chosen)) {
                return chosen;
            }
            chosen = std::move(next);
        }
        return std::unexpected(Failure{"dependency resolution did not settle; the version ranges keep moving each "
                                       "other",
                                       {}});
    }

private:
    const std::vector<Requirement>& roots_;
    const PackageSource& source_;
    const ResolveOptions& options_;
    std::map<std::string, std::vector<quartz::Version>> versions_;
    std::map<std::pair<std::string, quartz::Version>, Meta> metas_;
    std::set<std::pair<std::string, quartz::Version>> excluded_;

    static bool sameVersions(const std::map<std::string, Meta>& a, const std::map<std::string, Meta>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
            return x.first == y.first && x.second.version == y.second.version;
        });
    }

    std::expected<const Meta*, std::string> meta(const std::string& name, const quartz::Version& v) {
        auto key = std::pair(name, v);
        if (auto it = metas_.find(key); it != metas_.end()) {
            return &it->second;
        }
        auto m = source_.meta(name, v);
        if (!m) {
            return std::unexpected(m.error());
        }
        return &metas_.emplace(key, std::move(*m)).first->second;
    }

    std::expected<const std::vector<quartz::Version>*, std::string> versions(const std::string& name) {
        if (auto it = versions_.find(name); it != versions_.end()) {
            return &it->second;
        }
        auto list = source_.versions(name);
        if (!list) {
            return std::unexpected(list.error());
        }
        std::sort(list->begin(), list->end());
        return &versions_.emplace(name, std::move(*list)).first->second;
    }

    // Trust on first use: the key in the previous lock, else the key of the oldest published version.
    std::expected<PublicKey, std::string> trustedKey(const std::string& name, const std::vector<quartz::Version>& list,
                                                     const Locked* locked) {
        if (locked != nullptr) {
            return locked->key;
        }
        auto oldest = meta(name, list.front());
        if (!oldest) {
            return std::unexpected(oldest.error());
        }
        return (*oldest)->key;
    }

    std::expected<Meta, Failure> choose(const std::string& name, const std::vector<Want>& wants) {
        auto fatal = [](std::string message) { return std::unexpected(Failure{std::move(message), {}}); };
        auto list = versions(name);
        if (!list) {
            return fatal(list.error());
        }
        if ((*list)->empty()) {
            return fatal(std::format("{} is not in any repository (needed by {})", name, wants.front().req.by));
        }
        std::vector<quartz::Version> fitting;
        for (const quartz::Version& v : **list) {
            bool ok = !excluded_.contains({name, v});
            for (const Want& w : wants) {
                auto s = quartz::satisfies(v, w.req.range);
                if (!s) {
                    return fatal(std::format("{} asks for {} '{}': {}", w.req.by, name, w.req.range, s.error()));
                }
                ok = ok && *s;
            }
            if (ok) {
                fitting.push_back(v);
            }
        }
        if (fitting.empty()) {
            std::string published;
            for (const quartz::Version& v : **list) {
                published += (published.empty() ? "" : ", ") + quartz::formatVersion(v);
            }
            Failure f{std::format("version conflict: no version of {} satisfies {}; published: {}", name,
                                  describe(wants), published),
                      {}};
            for (const Want& w : wants) {
                if (!w.fromName.empty()) {
                    f.culprits.emplace_back(w.fromName, w.fromVersion);
                }
            }
            return std::unexpected(std::move(f));
        }
        const Locked* locked = options_.previous != nullptr ? options_.previous->find(name) : nullptr;
        quartz::Version pick = fitting.back();
        if (locked != nullptr && !options_.update &&
            std::find(fitting.begin(), fitting.end(), locked->version) != fitting.end()) {
            pick = locked->version;
        }
        auto m = meta(name, pick);
        if (!m) {
            return fatal(m.error());
        }
        auto trusted = trustedKey(name, **list, locked);
        if (!trusted) {
            return fatal(trusted.error());
        }
        if (!options_.allowKeyChange && (*m)->key != *trusted) {
            return fatal(std::format(
                "{} is signed by key {}, not by {} which signed it before; if the publisher really changed keys, "
                "run again with --allow-key-change",
                label(**m), toHex((*m)->key), toHex(*trusted)));
        }
        if (locked != nullptr && locked->version == pick && (*m)->checksum != locked->checksum) {
            return fatal(std::format("{} changed since it was locked: published versions never change, so "
                                               "the repository is not serving what was published",
                                               label(**m)));
        }
        return **m;
    }

    std::expected<LockGraph, std::string> order(const std::map<std::string, Meta>& chosen) {
        LockGraph out;
        std::set<std::string> done;
        std::vector<std::string> path;
        auto visit = [&](auto& self, const std::string& name) -> std::expected<void, std::string> {
            if (done.contains(name)) {
                return {};
            }
            if (std::find(path.begin(), path.end(), name) != path.end()) {
                std::string cycle;
                for (const std::string& p : path) {
                    cycle += p + " -> ";
                }
                return std::unexpected("dependency cycle: " + cycle + name);
            }
            path.push_back(name);
            const Meta& m = chosen.at(name);
            for (const Dependency& d : m.dependencies) {
                if (auto r = self(self, d.name); !r) {
                    return r;
                }
            }
            path.pop_back();
            done.insert(name);
            out.packages.push_back(Locked{m.name, m.version, m.kind, m.dependencies, m.checksum, m.key});
            return {};
        };
        for (const auto& [name, m] : chosen) {
            if (auto r = visit(visit, name); !r) {
                return std::unexpected(r.error());
            }
        }
        return out;
    }
};

} // namespace

std::expected<LockGraph, std::string> resolve(const std::vector<Requirement>& roots, const PackageSource& source,
                                              const ResolveOptions& options) {
    return Resolver(roots, source, options).run();
}

} // namespace qiln

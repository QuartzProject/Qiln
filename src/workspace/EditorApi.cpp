#include "QilnApi.hpp"
#include "support/Files.hpp"
#include "workspace/Workspace.hpp"

#include <format>

namespace qiln {

namespace {

constexpr std::string_view kApiDir = ".qiln/api";

// Rewrites only what changed, so an editor watching the files does not analyze again for nothing.
std::expected<void, std::string> keep(const fs::path& path, std::string_view text) {
    if (auto old = readText(path); old && *old == text) {
        return {};
    }
    return writeAtomically(path, text);
}

// `../bundle/api/bundle.qz` or `org.example.bundle@1.0.0/api/bundle.qz` becomes `bundle.qz`.
std::string apiPath(const std::string& name) {
    std::size_t at = name.rfind("api/");
    return at == std::string::npos ? fs::path(name).filename().string() : name.substr(at + 4);
}

} // namespace

std::expected<void, std::string> writeEditorApi(Workspace& ws) {
    fs::path dir = ws.root / std::string(kApiDir);
    for (const auto& [file, text] : {std::pair{"qiln.qzi", api::kQilnQzi}, std::pair{"qiln.qz", api::kQilnQz},
                                     std::pair{"task.qzi", api::kTaskQzi}}) {
        if (auto w = keep(dir / file, text); !w) {
            return w;
        }
    }
    std::error_code ec;
    fs::path plugins = dir / "plugins";
    std::vector<fs::path> stale;
    for (auto it = fs::directory_iterator(plugins, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        stale.push_back(it->path());
    }
    if (ws.model.plugins.empty()) {
        fs::remove_all(plugins, ec);
        return {};
    }
    auto loaded = ws.plugins();
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    for (const LoadedPlugin& p : **loaded) {
        std::erase(stale, plugins / p.name);
        for (const quartz::SourceInput& in : p.api) {
            std::string rel = apiPath(in.name);
            if (!safeRelativePath(rel)) {
                continue;
            }
            if (auto w = keep(plugins / p.name / rel, in.text); !w) {
                return w;
            }
        }
    }
    for (const fs::path& s : stale) {
        fs::remove_all(s, ec);
    }
    return {};
}

} // namespace qiln

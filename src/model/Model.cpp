#include "model/Model.hpp"

namespace qiln {

const TaskSpec* ProjectModel::findTask(std::string_view task) const {
    for (const TaskSpec& t : tasks) {
        if (t.name == task) {
            return &t;
        }
    }
    return nullptr;
}

const ProjectModel* WorkspaceModel::findProject(std::string_view path) const {
    for (const ProjectModel& p : projects) {
        if (p.path == path) {
            return &p;
        }
    }
    return nullptr;
}

std::string projectPath(std::string_view dir) {
    std::string out = ":";
    for (char c : dir) {
        out += c == '/' ? ':' : c;
    }
    return out;
}

} // namespace qiln

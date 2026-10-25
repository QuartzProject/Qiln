#pragma once

#include "build/Tasks.hpp"
#include "workspace/Workspace.hpp"

#include <string>
#include <vector>

namespace qiln::cli {

struct Args {
    std::string command;
    std::vector<std::string> positional;
    fs::path dir = ".";
    Options options;
    RunOptions run;
    std::string kind = "application"; // init
    std::string group;                // init
    bool testDependency = false;      // add: testImplementation
};

int init(const Args& a);
int tasks(Workspace& ws, const ProjectModel* current);
int deps(Workspace& ws, const ProjectModel* current);
int keygen(const Args& a);
int add(const Args& a);
int remove(const Args& a);

} // namespace qiln::cli

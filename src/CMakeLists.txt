set(QILN_API_FILES ${PROJECT_SOURCE_DIR}/api/qiln.qzi ${PROJECT_SOURCE_DIR}/api/qiln.qz
    ${PROJECT_SOURCE_DIR}/api/task.qzi)
set(QILN_API_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/QilnApi.hpp)
add_custom_command(OUTPUT ${QILN_API_HEADER}
    COMMAND ${CMAKE_COMMAND} "-DINPUTS=${QILN_API_FILES}" -DOUTPUT=${QILN_API_HEADER}
        -P ${PROJECT_SOURCE_DIR}/cmake/QilnEmbed.cmake
    DEPENDS ${QILN_API_FILES} ${PROJECT_SOURCE_DIR}/cmake/QilnEmbed.cmake
    COMMENT "Embedding the build API"
    VERBATIM)

add_library(qiln_lib STATIC ${QILN_API_HEADER}
    support/Toml.cpp support/Files.cpp support/Hash.cpp
    repo/Artifact.cpp repo/Transport.cpp repo/Credentials.cpp repo/Metadata.cpp repo/Meta.cpp repo/Archive.cpp
    repo/Repositories.cpp
    resolve/Lockfile.cpp resolve/Resolver.cpp
    model/Model.cpp
    script/Script.cpp script/SettingsHost.cpp script/ProjectHost.cpp script/Edit.cpp
    repo/Publish.cpp
    workspace/Workspace.cpp workspace/Graphs.cpp workspace/Plugins.cpp workspace/ConfigCache.cpp
    workspace/EditorApi.cpp
    build/Console.cpp build/Compile.cpp build/Runner.cpp build/TaskHost.cpp build/Stamp.cpp build/Tasks.cpp
    build/Schedule.cpp build/Builtins.cpp
    cli/Init.cpp cli/Info.cpp cli/Add.cpp)
target_include_directories(qiln_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(qiln_lib PUBLIC qiln_options qiln_quartz)
target_compile_definitions(qiln_lib PUBLIC QILN_VERSION="${PROJECT_VERSION}"
    QILN_QUARTZ_VERSION="${QILN_QUARTZ_VERSION}")

add_executable(qiln cli/Main.cpp)
target_link_libraries(qiln PRIVATE qiln_lib)

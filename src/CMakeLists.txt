add_library(qiln_lib STATIC
    support/Toml.cpp support/Files.cpp support/Hash.cpp
    repo/Artifact.cpp repo/Transport.cpp repo/Metadata.cpp repo/Meta.cpp repo/Archive.cpp repo/Repositories.cpp
    resolve/Lockfile.cpp resolve/Resolver.cpp
    repo/Publish.cpp)
target_include_directories(qiln_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qiln_lib PUBLIC qiln_options qiln_quartz)

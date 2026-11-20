cmake_minimum_required(VERSION 3.28)
project(Qiln VERSION 0.1.0 LANGUAGES C CXX)
set(QILN_VERSION "${PROJECT_VERSION}" CACHE STRING "The version qiln reports; CI stamps each release")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(QilnOptions)
include(QilnQuartz)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

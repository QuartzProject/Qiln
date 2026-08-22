add_executable(qiln-tests unit/Main.cpp unit/ResolverTest.cpp unit/LockfileTest.cpp)
target_include_directories(qiln-tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(qiln-tests PRIVATE qiln_lib)
target_compile_definitions(qiln-tests PRIVATE QILN_TEST_WORK="${CMAKE_CURRENT_BINARY_DIR}/work")

add_test(NAME qiln.unit COMMAND qiln-tests)
# Plugin end to end and the multi-project build, on copies of examples/ inside the build directory.
add_test(NAME qiln.examples COMMAND ${CMAKE_COMMAND} -DQILN=$<TARGET_FILE:qiln>
    -DEXAMPLES=${PROJECT_SOURCE_DIR}/examples -DWORK=${CMAKE_CURRENT_BINARY_DIR}/examples
    -P ${CMAKE_CURRENT_SOURCE_DIR}/Examples.cmake)
set_tests_properties(qiln.unit qiln.examples PROPERTIES LABELS qiln
    ENVIRONMENT "QILN_HOME=${CMAKE_CURRENT_BINARY_DIR}/home")

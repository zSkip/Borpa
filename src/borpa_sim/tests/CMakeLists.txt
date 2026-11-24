if (NOT TARGET Catch2WithMain)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)
    if (MSVC)
        target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)
    endif()
endif()

# A stand-in for a solver: exits, fails, hangs, spins or starts a grandchild on request.
add_executable(borpa_sim_test_child sim_test_child.cpp)

add_executable(borpa_sim_tests
    test_formats.cpp
    test_process.cpp
    test_benchmarks.cpp
    test_geometry.hpp
)
target_link_libraries(borpa_sim_tests PRIVATE borpa_sim Catch2::Catch2WithMain)
target_include_directories(borpa_sim_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps_src)
target_compile_definitions(borpa_sim_tests PRIVATE
    SIM_TEST_CHILD="$<TARGET_FILE:borpa_sim_test_child>"
    SIM_TEST_WORK="${CMAKE_CURRENT_BINARY_DIR}/work")
if (MSVC)
    target_compile_options(borpa_sim_tests PRIVATE /utf-8)
endif()
add_dependencies(borpa_sim_tests borpa_sim_test_child)

include(CTest)
include(Catch)
catch_discover_tests(borpa_sim_tests)

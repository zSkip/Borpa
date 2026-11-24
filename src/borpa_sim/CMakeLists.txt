# Borpa's simulation pipeline: meshing, solver input and output, child processes, result files.
# No wxWidgets and no libslic3r, so it also builds on its own with its tests:
#   cmake -S src/borpa_sim -B <build> && cmake --build <build> --config Release
#   ctest --test-dir <build> -C Release
# The benchmark tests need the solvers: set BORPA_SOLVERS_DIR to a folder holding
# gmsh/gmsh.exe and calculix/ccx_i4_static.exe, or they are skipped.
cmake_minimum_required(VERSION 3.16)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(borpa_sim CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(BORPA_SIM_STANDALONE ON)
endif()

add_library(borpa_sim STATIC
    Base64.cpp
    Base64.hpp
    Calculix.cpp
    Calculix.hpp
    Gmsh.cpp
    Gmsh.hpp
    Mesh.cpp
    Mesh.hpp
    Paths.hpp
    Pipeline.cpp
    Pipeline.hpp
    Process.cpp
    Process.hpp
    Request.cpp
    Request.hpp
    ResultFile.cpp
    ResultFile.hpp
)
target_include_directories(borpa_sim
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../deps_src)
if (MSVC)
    target_compile_definitions(borpa_sim PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(borpa_sim PRIVATE /utf-8)
endif()
if (UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(borpa_sim PUBLIC Threads::Threads)
endif()

# Inside Borpa's build, tests/CMakeLists.txt adds the tests once its Catch2 target exists.
if (BORPA_SIM_STANDALONE)
    add_subdirectory(tests)
endif()

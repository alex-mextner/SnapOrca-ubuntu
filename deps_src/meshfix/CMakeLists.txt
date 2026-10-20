cmake_minimum_required(VERSION 3.13)
project(meshfix CXX)

# Vendored snapshot of https://github.com/alex-mextner/meshfix, see README.md. Built as a static library with the
# C ABI of include/meshfix.h; no dependencies (no CGAL, no libslic3r), so it needs none of the CGAL compile options.
add_library(meshfix STATIC
    include/meshfix.h
    src/capi.cpp
    src/cleanup.cpp
    src/components.cpp
    src/core.hpp
    src/edges.cpp
    src/holes.cpp
    src/io.cpp
    src/io.hpp
    src/nonmanifold.cpp
    src/repair.cpp
    src/selfint.cpp
)

target_include_directories(meshfix
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(meshfix PUBLIC cxx_std_17)
target_compile_definitions(meshfix PUBLIC MESHFIX_STATIC_DEFINE)
set_target_properties(meshfix PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

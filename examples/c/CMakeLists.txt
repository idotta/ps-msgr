# SPDX-License-Identifier: Apache-2.0
#
# Part of the libpsmsgr build with -DPSMSGR_BUILD_EXAMPLES=ON, or on its own
# against an installed library (the libpsmsgr-dev package, or any prefix):
#
#   cmake -S examples/c -B build/examples [-DCMAKE_PREFIX_PATH=<prefix>]
#   cmake --build build/examples
cmake_minimum_required(VERSION 3.25)
project(psmsgr_examples LANGUAGES C)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    add_compile_options(-Wall -Wextra)
    find_package(psmsgr 1 REQUIRED)
endif()

foreach(_example motor_writer motor_reader)
    add_executable(${_example} ${_example}.c)
    target_include_directories(${_example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(${_example} PRIVATE psmsgr::psmsgr)
    if(TARGET psmsgr_flags)
        target_link_libraries(${_example} PRIVATE psmsgr_flags)
    endif()
endforeach()

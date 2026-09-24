# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

# Tests call the public API through the shared library, exactly like the
# bindings do. With STATIC they link the static library instead, which also
# gives them the internal test hooks (src/internal.h); the .so exports none.
function(psmsgr_add_test name source)
    cmake_parse_arguments(ARG "STATIC" "" "" ${ARGN})
    add_executable(${name} ${source})
    if(ARG_STATIC)
        target_link_libraries(${name} PRIVATE psmsgr_static)
    else()
        target_link_libraries(${name} PRIVATE psmsgr)
    endif()
    target_link_libraries(${name} PRIVATE psmsgr_flags Threads::Threads)
    # layout.h: tests inspect and corrupt channel files from the outside.
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit TIMEOUT 300)
endfunction()

psmsgr_add_test(test_common test_common.c)
psmsgr_add_test(test_headers test_headers.c)
psmsgr_add_test(test_state test_state.c)
psmsgr_add_test(test_state_hooks test_state_hooks.c STATIC)

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    add_executable(test_headers_cxx test_headers_cxx.cpp)
    target_link_libraries(test_headers_cxx PRIVATE psmsgr)
    target_compile_options(test_headers_cxx PRIVATE -Wall -Wextra -Wpedantic
                           $<$<BOOL:${PSMSGR_WERROR}>:-Werror>)
    add_test(NAME test_headers_cxx COMMAND test_headers_cxx)
    set_tests_properties(test_headers_cxx PROPERTIES LABELS unit)
endif()

add_test(NAME abi_check
    COMMAND ${CMAKE_COMMAND}
        -DLIB=$<TARGET_FILE:psmsgr>
        -DNM=${CMAKE_NM}
        -DREADELF=${CMAKE_READELF}
        -P ${PROJECT_SOURCE_DIR}/cmake/CheckAbi.cmake)
set_tests_properties(abi_check PROPERTIES LABELS abi)

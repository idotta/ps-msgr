# SPDX-License-Identifier: Apache-2.0

# Tests call only the public API, through the shared library, exactly like
# the bindings do.
function(psmsgr_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE psmsgr psmsgr_flags)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

psmsgr_add_test(test_common test_common.c)
psmsgr_add_test(test_headers test_headers.c)

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

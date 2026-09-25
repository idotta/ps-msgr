# SPDX-License-Identifier: Apache-2.0
#
# ctest script: compares the shared library with the ABI baseline of the
# latest release of its major (abi/, spec/build-and-test.md) using abidiff.
# Added functions pass; any other change left after the suppressions fails.
# Skipped when there is no baseline for this major and architecture (a new
# major before its first release) or no abidiff.
#
# No --headers-dir here: abidiff would then ignore changes to types from
# system headers, such as a parameter going from int32_t to int64_t. The
# suppressions cover the opaque handles instead.
#
# cmake -DLIB=<libpsmsgr.so> -DBASELINE=<.abi> -DSUPPRESSIONS=<.suppr>
#       -DABIDIFF=<abidiff> -P CheckAbiCompat.cmake

cmake_minimum_required(VERSION 3.25)

foreach(_var LIB BASELINE SUPPRESSIONS)
    if(NOT ${_var})
        message(FATAL_ERROR "CheckAbiCompat: ${_var} not set")
    endif()
endforeach()

if(NOT EXISTS "${BASELINE}")
    message("abi_compat skipped: no baseline ${BASELINE}")
    return()
endif()
if(NOT ABIDIFF)
    message("abi_compat skipped: abidiff not found (abigail-tools)")
    return()
endif()

execute_process(
    COMMAND "${ABIDIFF}" --no-added-syms --suppressions "${SUPPRESSIONS}"
            "${BASELINE}" "${LIB}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "ABI of ${LIB} is incompatible with ${BASELINE} "
                        "(abidiff exit ${_rc}):\n${_out}")
endif()
message(STATUS "ABI compatible with ${BASELINE}")

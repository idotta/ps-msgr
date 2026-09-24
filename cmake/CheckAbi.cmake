# SPDX-License-Identifier: Apache-2.0
#
# ctest script: checks the shared library's ABI surface.
#   - every exported symbol is psmsgr_* (version script works)
#   - no libatomic: no __atomic_* references and no libatomic in NEEDED.
#     Either would mean a non-lock-free (e.g. 64-bit) atomic slipped in,
#     which on ARMv7 is a library call instead of an ldrex/strex loop.
#
# cmake -DLIB=<libpsmsgr.so> -DNM=<nm> -DREADELF=<readelf> -P CheckAbi.cmake

foreach(_var LIB NM READELF)
    if(NOT ${_var})
        message(FATAL_ERROR "CheckAbi: ${_var} not set")
    endif()
endforeach()

execute_process(COMMAND "${NM}" -D --defined-only "${LIB}"
    OUTPUT_VARIABLE _defined COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${NM}" -D --undefined-only "${LIB}"
    OUTPUT_VARIABLE _undefined COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${READELF}" -d "${LIB}"
    OUTPUT_VARIABLE _dynamic COMMAND_ERROR_IS_FATAL ANY)

set(_errors "")
string(REPLACE "\n" ";" _defined "${_defined}")
foreach(_line IN LISTS _defined)
    # "<addr> <type> <name>[@@version]"; skip the version-node definition.
    if(_line MATCHES "^[0-9a-fA-F]+ [A-Za-z] ([^@ ]+)")
        set(_sym "${CMAKE_MATCH_1}")
        if(NOT _sym MATCHES "^psmsgr_" AND NOT _sym MATCHES "^PSMSGR_[0-9]+$")
            string(APPEND _errors "  exported non-API symbol: ${_sym}\n")
        endif()
    endif()
endforeach()
if(_undefined MATCHES "__atomic_")
    string(APPEND _errors "  references __atomic_* (libatomic call)\n")
endif()
if(_dynamic MATCHES "libatomic")
    string(APPEND _errors "  links libatomic\n")
endif()

if(_errors)
    message(FATAL_ERROR "ABI check failed for ${LIB}:\n${_errors}")
endif()
message(STATUS "ABI check passed for ${LIB}")

# SPDX-License-Identifier: Apache-2.0
#
# ctest script: checks the shared library's ABI surface.
#   - the exported symbols, with their version nodes, are exactly the ones
#     listed in the version script, and the PSMSGR_API declarations in the
#     public headers are exactly those symbols
#   - version nodes are PSMSGR_<major>[.<minor>], the minor is at most the
#     library's, and the SONAME is libpsmsgr.so.<major>
#   - no libatomic: no __atomic_* references and no libatomic in NEEDED.
#     Either would mean a non-lock-free (e.g. 64-bit) atomic slipped in,
#     which on ARMv7 is a library call instead of an ldrex/strex loop.
#
# cmake -DLIB=<libpsmsgr.so> -DMAP=<libpsmsgr.map> -DINCLUDE_DIR=<include>
#       -DVERSION=<x.y.z> -DNM=<nm> -DREADELF=<readelf> -P CheckAbi.cmake

cmake_minimum_required(VERSION 3.25)

foreach(_var LIB MAP INCLUDE_DIR VERSION NM READELF)
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
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _ "${VERSION}")
set(_major "${CMAKE_MATCH_1}")
set(_minor "${CMAKE_MATCH_2}")

# Version script: "NODE { global: sym; ... local: *; } [PARENT];"
file(READ "${MAP}" _map)
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _map "${_map}")
string(REGEX REPLACE "#[^\n]*" "" _map "${_map}")
# ";" separates CMake list elements.
string(REPLACE ";" "," _map "${_map}")
string(REGEX MATCHALL "[A-Za-z0-9_.]+[ \t\r\n]*{[^}]*}[^,]*," _nodes "${_map}")
if(NOT _nodes)
    string(APPEND _errors "  no version node in ${MAP}\n")
endif()
set(_node_names "")
set(_expected "")
set(_map_syms "")
foreach(_node IN LISTS _nodes)
    string(REGEX MATCH "^([A-Za-z0-9_.]+)[ \t\r\n]*{([^}]*)}[ \t\r\n]*([A-Za-z0-9_.]*)"
           _ "${_node}")
    set(_name "${CMAKE_MATCH_1}")
    set(_body "${CMAKE_MATCH_2}")
    set(_parent "${CMAKE_MATCH_3}")
    list(APPEND _node_names "${_name}")
    if(NOT _name MATCHES "^PSMSGR_([0-9]+)(\\.([0-9]+))?$")
        string(APPEND _errors "  node name not PSMSGR_<major>[.<minor>]: ${_name}\n")
        continue()
    endif()
    set(_node_major "${CMAKE_MATCH_1}")
    set(_node_minor "${CMAKE_MATCH_3}") # empty for the base node
    if(NOT _node_major EQUAL _major)
        string(APPEND _errors "  node ${_name} does not match major version ${_major}\n")
    endif()
    if(_node_minor STREQUAL "")
        if(_parent)
            string(APPEND _errors "  base node ${_name} has a parent\n")
        endif()
    else()
        if(NOT _parent)
            string(APPEND _errors "  node ${_name} has no parent\n")
        endif()
        if(_node_minor GREATER _minor)
            string(APPEND _errors "  node ${_name} is newer than version ${VERSION}\n")
        endif()
    endif()
    string(REGEX REPLACE "local:.*" "" _global "${_body}")
    string(REPLACE "global:" "" _global "${_global}")
    if(_global MATCHES "[*?[]")
        string(APPEND _errors "  wildcard in node ${_name}\n")
    endif()
    string(REGEX MATCHALL "[A-Za-z0-9_]+" _syms "${_global}")
    foreach(_sym IN LISTS _syms)
        if(_sym IN_LIST _map_syms)
            string(APPEND _errors "  listed twice in ${MAP}: ${_sym}\n")
        endif()
        list(APPEND _map_syms "${_sym}")
        list(APPEND _expected "${_sym}@@${_name}")
    endforeach()
endforeach()

# "<addr> <type> <name>[@@version]"; a version node is defined as an A symbol.
set(_exported "")
string(REPLACE "\n" ";" _defined "${_defined}")
foreach(_line IN LISTS _defined)
    if(_line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) ([^ ]+)$")
        set(_sym "${CMAKE_MATCH_2}")
        if(CMAKE_MATCH_1 STREQUAL "A" AND _sym IN_LIST _node_names)
            continue()
        endif()
        list(APPEND _exported "${_sym}")
        if(NOT _sym IN_LIST _expected)
            string(APPEND _errors "  exported but not in ${MAP}: ${_sym}\n")
        endif()
    endif()
endforeach()
foreach(_sym IN LISTS _expected)
    if(NOT _sym IN_LIST _exported)
        string(APPEND _errors "  in ${MAP} but not exported: ${_sym}\n")
    endif()
endforeach()

# Declarations are one line up to the "(": PSMSGR_API <type> psmsgr_x(...
file(GLOB_RECURSE _headers "${INCLUDE_DIR}/*.h")
set(_declared "")
foreach(_header IN LISTS _headers)
    file(READ "${_header}" _text)
    string(REGEX MATCHALL "PSMSGR_API[^;(\n]*[ *]psmsgr_[A-Za-z0-9_]+[ \t]*\\("
           _decls "${_text}")
    foreach(_decl IN LISTS _decls)
        string(REGEX MATCH "psmsgr_[A-Za-z0-9_]+[ \t]*\\($" _sym "${_decl}")
        string(REGEX REPLACE "[ \t]*\\($" "" _sym "${_sym}")
        list(APPEND _declared "${_sym}")
        if(NOT _sym IN_LIST _map_syms)
            string(APPEND _errors "  declared PSMSGR_API but not in ${MAP}: ${_sym}\n")
        endif()
    endforeach()
endforeach()
foreach(_sym IN LISTS _map_syms)
    if(NOT _sym IN_LIST _declared)
        string(APPEND _errors "  in ${MAP} but not declared PSMSGR_API: ${_sym}\n")
    endif()
endforeach()

if(NOT _dynamic MATCHES "Library soname: \\[libpsmsgr\\.so\\.${_major}\\]")
    string(REGEX MATCH "Library soname: \\[[^]]*\\]" _soname "${_dynamic}")
    string(APPEND _errors "  SONAME is not libpsmsgr.so.${_major}: ${_soname}\n")
endif()
if(_undefined MATCHES "__atomic_")
    string(APPEND _errors "  references __atomic_* (libatomic call)\n")
endif()
if(_dynamic MATCHES "libatomic")
    string(APPEND _errors "  links libatomic\n")
endif()

if(_errors)
    message(FATAL_ERROR "ABI check failed for ${LIB}:\n${_errors}")
endif()
list(LENGTH _expected _count)
message(STATUS "ABI check passed for ${LIB} (${_count} symbols)")

# SPDX-License-Identifier: Apache-2.0

# Public API through the shared library, plus layout.h for the raw view.
add_executable(psmsgr-dump psmsgr-dump.c)
target_link_libraries(psmsgr-dump PRIVATE psmsgr psmsgr_flags)
target_include_directories(psmsgr-dump PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(psmsgr-dump PRIVATE _GNU_SOURCE)
install(TARGETS psmsgr-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)

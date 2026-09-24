# SPDX-License-Identifier: Apache-2.0

# Built, never run by CTest: only numbers from the board mean anything.
add_executable(psmsgr-bench psmsgr-bench.c)
target_link_libraries(psmsgr-bench PRIVATE psmsgr psmsgr_flags)
target_compile_definitions(psmsgr-bench PRIVATE _GNU_SOURCE)
install(TARGETS psmsgr-bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)

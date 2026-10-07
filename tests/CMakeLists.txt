# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)

# Compile small production-body fixtures with fake guest threads and graphics
# objects. These tests do not require game assets or a graphics device. The
# cache-storage fixtures use POSIX stdio/file APIs, so register them on Linux
# with GNU-style compiler drivers only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT CMAKE_CROSSCOMPILING
   AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(REXGLUE_REGRESSION_CHECKS)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
            list(APPEND REXGLUE_REGRESSION_CHECKS simde_stream_pi)
        endif()
        foreach(check IN LISTS REXGLUE_REGRESSION_CHECKS)
            add_test(NAME regression_${check}
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/regression/test_${check}.py
                    ${CMAKE_CURRENT_BINARY_DIR}/regression/${check}
                    --compiler ${CMAKE_CXX_COMPILER})
            set_tests_properties(regression_${check} PROPERTIES
                TIMEOUT 60 LABELS "regression;asset_free")
            if(check STREQUAL "timer_wait")
                set_tests_properties(regression_${check} PROPERTIES TIMEOUT 100)
            endif()
        endforeach()
    else()
        message(STATUS "Python3 unavailable: production-body regression fixtures are disabled")
    endif()
endif()

# Patch preparation uses local Git objects and does not execute target binaries.
if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_package(Git QUIET)
    if(Python3_Interpreter_FOUND AND Git_FOUND
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/simde/.git")
        add_test(NAME regression_prepare_simde
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/regression/test_prepare_simde.py
                ${CMAKE_CURRENT_BINARY_DIR}/regression/prepare_simde)
        set_tests_properties(regression_prepare_simde PROPERTIES
            TIMEOUT 60 LABELS "regression;asset_free")
    endif()
endif()

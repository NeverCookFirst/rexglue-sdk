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
        set(REXGLUE_REGRESSION_CHECKS graph_registration gapfill_cleanup)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
            # This producer uses the real decoder and writer; its test compiles
            # and executes unmodified generated C++ with the actual SDK headers.
            add_executable(regression_interior_entries regression/emit_interior_entries.cpp)
            target_link_libraries(regression_interior_entries PRIVATE rexcodegen)
            add_test(NAME regression_interior_entries
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/regression/test_interior_entries.py
                    ${CMAKE_CURRENT_BINARY_DIR}/regression/interior_entries
                    --compiler ${CMAKE_CXX_COMPILER}
                    --producer $<TARGET_FILE:regression_interior_entries>)
            set_tests_properties(regression_interior_entries PROPERTIES
                TIMEOUT 60 LABELS "regression;asset_free")
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

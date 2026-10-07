# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)

# Timer publication/lifecycle fixtures compile the actual production queue and
# Disruptor with runtime-independent assert/thread-name facilities only.
# POSIX runner/process limits require Linux and GNU-style compiler drivers.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        foreach(check IN ITEMS timer_wait timer_lifecycle)
            add_test(NAME regression_${check}
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/regression/test_${check}.py
                    ${CMAKE_CURRENT_BINARY_DIR}/regression/${check}
                    --compiler ${CMAKE_CXX_COMPILER})
            set_tests_properties(regression_${check} PROPERTIES
                TIMEOUT 100 LABELS "regression;asset_free")
        endforeach()
    else()
        message(STATUS "Python3 unavailable: timer regression fixtures are disabled")
    endif()
endif()

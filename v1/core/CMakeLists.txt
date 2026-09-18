# The scoring core: C++17, no engine types, no dependencies.
#
# Section 12's setup note is "build the core standalone first", and this is
# what makes that possible. Nothing here knows Unreal exists, which is also
# what makes the tests run in CI without an engine installation (16.1), makes
# replay determinism achievable (9.3), and makes a Unity or Godot binding a
# matter of writing a binding.

cmake_minimum_required(VERSION 3.16)
project(aib_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(aib_core STATIC
    src/curve.cpp
    src/scoring.cpp
    src/selection.cpp
    src/compiled.cpp
    src/record.cpp
)
target_include_directories(aib_core PUBLIC include)

if(MSVC)
    target_compile_options(aib_core PRIVATE /W4 /WX)
else()
    target_compile_options(aib_core PRIVATE -Wall -Wextra -Werror)
endif()

# ------------------------------------------------------------------- tests

enable_testing()

set(AIB_TESTS curve scoring selection compiled record crosscheck)

foreach(name ${AIB_TESTS})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${name}.cpp)
        add_executable(test_${name} tests/test_${name}.cpp)
        target_link_libraries(test_${name} PRIVATE aib_core)
        target_include_directories(test_${name} PRIVATE tests)
        if(MSVC)
            target_compile_options(test_${name} PRIVATE /W4 /WX)
        else()
            target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror)
        endif()
        add_test(NAME ${name} COMMAND test_${name})
    endif()
endforeach()

# --------------------------------------------------------------- benchmark

# Section 11.3 says to measure in-engine with Unreal Insights, and that is
# right for shipping numbers. This is the other half: a host-side benchmark
# that tells you whether the core itself is anywhere near the budget before
# the engine is ever involved.
add_executable(aib_bench tools/bench.cpp)
target_link_libraries(aib_bench PRIVATE aib_core)
target_include_directories(aib_bench PRIVATE tests)

# ------------------------------------------------------------------- probe
#
# Exposes the core's decisions on a CLI so the editor's TypeScript scorer can
# be checked against it (editor/tests/crosscheck.test.ts). The C++ side is
# normative -- it is what ships -- so this is the reference the other
# implementation is measured against.
add_executable(aib_probe tools/probe.cpp)
target_link_libraries(aib_probe PRIVATE aib_core)

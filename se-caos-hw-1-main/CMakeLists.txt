cmake_minimum_required(VERSION 3.20)

project(crossroad LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(crossroad
    src/main.c
    src/config.c
    src/io.c
    src/simulation.c
)

target_compile_definitions(crossroad PRIVATE
    _POSIX_C_SOURCE=200809L
)

target_compile_options(crossroad PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

option(ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

if(ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(crossroad PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(crossroad PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)

    if(Python3_FOUND)
        add_test(NAME scenarios
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scenarios.py
                $<TARGET_FILE:crossroad>
        )
        set_tests_properties(scenarios PROPERTIES TIMEOUT 60)
    endif()
endif()

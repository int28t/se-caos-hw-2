cmake_minimum_required(VERSION 3.20)

project(crossroad LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(SIMULATION_SOURCES
    src/config.c
    src/io.c
    src/simulation.c
    src/simulation_state.c
    src/simulation_time.c
    src/participants.c
)

add_executable(crossroad src/main.c ${SIMULATION_SOURCES})

include(CTest)

set(CROSSROAD_TARGETS crossroad)

if(BUILD_TESTING)
    add_executable(state_test
        tests/test_state.c
        src/simulation_state.c
    )
    target_include_directories(state_test PRIVATE src)
    list(APPEND CROSSROAD_TARGETS state_test)
    add_test(NAME state COMMAND state_test)
    set_tests_properties(state PROPERTIES TIMEOUT 15)

    add_executable(participants_test tests/test_participants.c ${SIMULATION_SOURCES})
    target_include_directories(participants_test PRIVATE src)
    list(APPEND CROSSROAD_TARGETS participants_test)
    add_test(NAME participants COMMAND participants_test)
    set_tests_properties(participants PROPERTIES TIMEOUT 15)

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

option(ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

foreach(target IN LISTS CROSSROAD_TARGETS)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    target_compile_definitions(${target} PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )

    if(ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

cmake_minimum_required(VERSION 3.1)

project(rax C)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
    set(RAX_IS_STANDALONE TRUE)
endif()

option(RAX_BUILD_TESTS "Build rax tests" ${RAX_IS_STANDALONE})

if(RAX_BUILD_TESTS)
    add_executable(rax-test
        rax-test.c rax.c rc4rand.c crc16.c
    )
    add_executable(rax-oom-test
        rax-oom-test.c rax.c rc4rand.c crc16.c
    )
endif()

add_library(${PROJECT_NAME}
    rax.c
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        .
)

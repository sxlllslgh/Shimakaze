cmake_minimum_required(VERSION 3.24)

project(Shimakaze
    VERSION 0.1.0
    DESCRIPTION "A lightweight C++ KCP tunnel"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHIMAKAZE_STATIC_LINK
    "Build fully static Linux executables (intended for portable musl releases)"
    OFF
)

if(SHIMAKAZE_STATIC_LINK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "SHIMAKAZE_STATIC_LINK is supported only on Linux")
    endif()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL
        "Build static dependency libraries when SHIMAKAZE_STATIC_LINK is enabled"
        FORCE
    )
endif()

include(CTest)
include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

find_package(Threads REQUIRED)

add_library(shimakaze_core
    src/shimakaze/client.cpp
    src/shimakaze/config.cpp
    src/shimakaze/crypto.cpp
    src/shimakaze/diagnostics.cpp
    src/shimakaze/fec.cpp
    src/shimakaze/kcp_transport.cpp
    src/shimakaze/logger.cpp
    src/shimakaze/qpp.cpp
    src/shimakaze/server.cpp
    src/shimakaze/smux.cpp
    src/shimakaze/snappy_stream.cpp
    src/shimakaze/stats.cpp
    src/shimakaze/utils.cpp
)

target_include_directories(shimakaze_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_compile_features(shimakaze_core PUBLIC cxx_std_23)

target_link_libraries(shimakaze_core
    PUBLIC
        kcp::kcp
        Boost::asio
        Boost::json
        CryptoPP::cryptopp
        snappy
        Threads::Threads
)

if(WIN32)
    target_compile_definitions(shimakaze_core
        PUBLIC
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0601
    )
    target_link_libraries(shimakaze_core PUBLIC ws2_32 mswsock)
endif()

if(MSVC)
    target_compile_options(shimakaze_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(shimakaze_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(shimakaze_client src/client_main.cpp)
target_link_libraries(shimakaze_client PRIVATE shimakaze_core)
set_target_properties(shimakaze_client PROPERTIES OUTPUT_NAME client)

add_executable(shimakaze_server src/server_main.cpp)
target_link_libraries(shimakaze_server PRIVATE shimakaze_core)
set_target_properties(shimakaze_server PROPERTIES OUTPUT_NAME server)

if(SHIMAKAZE_STATIC_LINK)
    target_link_options(shimakaze_client PRIVATE -static)
    target_link_options(shimakaze_server PRIVATE -static)
endif()

if(BUILD_TESTING)
    add_executable(shimakaze_config_tests tests/config_tests.cpp)
    target_link_libraries(shimakaze_config_tests PRIVATE shimakaze_core)
    add_test(NAME shimakaze_config_tests COMMAND shimakaze_config_tests)
endif()

install(TARGETS shimakaze_client shimakaze_server
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

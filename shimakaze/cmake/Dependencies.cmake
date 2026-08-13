include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

set(SHIMAKAZE_KCP_GIT_REPOSITORY "https://github.com/skywind3000/kcp.git"
    CACHE STRING "KCP git repository")
set(SHIMAKAZE_KCP_GIT_TAG "master"
    CACHE STRING "KCP git ref. The upstream project currently publishes master only.")
set(SHIMAKAZE_BOOST_GIT_REPOSITORY "https://github.com/boostorg/boost.git"
    CACHE STRING "Boost git repository")
set(SHIMAKAZE_BOOST_GIT_TAG "boost-1.91.0-1"
    CACHE STRING "Boost release git tag")
set(SHIMAKAZE_SNAPPY_GIT_REPOSITORY "https://github.com/google/snappy.git"
    CACHE STRING "Snappy git repository")
set(SHIMAKAZE_SNAPPY_GIT_TAG "1.2.2"
    CACHE STRING "Snappy release git tag")
set(SHIMAKAZE_CRYPTOPP_GIT_REPOSITORY "https://github.com/weidai11/cryptopp.git"
    CACHE STRING "Crypto++ git repository")
set(SHIMAKAZE_CRYPTOPP_GIT_TAG "CRYPTOPP_8_9_0"
    CACHE STRING "Crypto++ release git tag")

set(SHIMAKAZE_KCP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../kcp"
    CACHE PATH "Optional local KCP source tree")
option(SHIMAKAZE_PREFER_LOCAL_KCP "Use sibling ../kcp when it exists" ON)

set(_SHIMAKAZE_BUILD_TESTING "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency tests" FORCE)

if(SHIMAKAZE_PREFER_LOCAL_KCP AND EXISTS "${SHIMAKAZE_KCP_SOURCE_DIR}/ikcp.c")
    set(_SHIMAKAZE_EFFECTIVE_KCP_SOURCE_DIR "${SHIMAKAZE_KCP_SOURCE_DIR}")
else()
    FetchContent_Declare(kcp
        GIT_REPOSITORY "${SHIMAKAZE_KCP_GIT_REPOSITORY}"
        GIT_TAG "${SHIMAKAZE_KCP_GIT_TAG}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        SOURCE_SUBDIR cmake-does-not-exist
    )
    FetchContent_MakeAvailable(kcp)
    set(_SHIMAKAZE_EFFECTIVE_KCP_SOURCE_DIR "${kcp_SOURCE_DIR}")
endif()

add_library(kcp "${_SHIMAKAZE_EFFECTIVE_KCP_SOURCE_DIR}/ikcp.c")
target_include_directories(kcp PUBLIC "$<BUILD_INTERFACE:${_SHIMAKAZE_EFFECTIVE_KCP_SOURCE_DIR}>")
add_library(kcp::kcp ALIAS kcp)

set(BUILD_TESTING "${_SHIMAKAZE_BUILD_TESTING}" CACHE BOOL "Build project tests" FORCE)

set(_SHIMAKAZE_BOOST_SUBMODULES
    libs/algorithm
    libs/align
    libs/array
    libs/asio
    libs/assert
    libs/bind
    libs/compat
    libs/concept_check
    libs/config
    libs/container
    libs/container_hash
    libs/conversion
    libs/core
    libs/date_time
    libs/describe
    libs/detail
    libs/endian
    libs/exception
    libs/function
    libs/function_types
    libs/functional
    libs/fusion
    libs/headers
    libs/integer
    libs/intrusive
    libs/io
    libs/iterator
    libs/json
    libs/lambda
    libs/lexical_cast
    libs/move
    libs/mp11
    libs/mpl
    libs/numeric/conversion
    libs/optional
    libs/pool
    libs/predef
    libs/preprocessor
    libs/range
    libs/regex
    libs/smart_ptr
    libs/static_assert
    libs/system
    libs/throw_exception
    libs/tokenizer
    libs/tuple
    libs/typeof
    libs/type_traits
    libs/unordered
    libs/utility
    libs/variant2
    libs/winapi
)
FetchContent_Declare(Boost
    GIT_REPOSITORY "${SHIMAKAZE_BOOST_GIT_REPOSITORY}"
    GIT_TAG "${SHIMAKAZE_BOOST_GIT_TAG}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_SUBMODULES ${_SHIMAKAZE_BOOST_SUBMODULES}
    GIT_SUBMODULES_RECURSE FALSE
    SOURCE_SUBDIR cmake-does-not-exist
)
FetchContent_MakeAvailable(Boost)

if(DEFINED Boost_SOURCE_DIR)
    set(_SHIMAKAZE_BOOST_SOURCE_DIR "${Boost_SOURCE_DIR}")
elseif(DEFINED boost_SOURCE_DIR)
    set(_SHIMAKAZE_BOOST_SOURCE_DIR "${boost_SOURCE_DIR}")
else()
    message(FATAL_ERROR "Boost source directory was not populated")
endif()

file(GLOB _SHIMAKAZE_BOOST_INCLUDE_DIRS CONFIGURE_DEPENDS
    "${_SHIMAKAZE_BOOST_SOURCE_DIR}/libs/*/include"
    "${_SHIMAKAZE_BOOST_SOURCE_DIR}/libs/*/*/include"
)

if(NOT EXISTS "${_SHIMAKAZE_BOOST_SOURCE_DIR}/libs/asio/include/boost/asio.hpp")
    message(FATAL_ERROR "Boost.Asio headers were not found")
endif()
if(NOT EXISTS "${_SHIMAKAZE_BOOST_SOURCE_DIR}/libs/json/src/src.cpp")
    message(FATAL_ERROR "Boost.JSON sources were not found")
endif()

add_library(boost_headers INTERFACE)
target_include_directories(boost_headers
    INTERFACE
        ${_SHIMAKAZE_BOOST_INCLUDE_DIRS}
)
target_compile_definitions(boost_headers
    INTERFACE
        BOOST_ALL_NO_LIB
        BOOST_ERROR_CODE_HEADER_ONLY
        BOOST_JSON_NO_LIB
)

add_library(Boost::headers ALIAS boost_headers)
add_library(Boost::asio ALIAS boost_headers)

add_library(boost_json STATIC
    "${_SHIMAKAZE_BOOST_SOURCE_DIR}/libs/json/src/src.cpp"
)
target_link_libraries(boost_json PUBLIC boost_headers)
target_compile_features(boost_json PUBLIC cxx_std_17)
target_compile_definitions(boost_json
    PUBLIC
        BOOST_ALL_NO_LIB
        BOOST_ERROR_CODE_HEADER_ONLY
        BOOST_JSON_NO_LIB
)
add_library(Boost::json ALIAS boost_json)

FetchContent_Declare(cryptopp
    GIT_REPOSITORY "${SHIMAKAZE_CRYPTOPP_GIT_REPOSITORY}"
    GIT_TAG "${SHIMAKAZE_CRYPTOPP_GIT_TAG}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR cmake-does-not-exist
)
FetchContent_MakeAvailable(cryptopp)

if(NOT TARGET CryptoPP::cryptopp)
    file(GLOB _SHIMAKAZE_CRYPTOPP_ALL_SOURCES CONFIGURE_DEPENDS
        "${cryptopp_SOURCE_DIR}/*.cpp"
    )
    set(_SHIMAKAZE_CRYPTOPP_FIRST_SOURCES
        "${cryptopp_SOURCE_DIR}/cryptlib.cpp"
        "${cryptopp_SOURCE_DIR}/cpu.cpp"
        "${cryptopp_SOURCE_DIR}/integer.cpp"
    )
    set(_SHIMAKAZE_CRYPTOPP_EXCLUDED_SOURCES
        "${cryptopp_SOURCE_DIR}/bench1.cpp"
        "${cryptopp_SOURCE_DIR}/bench2.cpp"
        "${cryptopp_SOURCE_DIR}/bench3.cpp"
        "${cryptopp_SOURCE_DIR}/datatest.cpp"
        "${cryptopp_SOURCE_DIR}/dlltest.cpp"
        "${cryptopp_SOURCE_DIR}/fipsalgt.cpp"
        "${cryptopp_SOURCE_DIR}/fipstest.cpp"
        "${cryptopp_SOURCE_DIR}/pch.cpp"
        "${cryptopp_SOURCE_DIR}/regtest1.cpp"
        "${cryptopp_SOURCE_DIR}/regtest2.cpp"
        "${cryptopp_SOURCE_DIR}/regtest3.cpp"
        "${cryptopp_SOURCE_DIR}/regtest4.cpp"
        "${cryptopp_SOURCE_DIR}/test.cpp"
        "${cryptopp_SOURCE_DIR}/validat0.cpp"
        "${cryptopp_SOURCE_DIR}/validat1.cpp"
        "${cryptopp_SOURCE_DIR}/validat2.cpp"
        "${cryptopp_SOURCE_DIR}/validat3.cpp"
        "${cryptopp_SOURCE_DIR}/validat4.cpp"
        "${cryptopp_SOURCE_DIR}/validat5.cpp"
        "${cryptopp_SOURCE_DIR}/validat6.cpp"
        "${cryptopp_SOURCE_DIR}/validat7.cpp"
        "${cryptopp_SOURCE_DIR}/validat8.cpp"
        "${cryptopp_SOURCE_DIR}/validat9.cpp"
        "${cryptopp_SOURCE_DIR}/validat10.cpp"
    )
    list(REMOVE_ITEM _SHIMAKAZE_CRYPTOPP_ALL_SOURCES
        ${_SHIMAKAZE_CRYPTOPP_FIRST_SOURCES}
        ${_SHIMAKAZE_CRYPTOPP_EXCLUDED_SOURCES}
    )

    add_library(cryptopp STATIC
        ${_SHIMAKAZE_CRYPTOPP_FIRST_SOURCES}
        ${_SHIMAKAZE_CRYPTOPP_ALL_SOURCES}
    )
    add_library(CryptoPP::cryptopp ALIAS cryptopp)
    target_include_directories(cryptopp
        PUBLIC
            "$<BUILD_INTERFACE:${cryptopp_SOURCE_DIR}>"
    )
    target_compile_definitions(cryptopp
        PUBLIC
            CRYPTOPP_DISABLE_ASM
            CRYPTOPP_DISABLE_MIXED_ASM
    )
    target_compile_features(cryptopp PUBLIC cxx_std_17)
    if(MSVC)
        target_compile_definitions(cryptopp
            PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _SCL_SECURE_NO_WARNINGS
        )
    endif()
endif()

set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "Disable Snappy tests" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "Disable Snappy benchmarks" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "Disable Snappy install rules" FORCE)
FetchContent_Declare(snappy
    GIT_REPOSITORY "${SHIMAKAZE_SNAPPY_GIT_REPOSITORY}"
    GIT_TAG "${SHIMAKAZE_SNAPPY_GIT_TAG}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(snappy)
